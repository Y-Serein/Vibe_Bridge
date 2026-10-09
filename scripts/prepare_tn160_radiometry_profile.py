#!/usr/bin/env python3
"""Build an experimental TN160 KT/BT state profile from calibration archives.

This is a host-side fitter only.  It does not write Flash, build firmware, or
claim a production radiometry model.  The input archive follows the current
Scope/TN160 calibration layout: ``manifest.json`` plus full-frame mean files
named ``low_mean_u16le.bin``, ``high_mean_u16le.bin`` and, when available,
``offset_mean_u16le.bin`` / ``state_b_high_mean_u16le.bin``.

The fitted KT/BT nodes map each thermal state into the reference state's
low/high response domain.  A NUC-T table is intentionally required separately
for a product-ready profile; without it the output is marked experimental.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import struct
import zlib
from pathlib import Path
from typing import Any


FRAME_PIXELS = 160 * 120
KT_Q14_ONE = 16384
MIN_RESPONSE_ADU = 100.0
MAX_Q14 = 65535
PROFILE_SCHEMA = "tn160-radiometry-profile-v1"
ALGORITHM_ID = "tn160.kt-bt-nuct.v1"


def _finite(value: Any, name: str) -> float:
    result = float(value)
    if not math.isfinite(result):
        raise ValueError(f"{name} is not finite")
    return result


def _mean_u16(path: Path) -> float:
    data = path.read_bytes()
    if len(data) != FRAME_PIXELS * 2:
        raise ValueError(f"{path}: expected {FRAME_PIXELS * 2} bytes")
    values = struct.unpack("<" + "H" * FRAME_PIXELS, data)
    return sum(values) / len(values)


def _load_nuc_t(path: Path | None, generated: list[int] | None = None) -> tuple[dict[str, Any], list[int] | None]:
    if generated is not None:
        data = struct.pack("<" + "H" * len(generated), *generated)
        return ({
            "present": True,
            "required_for_product": True,
            "experimental_two_point": True,
            "count": len(generated),
            "bytes": len(data),
            "data_type": "uint16",
            "unit": "kelvin_x16",
            "byte_order": "little",
            "sha256": hashlib.sha256(data).hexdigest(),
            "crc32": f"{zlib.crc32(data) & 0xffffffff:08x}",
        }, generated)
    if path is None:
        return ({
            "present": False,
            "required_for_product": True,
        }, None)
    data = path.read_bytes()
    if len(data) not in (8192 * 2, 16384 * 2):
        raise ValueError("NUC-T must contain exactly 8192 or 16384 uint16 items")
    values = struct.unpack("<" + "H" * (len(data) // 2), data)
    if any(right < left for left, right in zip(values, values[1:])):
        raise ValueError("NUC-T must be monotonic non-decreasing")
    return ({
        "present": True,
        "required_for_product": True,
        "count": len(values),
        "bytes": len(data),
        "data_type": "uint16",
        "unit": "kelvin_x16",
        "byte_order": "little",
        "sha256": hashlib.sha256(data).hexdigest(),
        "crc32": f"{zlib.crc32(data) & 0xffffffff:08x}",
        "path": str(path),
    }, list(values))


def _generate_two_point_nuc_t(
    low_signal: float, high_signal: float, low_c: float, high_c: float
) -> list[int]:
    low_index = max(0, min(16383, round(low_signal)))
    high_index = max(low_index + 1, min(16383, round(high_signal)))
    low_k = low_c + 273.15
    high_k = high_c + 273.15
    low_k4 = low_k ** 4
    delta_k4 = high_k ** 4 - low_k4
    values = []
    for index in range(16384):
        if index <= low_index:
            kelvin = low_k
        elif index >= high_index:
            kelvin = high_k
        else:
            fraction = (index - low_index) / (high_index - low_index)
            kelvin = (low_k4 + fraction * delta_k4) ** 0.25
        values.append(max(0, min(0xffff, round(kelvin * 16.0))))
    return values


def _capture(manifest: dict[str, Any], archive: Path, name: str) -> dict[str, Any]:
    captures = manifest.get("captures", {})
    entry = captures.get(name)
    if not isinstance(entry, dict) or not entry.get("complete"):
        raise ValueError(f"capture {name!r} is missing or incomplete")
    after = entry.get("after")
    if not isinstance(after, dict):
        raise ValueError(f"capture {name!r} has no after-state")
    mean_path = archive / f"{name}_mean_u16le.bin"
    return {
        "name": name,
        "mean_adu": _mean_u16(mean_path),
        "anchor_adu": _finite(after["anchor"], f"{name}.anchor"),
        "ntc_adu": _finite(after["ntc_adu"], f"{name}.ntc_adu"),
        "vtemp_adu": _finite(after["vtemp_adu"], f"{name}.vtemp_adu"),
        "mean_diff": _finite(after["mean_diff"], f"{name}.mean_diff"),
        "nuc_count": int(after.get("nuc_count", 0)),
    }


def _signal(capture: dict[str, Any]) -> float:
    return capture["mean_adu"] - capture["anchor_adu"]


def _state_pair(
    manifest: dict[str, Any],
    archive: Path,
    low_name: str,
    high_name: str,
    target_low_c: float,
    target_high_c: float,
) -> dict[str, Any]:
    low = _capture(manifest, archive, low_name)
    high = _capture(manifest, archive, high_name)
    response = _signal(high) - _signal(low)
    if response < MIN_RESPONSE_ADU:
        raise ValueError(
            f"{low_name}/{high_name}: response {response:.3f} ADU is too small"
        )
    return {
        "state_name": low_name.replace("_", "-")
        + "-to-"
        + high_name.replace("_", "-"),
        "target_low_c": target_low_c,
        "target_high_c": target_high_c,
        "low": low,
        "high": high,
        "response_adu": response,
    }


def _fit_node(
    reference: dict[str, Any],
    state: dict[str, Any],
) -> dict[str, Any]:
    ref_low = _signal(reference["low"])
    ref_response = reference["response_adu"]
    state_low = _signal(state["low"])
    state_response = state["response_adu"]
    kt_q14 = round(ref_response / state_response * KT_Q14_ONE)
    if not 0 < kt_q14 <= MAX_Q14:
        raise ValueError(
            f"{state['state_name']}: KT Q14 value {kt_q14} is out of range"
        )
    bt_adu = round(ref_low - kt_q14 * state_low / KT_Q14_ONE)
    return {
        "state_name": state["state_name"],
        "ntc_adu": round(state["low"]["ntc_adu"]),
        "vtemp_adu": round(state["low"]["vtemp_adu"]),
        "kt_q14": kt_q14,
        "bt_adu": bt_adu,
        "response_adu": state_response,
        "response_ratio": state_response / ref_response,
        "source_low": state["low_name"] if "low_name" in state else state["low"]["name"],
        "source_high": state["high_name"] if "high_name" in state else state["high"]["name"],
    }


def fit_archive(
    archive: Path,
    nuc_t_path: Path | None = None,
    generate_two_point_nuc_t: bool = False,
) -> dict[str, Any]:
    manifest_path = archive / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    calibration = manifest.get("calibration", {})
    target_low = _finite(calibration.get("t_ref_low_c", 0.0), "t_ref_low_c")
    target_high = _finite(calibration.get("t_ref_high_c", 50.0), "t_ref_high_c")
    if not target_low < target_high:
        raise ValueError("calibration target temperatures are not ordered")

    reference = _state_pair(
        manifest, archive, "low", "high", target_low, target_high
    )
    reference["state_name"] = "reference"
    nodes = [{
        "state_name": "reference",
        "ntc_adu": round(reference["low"]["ntc_adu"]),
        "vtemp_adu": round(reference["low"]["vtemp_adu"]),
        "kt_q14": KT_Q14_ONE,
        "bt_adu": 0,
        "response_adu": reference["response_adu"],
        "response_ratio": 1.0,
        "source_low": "low",
        "source_high": "high",
    }]
    state_a_reference = manifest.get("state_a_reference")
    if isinstance(state_a_reference, dict):
        if "ntc_adu" in state_a_reference:
            nodes[0]["ntc_adu"] = round(
                _finite(state_a_reference["ntc_adu"], "state_a_reference.ntc_adu")
            )
        if "vtemp_adu" in state_a_reference:
            nodes[0]["vtemp_adu"] = round(
                _finite(
                    state_a_reference["vtemp_adu"],
                    "state_a_reference.vtemp_adu",
                )
            )

    # Scope ProductionCalibrationPage writes the research B-high capture as
    # ``high_b``; the standalone capture helper uses ``state_b_high``.  Accept
    # both names so the existing Scope archive is the single source of truth.
    for high_name in ("high_b", "state_b_high"):
        if (archive / "offset_mean_u16le.bin").exists() and (
            archive / f"{high_name}_mean_u16le.bin"
        ).exists():
            manifest_high_name = high_name
            if high_name == "high_b" and "high_b" not in manifest.get(
                "captures", {}
            ) and "state_b_high" in manifest.get("captures", {}):
                manifest_high_name = "state_b_high"
            state = _state_pair(
                manifest, archive, "offset", manifest_high_name,
                target_low, target_high
            )
            nodes.append(_fit_node(reference, state))
            break

    nodes.sort(key=lambda node: (node["ntc_adu"], node["vtemp_adu"]))

    generated_values = None
    if generate_two_point_nuc_t and nuc_t_path is not None:
        raise ValueError("--nuc-t and --generate-two-point-nuc-t are exclusive")
    if generate_two_point_nuc_t:
        generated_values = _generate_two_point_nuc_t(
            _signal(reference["low"]), _signal(reference["high"]),
            target_low, target_high
        )
    nuc_t, nuc_t_values = _load_nuc_t(nuc_t_path, generated_values)
    if not nuc_t["present"]:
        status = "experimental-no-nuc-t"
    elif nuc_t.get("experimental_two_point"):
        status = "experimental-two-point-nuc-t"
    elif len(nodes) < 3:
        status = "experimental-insufficient-states"
    else:
        status = "candidate-awaiting-validation"
    return {
        "schema": PROFILE_SCHEMA,
        "algorithm_id": ALGORITHM_ID,
        "status": status,
        "product_ready": False,
        "identity": {
            "board_id": manifest.get("board_id"),
            "sensor_id": manifest.get("sensor_id"),
            "firmware_sha256": manifest.get("firmware_sha256"),
        },
        "calibration": {
            "calibration_id": calibration.get("header_crc32"),
            "mean_diff_adu": calibration.get("mean_diff_adu"),
            "target_low_c": target_low,
            "target_high_c": target_high,
        },
        "state_axis": {
            "primary": "ntc_adu",
            "secondary": "vtemp_adu",
            "interpolation": "linear",
            "outside_range": "clamp",
        },
        "nodes": nodes,
        "reference_domain": {
            "low_signal_adu": _signal(reference["low"]),
            "high_signal_adu": _signal(reference["high"]),
            "response_adu": reference["response_adu"],
        },
        "device_residual": {
            "gain_q16": 65536,
            "bias_adu": 0,
        },
        "nuc_t": nuc_t,
        "validation": {
            "independent_targets_required": True,
            "cold_start_required": True,
            "hot_rst_required": True,
            "ffc_boundary_required": True,
        },
        "source_archive": str(archive),
        "_nuc_t_values": nuc_t_values,
    }


def _calibration_id(value: Any) -> int:
    if value in (None, ""):
        return 0
    text = str(value).strip().lower()
    return int(text, 16) if text.startswith(("0x",)) or any(
        ch in "abcdef" for ch in text
    ) else int(text, 10)


def write_c_profile(
    profile: dict[str, Any], nuc_t_path: Path | None, output: Path
) -> None:
    if not profile["nuc_t"].get("present"):
        raise ValueError("--output-c requires --nuc-t")
    values = profile.get("_nuc_t_values")
    if values is None:
        if nuc_t_path is None:
            raise ValueError("--output-c requires --nuc-t or generated NUC-T")
        nuc_data = nuc_t_path.read_bytes()
        values = struct.unpack("<" + "H" * (len(nuc_data) // 2), nuc_data)
    calibration_id = _calibration_id(
        profile.get("calibration", {}).get("calibration_id")
    )
    nodes = profile["nodes"]
    lines = [
        '/* Generated by prepare_tn160_radiometry_profile.py. */',
        '#include "thermal_radiometry_profile.h"',
        "",
        "static const struct thermal_radiometry_node nodes[] = {",
    ]
    for node in nodes:
        lines.append(
            "    {%d, %d, %d},"
            % (node["ntc_adu"], node["kt_q14"], node["bt_adu"])
        )
    lines.extend(["};", "", "static const uint16_t nuc_t[] = {"])
    for offset in range(0, len(values), 12):
        lines.append("    " + ", ".join(str(v) for v in values[offset:offset + 12]) + ",")
    lines.extend([
        "};",
        "",
        "bool thermal_radiometry_profile_get(",
        "    struct thermal_radiometry_profile* profile)",
        "{",
        "    if (!profile) return false;",
        "    *profile = (struct thermal_radiometry_profile){",
        f"        .calibration_id = 0x{calibration_id:08x}u,",
        "        .gain_mode = 0xffu,",
        "        .axis = THERMAL_RADIOMETRY_AXIS_NTC,",
        "        .node_count = (uint16_t)(sizeof(nodes) / sizeof(nodes[0])),",
        "        .nodes = nodes,",
        "        .nuc_t_count = (uint16_t)(sizeof(nuc_t) / sizeof(nuc_t[0])),",
        "        .nuc_t_kelvin_x16 = nuc_t,",
        f"        .device_gain_q16 = {int(profile['device_residual']['gain_q16'])}u,",
        f"        .device_bias_adu = {int(profile['device_residual']['bias_adu'])},",
        "    };",
        "    return true;",
        "}",
        "",
    ])
    output.write_text("\n".join(lines), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--nuc-t",
        type=Path,
        help="optional little-endian uint16 NUC-T table; required for product-ready status",
    )
    parser.add_argument(
        "--output-c",
        type=Path,
        help="optional generated C profile source; requires --nuc-t",
    )
    parser.add_argument(
        "--generate-two-point-nuc-t",
        action="store_true",
        help="generate an explicitly experimental 0/50 C NUC-T table",
    )
    args = parser.parse_args()
    profile = fit_archive(
        args.archive, args.nuc_t, args.generate_two_point_nuc_t
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    json_profile = {key: value for key, value in profile.items()
                    if not key.startswith("_")}
    args.output.write_text(
        json.dumps(json_profile, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    if args.output_c is not None:
        write_c_profile(profile, args.nuc_t, args.output_c)
    print(json.dumps({
        "output": str(args.output),
        "status": profile["status"],
        "nodes": len(profile["nodes"]),
        "product_ready": profile["product_ready"],
    }, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
