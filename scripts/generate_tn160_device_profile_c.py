#!/usr/bin/env python3
"""Generate the compile-time C profile from a family profile and device residual."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from prepare_tn160_radiometry_profile import write_c_profile


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--family", type=Path, required=True)
    parser.add_argument("--device-residual", type=Path, required=True)
    parser.add_argument("--nuc-t", type=Path, required=True)
    parser.add_argument("--output-c", type=Path, required=True)
    args = parser.parse_args()
    profile = json.loads(args.family.read_text(encoding="utf-8"))
    residual = json.loads(
        args.device_residual.read_text(encoding="utf-8")
    ).get("device_residual")
    if not isinstance(residual, dict):
        raise ValueError("device residual JSON is missing device_residual")
    if residual.get("family_id") not in (None, profile.get("family_id")):
        raise ValueError("device residual does not match family profile")
    profile["device_residual"] = {
        "gain_q16": int(residual["gain_q16"]),
        "bias_adu": int(residual["bias_adu"]),
    }
    profile["nuc_t"] = dict(profile.get("nuc_t", {}))
    profile["nuc_t"]["present"] = True
    write_c_profile(profile, args.nuc_t, args.output_c)
    print(json.dumps({
        "output_c": str(args.output_c),
        "gain_q16": profile["device_residual"]["gain_q16"],
        "bias_adu": profile["device_residual"]["bias_adu"],
    }, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
