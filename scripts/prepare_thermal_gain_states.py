#!/usr/bin/env python3
"""Build two TN160 raw 0°C/50°C state pairs for the PWL model fitter.

One fixed 50°C trace supplies H(N) continuously.  Each state needs only one
short 0°C capture; H(N) and the contemporaneous FFC anchor are interpolated
from the high trace at the same NTC.  The resulting CSV is inspectable:
ntc_adu,low_adu,high_adu,anchor_adu.
"""

import argparse
import csv
import statistics
from pathlib import Path


HYSTERESIS_DIRECTION_WINDOW_S = 3.0
HYSTERESIS_MIN_NTC_DELTA_ADU = 4
HYSTERESIS_MAX_C = 0.5
HYSTERESIS_MIN_COMMON_NTC = 3
T_REF_LOW_C = 0.0
T_REF_HIGH_C = 50.0


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--high-trace", required=True, type=Path)
    parser.add_argument("--state-a-low", required=True, type=Path)
    parser.add_argument("--state-b-low", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--tail-seconds", type=float, default=8.0,
                        help="use the stable tail of each capture")
    return parser.parse_args()


def number(row, field):
    value = row.get(field, "")
    if value in (None, ""):
        raise ValueError(f"missing {field}")
    return float(value)


def elapsed(row):
    if row.get("t_s", "") not in (None, ""):
        return number(row, "t_s")
    return number(row, "elapsed_s")


def stable_tail(path, tail_seconds):
    rows = []
    with path.open(encoding="utf-8-sig", newline="") as source:
        for row in csv.DictReader(source):
            try:
                sample = (
                    elapsed(row),
                    number(row, "ntc_adu"),
                    number(row, "center_roi_adu"),
                    number(row, "anchor_adu"),
                    int(round(number(row, "ffc_state"))),
                )
            except ValueError:
                continue
            if 8 < sample[1] < 4088 and sample[4] == 0:
                rows.append(sample)
    if len(rows) < 5:
        raise ValueError(f"{path}: fewer than five idle diagnostics rows")
    end = max(row[0] for row in rows)
    tail = [row for row in rows if row[0] >= end - tail_seconds]
    if len(tail) < 5:
        raise ValueError(f"{path}: stable tail has fewer than five rows")
    return {
        "ntc": statistics.median(row[1] for row in tail),
        "adu": statistics.median(row[2] for row in tail),
        "anchor": statistics.median(row[3] for row in tail),
        "samples": len(tail),
    }


def high_target_delta_adu(mean_diff, delta_c=HYSTERESIS_MAX_C):
    low_k = T_REF_LOW_C + 273.15
    high_k = T_REF_HIGH_C + 273.15
    threshold_k = high_k - delta_c
    fraction = ((high_k ** 4 - threshold_k ** 4)
                / (high_k ** 4 - low_k ** 4))
    return mean_diff * fraction


def validate_no_path_hysteresis(samples):
    """Reject a high trace when the same absolute NTC needs two responses.

    Each sample is (elapsed_s, ntc_adu, signal_adu, mean_diff).  Direction is
    measured across a centred time window so one-code ADC jitter is not
    mistaken for a warming/cooling branch.  Three common NTC codes must exceed
    the 0.5 C-equivalent signal difference before the trace is rejected.
    """
    samples = sorted(samples)
    if not samples:
        return
    mean_diffs = [sample[3] for sample in samples]
    if max(mean_diffs) - min(mean_diffs) > 0.5:
        raise ValueError("high trace mixes different V1 mean_diff values")
    by_direction = {-1: {}, 1: {}}
    for index, sample in enumerate(samples):
        elapsed, ntc, signal, _ = sample
        left = index - 1
        while left >= 0 and elapsed - samples[left][0] \
                < HYSTERESIS_DIRECTION_WINDOW_S:
            left -= 1
        right = index + 1
        while right < len(samples) and samples[right][0] - elapsed \
                < HYSTERESIS_DIRECTION_WINDOW_S:
            right += 1
        if left < 0 or right >= len(samples):
            continue
        delta_ntc = samples[right][1] - samples[left][1]
        if abs(delta_ntc) < HYSTERESIS_MIN_NTC_DELTA_ADU:
            continue
        direction = 1 if delta_ntc > 0 else -1
        by_direction[direction].setdefault(ntc, []).append(signal)

    common_ntc = sorted(set(by_direction[-1]) & set(by_direction[1]))
    if not common_ntc:
        return
    mean_diff = statistics.median(mean_diffs)
    limit_adu = high_target_delta_adu(mean_diff)
    violations = []
    for ntc in common_ntc:
        warming = statistics.median(by_direction[-1][ntc])
        cooling = statistics.median(by_direction[1][ntc])
        difference = abs(warming - cooling)
        if difference > limit_adu:
            violations.append((ntc, difference))
    if len(violations) >= HYSTERESIS_MIN_COMMON_NTC:
        details = ", ".join(
            f"N={ntc}: {difference:.1f} ADU"
            for ntc, difference in violations[:5]
        )
        raise ValueError(
            "high trace has warming/cooling path hysteresis above "
            f"{HYSTERESIS_MAX_C:.1f} C at {len(violations)} NTC codes "
            f"({details})"
        )


