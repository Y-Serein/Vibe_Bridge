#!/usr/bin/env python3
"""Fit a production device residual from one 0°C/50°C archive.

The family profile supplies the common thermal response.  This tool fits only
the device-specific two-point gain/bias, so the production operation remains
one low/high calibration sequence.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from prepare_tn160_radiometry_profile import fit_archive


def fit_device(family_path: Path, archive_path: Path) -> dict:
    family = json.loads(family_path.read_text(encoding="utf-8"))
    if family.get("schema") != "tn160-radiometry-family-profile-v1":
        raise ValueError("family profile schema is invalid")
    device = fit_archive(archive_path)
    family_domain = family.get("reference_domain")
    device_domain = device.get("reference_domain")
    if not family_domain or not device_domain:
        raise ValueError("family/device profile lacks reference domain")
    family_response = float(family_domain["response_adu"])
    device_response = float(device_domain["response_adu"])
    if family_response <= 100.0 or device_response <= 100.0:
        raise ValueError("family or device response is too small")
    gain_q16 = round(family_response / device_response * 65536.0)
    bias_adu = round(
        float(family_domain["low_signal_adu"])
        - gain_q16 * float(device_domain["low_signal_adu"]) / 65536.0
    )
    return {
        "schema": "tn160-radiometry-device-residual-v1",
        "family_profile": str(family_path),
        "family_id": family.get("family_id"),
        "device_identity": device["identity"],
        "calibration": device["calibration"],
        "device_residual": {
            "gain_q16": gain_q16,
            "bias_adu": bias_adu,
        },
        "validation": {
            "two_point_only": True,
            "cold_start_required": True,
            "hot_rst_required": True,
            "independent_midpoint_required": True,
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--family", type=Path, required=True)
    parser.add_argument("--archive", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    result = fit_device(args.family, args.archive)
    args.output.write_text(
        json.dumps(result, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    print(json.dumps(result["device_residual"], ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
