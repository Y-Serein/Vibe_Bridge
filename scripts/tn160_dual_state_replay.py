#!/usr/bin/env python3
"""Replay a TN160 two-state factory fit against a later fixed target window.

This tool deliberately evaluates only the global response visible at one
reference temperature.  A single fixed target cannot separate anchor drift
from mean-diff drift; the report makes that limitation explicit.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import statistics
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable, Sequence


REQUIRED_COLUMNS = {
    "t_s",
    "center_roi_adu",
    "mean_diff",
    "ntc_adu",
    "nuc_decision",
    "nuc_count",
    "nuc_dg",
}


@dataclass(frozen=True)
class Window:
    start_s: float
    end_s: float

    def contains(self, value: float) -> bool:
        return self.start_s <= value <= self.end_s


@dataclass(frozen=True)
class ReplaySample:
    t_s: float
    ntc_adu: float
    ntc_c: float | None
    center_adu: float
    mean_diff: float
    shutter_delta_adu: float
    required_anchor_adu: float


@dataclass(frozen=True)
class ReplayMetrics:
    samples: int
    predicted_start_c: float
    predicted_end_c: float
    predicted_mean_c: float
    predicted_min_c: float
    predicted_max_c: float
    peak_to_peak_c: float
    endpoint_drift_c: float
    maximum_absolute_error_c: float
    absolute_error_pass: bool
    peak_to_peak_pass: bool
    endpoint_drift_pass: bool


@dataclass(frozen=True)
class ReplayReport:
    csv_path: str
    target_c: float
    calibration_low_c: float
    calibration_high_c: float
    state_a_s: tuple[float, float]
    state_b_s: tuple[float, float]
    verify_s: tuple[float, float]
    logger_starts_by_5s: bool
    state_a_ntc_adu: float
    state_b_ntc_adu: float
    state_a_ntc_c: float | None
    state_b_ntc_c: float | None
    fitted_ntc_coefficient_adu_per_code: float
    fixed_shutter_coefficient: float
    fitted_intercept_adu: float
    observable: str
    dynamic_mean_diff_identifiable: bool
    metrics: ReplayMetrics


def parse_window(value: str) -> Window:
    try:
        start_text, end_text = value.split(":", 1)
        window = Window(float(start_text), float(end_text))
    except (TypeError, ValueError) as exc:
        raise argparse.ArgumentTypeError(
            "窗口必须使用 start:end，例如 10:20"
        ) from exc
    if not math.isfinite(window.start_s) or not math.isfinite(window.end_s):
        raise argparse.ArgumentTypeError("窗口必须是有限数值")
    if window.start_s < 0.0 or window.end_s <= window.start_s:
        raise argparse.ArgumentTypeError("窗口必须满足 0 <= start < end")
    return window


def _float(row: dict[str, str], name: str) -> float:
    value = float(row[name])
    if not math.isfinite(value):
        raise ValueError(f"{name} 不是有限数值")
    return value


def target_fraction(target_c: float, low_c: float, high_c: float) -> float:
    if not low_c < high_c:
        raise ValueError("标定低温必须小于高温")
    target_k = target_c + 273.15
    low_k = low_c + 273.15
    high_k = high_c + 273.15
    if min(target_k, low_k, high_k) <= 0.0:
        raise ValueError("温度必须高于绝对零度")
    return (target_k**4 - low_k**4) / (high_k**4 - low_k**4)


def temperature_from_adu(
    corrected_adu: float,
    effective_anchor: float,
    mean_diff: float,
    low_c: float,
    high_c: float,
) -> float:
    if not math.isfinite(mean_diff) or mean_diff <= 0.0:
        raise ValueError("mean_diff 必须为正有限数值")
    low_k = low_c + 273.15
    high_k = high_c + 273.15
    fraction = (corrected_adu - effective_anchor) / mean_diff
    temperature_k4 = low_k**4 + fraction * (high_k**4 - low_k**4)
    if temperature_k4 <= 0.0:
        raise ValueError("T^4 回放结果无效")
    return temperature_k4**0.25 - 273.15


def load_samples(
    csv_path: Path,
    target_c: float,
    low_c: float,
    high_c: float,
) -> list[ReplaySample]:
    fraction = target_fraction(target_c, low_c, high_c)
    samples: list[ReplaySample] = []
    shutter_delta = 0.0
    last_nuc_count: int | None = None

    with csv_path.open("r", encoding="utf-8-sig", newline="") as stream:
        reader = csv.DictReader(stream)
        columns = set(reader.fieldnames or [])
        missing = REQUIRED_COLUMNS - columns
        if missing:
            raise ValueError(f"CSV 缺少字段：{', '.join(sorted(missing))}")

        for line_number, row in enumerate(reader, start=2):
            try:
                t_s = _float(row, "t_s")
                center_adu = _float(row, "center_roi_adu")
                mean_diff = _float(row, "mean_diff")
                ntc_adu = _float(row, "ntc_adu")
                nuc_count = int(_float(row, "nuc_count"))
                nuc_decision = int(_float(row, "nuc_decision"))
                nuc_dg = _float(row, "nuc_dg")
            except (KeyError, TypeError, ValueError) as exc:
                raise ValueError(f"CSV 第 {line_number} 行无法解析：{exc}") from exc

            if mean_diff <= 0.0 or mean_diff > 16383.0:
                continue
            if not 0.0 < ntc_adu < 4095.0:
                continue

            if last_nuc_count is None:
                last_nuc_count = nuc_count
            elif nuc_count < last_nuc_count:
                raise ValueError(
                    f"CSV 第 {line_number} 行 NUC count 回退，疑似包含复位"
                )
            elif nuc_count > last_nuc_count:
                if nuc_decision == 0 and nuc_count > 1:
                    # count=1 is relative to the factory low frame.  The
                    # factory fit needs shutter movement after that first
                    # accepted FFC, so do not fold its absolute offset in.
                    shutter_delta += nuc_dg
                last_nuc_count = nuc_count

            ntc_c: float | None = None
            ntc_c_text = row.get("ntc_c", "")
            if ntc_c_text:
                try:
                    candidate = float(ntc_c_text)
                    if math.isfinite(candidate):
                        ntc_c = candidate
                except ValueError:
                    pass

            samples.append(
                ReplaySample(
                    t_s=t_s,
                    ntc_adu=ntc_adu,
                    ntc_c=ntc_c,
                    center_adu=center_adu,
                    mean_diff=mean_diff,
                    shutter_delta_adu=shutter_delta,
                    required_anchor_adu=center_adu - mean_diff * fraction,
                )
            )

    if not samples:
        raise ValueError("CSV 没有可用样本")
    return samples


def _median(samples: Sequence[ReplaySample], field: str) -> float | None:
    values = [getattr(sample, field) for sample in samples]
    finite = [float(value) for value in values if value is not None and math.isfinite(value)]
    return statistics.median(finite) if finite else None


def fit_two_state(
    samples: Sequence[ReplaySample],
    state_a: Window,
    state_b: Window,
    shutter_coefficient: float,
) -> tuple[float, float, list[ReplaySample], list[ReplaySample]]:
    a_samples = [sample for sample in samples if state_a.contains(sample.t_s)]
    b_samples = [sample for sample in samples if state_b.contains(sample.t_s)]
    if len(a_samples) < 3 or len(b_samples) < 3:
        raise ValueError("状态A或状态B少于3个有效样本")

    fit_samples = a_samples + b_samples
    xs = [sample.ntc_adu for sample in fit_samples]
    ys = [
        sample.required_anchor_adu
        - shutter_coefficient * sample.shutter_delta_adu
        for sample in fit_samples
    ]
    x_mean = statistics.mean(xs)
    y_mean = statistics.mean(ys)
    denominator = sum((value - x_mean) ** 2 for value in xs)
    if denominator <= 0.0:
        raise ValueError("状态A/B的NTC没有变化，无法拟合个体系数")
    ntc_coefficient = sum(
        (x_value - x_mean) * (y_value - y_mean)
        for x_value, y_value in zip(xs, ys)
    ) / denominator
    intercept = y_mean - ntc_coefficient * x_mean
    return intercept, ntc_coefficient, a_samples, b_samples


def evaluate(
    samples: Sequence[ReplaySample],
    verify: Window,
    intercept: float,
    ntc_coefficient: float,
    shutter_coefficient: float,
    target_c: float,
    low_c: float,
    high_c: float,
    endpoint_window_s: float,
    absolute_error_gate_c: float,
    peak_to_peak_gate_c: float,
    endpoint_drift_gate_c: float,
) -> ReplayMetrics:
    selected = [sample for sample in samples if verify.contains(sample.t_s)]
    if len(selected) < 3:
        raise ValueError("验证窗口少于3个有效样本")

    predicted: list[tuple[float, float]] = []
    for sample in selected:
        anchor = (
            intercept
            + ntc_coefficient * sample.ntc_adu
            + shutter_coefficient * sample.shutter_delta_adu
        )
        predicted.append(
            (
                sample.t_s,
                temperature_from_adu(
                    sample.center_adu,
                    anchor,
                    sample.mean_diff,
                    low_c,
                    high_c,
                ),
            )
        )

    start_limit = verify.start_s + endpoint_window_s
    end_limit = verify.end_s - endpoint_window_s
    start_values = [value for t_s, value in predicted if t_s <= start_limit]
    end_values = [value for t_s, value in predicted if t_s >= end_limit]
    if not start_values or not end_values:
        raise ValueError("端点统计窗口没有样本")

    values = [value for _, value in predicted]
    start_c = statistics.median(start_values)
    end_c = statistics.median(end_values)
    peak_to_peak = max(values) - min(values)
    endpoint_drift = abs(end_c - start_c)
    maximum_absolute_error = max(abs(value - target_c) for value in values)
    return ReplayMetrics(
        samples=len(values),
        predicted_start_c=start_c,
        predicted_end_c=end_c,
        predicted_mean_c=statistics.mean(values),
        predicted_min_c=min(values),
        predicted_max_c=max(values),
        peak_to_peak_c=peak_to_peak,
        endpoint_drift_c=endpoint_drift,
        maximum_absolute_error_c=maximum_absolute_error,
        absolute_error_pass=maximum_absolute_error <= absolute_error_gate_c,
        peak_to_peak_pass=peak_to_peak <= peak_to_peak_gate_c,
        endpoint_drift_pass=endpoint_drift <= endpoint_drift_gate_c,
    )


def build_report(
    csv_path: Path,
    target_c: float,
    low_c: float,
    high_c: float,
    state_a: Window,
    state_b: Window,
    verify: Window,
    shutter_coefficient: float,
    endpoint_window_s: float = 10.0,
    absolute_error_gate_c: float = 1.0,
    peak_to_peak_gate_c: float = 1.5,
    endpoint_drift_gate_c: float = 0.5,
) -> ReplayReport:
    samples = load_samples(csv_path, target_c, low_c, high_c)
    intercept, ntc_coefficient, a_samples, b_samples = fit_two_state(
        samples, state_a, state_b, shutter_coefficient
    )
    metrics = evaluate(
        samples,
        verify,
        intercept,
        ntc_coefficient,
        shutter_coefficient,
        target_c,
        low_c,
        high_c,
        endpoint_window_s,
        absolute_error_gate_c,
        peak_to_peak_gate_c,
        endpoint_drift_gate_c,
    )
    return ReplayReport(
        csv_path=str(csv_path),
        target_c=target_c,
        calibration_low_c=low_c,
        calibration_high_c=high_c,
        state_a_s=(state_a.start_s, state_a.end_s),
        state_b_s=(state_b.start_s, state_b.end_s),
        verify_s=(verify.start_s, verify.end_s),
        logger_starts_by_5s=min(sample.t_s for sample in samples) <= 5.0,
        state_a_ntc_adu=float(_median(a_samples, "ntc_adu")),
        state_b_ntc_adu=float(_median(b_samples, "ntc_adu")),
        state_a_ntc_c=_median(a_samples, "ntc_c"),
        state_b_ntc_c=_median(b_samples, "ntc_c"),
        fitted_ntc_coefficient_adu_per_code=ntc_coefficient,
        fixed_shutter_coefficient=shutter_coefficient,
        fitted_intercept_adu=intercept,
        observable="anchor_plus_target_fraction_times_mean_diff",
        dynamic_mean_diff_identifiable=False,
        metrics=metrics,
    )


def _rounded(value: object) -> object:
    if isinstance(value, float):
        return round(value, 6)
    if isinstance(value, dict):
        return {key: _rounded(item) for key, item in value.items()}
    if isinstance(value, list):
        return [_rounded(item) for item in value]
    if isinstance(value, tuple):
        return [_rounded(item) for item in value]
    return value


def format_text(report: ReplayReport) -> str:
    metrics = report.metrics
    overall = (
        metrics.absolute_error_pass
        and metrics.peak_to_peak_pass
        and metrics.endpoint_drift_pass
        and report.logger_starts_by_5s
        and report.dynamic_mean_diff_identifiable
    )
    return "\n".join(
        [
            f"CSV: {report.csv_path}",
            f"target: {report.target_c:.3f} °C",
            (
                "fit: required_anchor = "
                f"{report.fitted_intercept_adu:.6f} + "
                f"{report.fitted_ntc_coefficient_adu_per_code:.6f} * NTC + "
                f"{report.fixed_shutter_coefficient:.6f} * cumulative_NUC_dG"
            ),
            (
                "state NTC: "
                f"A={report.state_a_ntc_adu:.3f} ADU, "
                f"B={report.state_b_ntc_adu:.3f} ADU"
            ),
            (
                "prediction: "
                f"start={metrics.predicted_start_c:.3f} °C, "
                f"end={metrics.predicted_end_c:.3f} °C, "
                f"mean={metrics.predicted_mean_c:.3f} °C, "
                f"range=[{metrics.predicted_min_c:.3f}, "
                f"{metrics.predicted_max_c:.3f}] °C"
            ),
            (
                "gates: "
                f"max_abs={metrics.maximum_absolute_error_c:.3f} °C "
                f"({'PASS' if metrics.absolute_error_pass else 'FAIL'}), "
                f"p2p={metrics.peak_to_peak_c:.3f} °C "
                f"({'PASS' if metrics.peak_to_peak_pass else 'FAIL'}), "
                f"endpoint={metrics.endpoint_drift_c:.3f} °C "
                f"({'PASS' if metrics.endpoint_drift_pass else 'FAIL'})"
            ),
            (
                "identifiability: FAIL — one fixed target observes only "
                "anchor + target_fraction * mean_diff; dynamic mean_diff is not separable"
            ),
            f"overall model gate: {'PASS' if overall else 'FAIL'}",
        ]
    )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="TN160 双状态热漂移离线回放（只读CSV）"
    )
    parser.add_argument("csv_path", type=Path)
    parser.add_argument("--target-c", type=float, required=True)
    parser.add_argument("--state-a", type=parse_window, required=True)
    parser.add_argument("--state-b", type=parse_window, required=True)
    parser.add_argument("--verify", type=parse_window, required=True)
    parser.add_argument("--k-shutter", type=float, default=1.0)
    parser.add_argument("--cal-low-c", type=float, default=0.0)
    parser.add_argument("--cal-high-c", type=float, default=50.0)
    parser.add_argument("--endpoint-window-s", type=float, default=10.0)
    parser.add_argument("--json", action="store_true")
    return parser


def main(argv: Iterable[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        report = build_report(
            csv_path=args.csv_path,
            target_c=args.target_c,
            low_c=args.cal_low_c,
            high_c=args.cal_high_c,
            state_a=args.state_a,
            state_b=args.state_b,
            verify=args.verify,
            shutter_coefficient=args.k_shutter,
            endpoint_window_s=args.endpoint_window_s,
        )
    except (OSError, ValueError) as exc:
        print(f"ERROR: {exc}")
        return 2
    if args.json:
        print(json.dumps(_rounded(asdict(report)), ensure_ascii=False, indent=2))
    else:
        print(format_text(report))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
