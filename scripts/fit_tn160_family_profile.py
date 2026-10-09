#!/usr/bin/env python3
"""Aggregate normalized TN160 radiometry profiles into a family baseline.

Production must not reuse one device's absolute KT/BT.  This tool takes
several already-fitted representative profiles, requires the same node layout
and NUC-T table, and emits robust median family nodes.  Device-specific
two-point residuals are applied later by the per-device fitter.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import statistics
from pathlib import Path


def load(path: Path) -> dict:
    profile = json.loads(path.read_text(encoding="utf-8"))
    if profile.get("schema") != "tn160-radiometry-profile-v1":
        raise ValueError(f"{path}: unsupported profile schema")
    if not profile.get("nodes"):
        raise ValueError(f"{path}: no nodes")
    if not profile.get("nuc_t", {}).get("present"):
        raise ValueError(f"{path}: NUC-T is required for a family profile")
    return profile


def fit(paths: list[Path]) -> dict:
    if len(paths) < 3:
        raise ValueError("at least three representative profiles are required")
    profiles = [load(path) for path in paths]
    node_count = len(profiles[0]["nodes"])
    for profile in profiles[1:]:
        if len(profile["nodes"]) != node_count:
            raise ValueError("profiles have different node counts")
        if profile["nuc_t"].get("sha256") != profiles[0]["nuc_t"].get("sha256"):
            raise ValueError("profiles have different NUC-T tables")
    nodes = []
    for index in range(node_count):
        entries = [profile["nodes"][index] for profile in profiles]
        nodes.append({
            "state_name": entries[0]["state_name"],
            "ntc_adu": round(statistics.median(e["ntc_adu"] for e in entries)),
            "vtemp_adu": round(statistics.median(e["vtemp_adu"] for e in entries)),
            "kt_q14": round(statistics.median(e["kt_q14"] for e in entries)),
            "bt_adu": round(statistics.median(e["bt_adu"] for e in entries)),
            "response_ratio_median": statistics.median(
                e["response_ratio"] for e in entries
            ),
            "source_profiles": len(entries),
        })
    nodes.sort(key=lambda node: (node["ntc_adu"], node["vtemp_adu"]))
    first = profiles[0]
    family_material = "|".join(
        f"{profile.get('algorithm_id')}:{profile.get('nuc_t', {}).get('sha256')}"
        for profile in profiles
    ).encode("utf-8")
    family_id = hashlib.sha256(family_material).hexdigest()[:16]
    return {
        "schema": "tn160-radiometry-family-profile-v1",
        "algorithm_id": first["algorithm_id"],
        "status": "family-baseline-awaiting-held-out-validation",
        "product_ready": False,
        "family_id": family_id,
        "source_profiles": [str(path) for path in paths],
        "state_axis": first["state_axis"],
        "nodes": nodes,
        "nuc_t": first["nuc_t"],
        "device_residual": {
            "gain_q16": 65536,
            "bias_adu": 0,
        },
        "validation": {
            "held_out_device_required": True,
            "cold_start_required": True,
            "hot_rst_required": True,
            "target_points_required": True,
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("profiles", type=Path, nargs="+")
    args = parser.parse_args()
    result = fit(args.profiles)
    args.output.write_text(
        json.dumps(result, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    print(json.dumps({
        "output": str(args.output),
        "profiles": len(args.profiles),
        "nodes": len(result["nodes"]),
        "product_ready": result["product_ready"],
    }, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