def high_trace_points(path):
    by_ntc = {}
    path_samples = []
    with path.open(encoding="utf-8-sig", newline="") as source:
        for row in csv.DictReader(source):
            try:
                sample_elapsed = elapsed(row)
                ntc = int(round(number(row, "ntc_adu")))
                adu = number(row, "center_roi_adu")
                anchor = number(row, "anchor_adu")
                mean_diff = number(row, "mean_diff")
                ffc_state = int(round(number(row, "ffc_state")))
            except ValueError:
                continue
            if (8 < ntc < 4088 and mean_diff > 100.0
                    and ffc_state == 0):
                signal = adu - anchor
                by_ntc.setdefault(ntc, []).append((signal, anchor))
                path_samples.append(
                    (sample_elapsed, ntc, signal, mean_diff)
                )
    validate_no_path_hysteresis(path_samples)
    points = [
        (ntc,
         statistics.median(value[0] for value in values)
             + statistics.median(value[1] for value in values),
         statistics.median(value[1] for value in values))
        for ntc, values in by_ntc.items()
    ]
    points.sort()
    if len(points) < 8:
        raise ValueError(f"{path}: fewer than eight idle high-trace NTC points")
    return points


def interpolate(points, ntc, value_index):
    if ntc < points[0][0] or ntc > points[-1][0]:
        raise ValueError(
            f"NTC {ntc:.1f} is outside high trace "
            f"[{points[0][0]}, {points[-1][0]}]"
        )
    for left, right in zip(points, points[1:]):
        if left[0] <= ntc <= right[0]:
            if right[0] == left[0]:
                return left[value_index]
            fraction = (ntc - left[0]) / (right[0] - left[0])
            return left[value_index] + fraction * (right[value_index]
                                                   - left[value_index])
    return points[-1][value_index]


def make_state(name, low, high_points):
    high_adu = interpolate(high_points, low["ntc"], 1)
    high_anchor = interpolate(high_points, low["ntc"], 2)
    low_signal = low["adu"] - low["anchor"]
    high_signal = high_adu - high_anchor
    response = high_signal - low_signal
    if response <= 100.0:
        raise ValueError(f"{name}: interpolated 0°C/50°C response is only {response:.1f} ADU")
    return {
        "ntc_adu": round(low["ntc"]),
        # Express both endpoints in the high-trace FFC coordinate system.
        # Subtracting each capture's own anchor first prevents an intervening
        # FFC from being misidentified as a gain change.
        "low_adu": low_signal + high_anchor,
        "high_adu": high_adu,
        "anchor_adu": high_anchor,
        "response_adu": response,
    }


def main():
    args = parse_args()
    if args.tail_seconds <= 0.0:
        raise SystemExit("tail seconds must be positive")
    high_points = high_trace_points(args.high_trace)
    state_a = make_state(
        "state A", stable_tail(args.state_a_low, args.tail_seconds),
        high_points,
    )
    state_b = make_state(
        "state B", stable_tail(args.state_b_low, args.tail_seconds),
        high_points,
    )
    states = sorted((state_a, state_b), key=lambda item: item["ntc_adu"])
    if states[1]["ntc_adu"] <= states[0]["ntc_adu"] + 7:
        raise SystemExit("the two thermal states are not separated by 8 NTC ADU")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="utf-8", newline="") as output:
        writer = csv.DictWriter(
            output,
            fieldnames=("ntc_adu", "low_adu", "high_adu", "anchor_adu"),
        )
        writer.writeheader()
        for state in states:
            writer.writerow({key: state[key] for key in writer.fieldnames})

    for label, state in zip(("A", "B"), states):
        print(
            f"state {label}: NTC={state['ntc_adu']} ADU, "
            f"low={state['low_adu']:.3f}, high={state['high_adu']:.3f}, "
            f"anchor={state['anchor_adu']:.3f}, "
            f"response={state['response_adu']:.3f}"
        )
    print(f"Gain states: {args.output}")


if __name__ == "__main__":
    main()
