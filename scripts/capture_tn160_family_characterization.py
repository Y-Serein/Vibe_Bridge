#!/usr/bin/env python3
"""Capture one TN160 research Family-profile characterization archive.

The operator performs four read-only target captures on one device:
  A1: 0 C, A2: 50 C, then raise the device/body temperature,
  B1: 50 C, B2: 0 C.

This is an R&D characterization flow, not the production flow.  It reads
/frame.adc16 and /api/v1/diagnostics only; it never writes calibration or
thermal-model Flash.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import struct
import time
import urllib.request
import zlib
from datetime import datetime, timezone
from pathlib import Path


WIDTH = 160
HEIGHT = 120
PIXELS = WIDTH * HEIGHT
PAYLOAD_BYTES = PIXELS * 2
META_BYTES = 120
DEFAULT_BASE = "http://192.168.17.1"
DEFAULT_FRAMES = 32

STATE_INSTRUCTIONS = {
    "A1": "把目标稳定在 0°C，设备处于初始冷状态。",
    "A2": "把目标稳定在 50°C，保持当前初始热状态。",
    "B1": "保持 50°C，同时让设备/机芯升温到明显不同的热状态。",
    "B2": "切回并稳定在 0°C，保持 B1 后的热状态。",
}


def get_json(opener, base, path):
    with opener.open(base.rstrip("/") + path, timeout=5) as response:
        return json.load(response)


def get_bytes(opener, base, path):
    with opener.open(base.rstrip("/") + path, timeout=10) as response:
        return response.read()


def parse_frame(record: bytes):
    if len(record) != PAYLOAD_BYTES + META_BYTES:
        raise ValueError(f"adc16 record size {len(record)} is invalid")
    payload = record[:PAYLOAD_BYTES]
    meta = record[PAYLOAD_BYTES:]
    if meta[:8] != b"SITHMETA":
        raise ValueError("adc16 metadata magic is invalid")
    if struct.unpack_from("<H", meta, 10)[0] != META_BYTES:
        raise ValueError("adc16 metadata size is invalid")
    if zlib.crc32(payload) != struct.unpack_from("<I", meta, 32)[0]:
        raise ValueError("adc16 payload CRC is invalid")
    if zlib.crc32(meta[:116]) != struct.unpack_from("<I", meta, 116)[0]:
        raise ValueError("adc16 metadata CRC is invalid")
    values = struct.unpack("<" + "H" * PIXELS, payload)
    center = []
    for y in range(HEIGHT // 2 - 2, HEIGHT // 2 + 3):
        for x in range(WIDTH // 2 - 2, WIDTH // 2 + 3):
            center.append(values[y * WIDTH + x])
    return values, {
        "frame_number": struct.unpack_from("<I", meta, 16)[0],
        "timestamp_ms": struct.unpack_from("<I", meta, 24)[0],
        "calibration_id": struct.unpack_from("<I", meta, 92)[0],
        "nuc_table_index": struct.unpack_from("<H", meta, 96)[0],
        "ffc_count": struct.unpack_from("<H", meta, 98)[0],
        "ffc_state": meta[100],
        "shutter_state": meta[101],
        "global_mean_adu": sum(values) / len(values),
        "center_roi_adu": sum(center) / len(center),
    }


def read_frame_with_diagnostics(opener, base):
    before = get_json(opener, base, "/api/v1/diagnostics")
    values, meta = parse_frame(get_bytes(opener, base, "/frame.adc16"))
    after = get_json(opener, base, "/api/v1/diagnostics")
    row = {
        "host_timestamp_utc": datetime.now(timezone.utc).isoformat(),
        **meta,
        "diagnostics_before_frame": before,
        "diagnostics_after_frame": after,
    }
    for key in (
        "ntc_adu", "ntc_c", "vtemp_adu", "smooth_vtemp_adu",
        "mean_diff", "anchor_adu", "effective_anchor_adu",
        "nuc_count", "nuc_dg", "nuc_dv", "nuc_dn", "ffc_state",
        "shutter_state", "thermal_radiometry_active",
    ):
        if key in after:
            row[key] = after[key]
    return values, row


def capture_state(opener, base, state, output, frames):
    print(f"开始 {state}: {STATE_INSTRUCTIONS[state]}", flush=True)
    input("目标稳定后按 Enter 开始采集；Ctrl+C 可停止：")
    output.mkdir(parents=True, exist_ok=True)
    sums = [0] * PIXELS
    rows = []
    for index in range(frames):
        values, row = read_frame_with_diagnostics(opener, base)
        for pixel, value in enumerate(values):
            sums[pixel] += value
        row["accepted"] = int(
            row.get("ffc_state", 255) == 0
            and row.get("shutter_state", 255) == 0
            and row.get("nuc_count", 0) >= 1
        )
        row["frame_index"] = index
        rows.append(row)
        print(
            f"{state} {index + 1}/{frames}: "
            f"NTC={row.get('ntc_adu', 'NA')} "
            f"VTEMP={row.get('vtemp_adu', 'NA')} "
            f"FFC={row.get('ffc_count', 'NA')} "
            f"mean={row['global_mean_adu']:.1f}",
            flush=True,
        )
    mean_values = [round(value / frames) for value in sums]
    (output / f"{state.lower()}_mean_u16le.bin").write_bytes(
        struct.pack("<" + "H" * PIXELS, *mean_values)
    )
    with (output / f"{state.lower()}_acceptance.csv").open(
        "w", newline="", encoding="utf-8"
    ) as stream:
        fields = sorted({key for row in rows for key in row
                         if key not in ("diagnostics_before_frame",
                                        "diagnostics_after_frame")})
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows({key: row.get(key, "") for key in fields}
                         for row in rows)
    with (output / f"{state.lower()}_diagnostics.json").open(
        "w", encoding="utf-8"
    ) as stream:
        json.dump(rows, stream, ensure_ascii=False, indent=2)
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-url", default=DEFAULT_BASE)
    parser.add_argument("--frames", type=int, default=DEFAULT_FRAMES)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.frames < 8 or args.frames > 128:
        raise SystemExit("--frames must be between 8 and 128")
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    device = get_json(opener, args.base_url, "/api/v1/device")
    diagnostics = get_json(opener, args.base_url, "/api/v1/diagnostics")
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    root = args.output or Path("out/thermal_family") / stamp
    root.mkdir(parents=True, exist_ok=True)
    manifest = {
        "schema": "tn160-family-capture-v1",
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "base_url": args.base_url,
        "frames_per_state": args.frames,
        "device": device.get("device", {}),
        "initial_diagnostics": diagnostics,
        "states": {},
        "captures": {},
        "calibration": {
            "t_ref_low_c": 0.0,
            "t_ref_high_c": 50.0,
            "header_crc32": diagnostics.get("calibration_id"),
            "mean_diff_adu": diagnostics.get("mean_diff"),
        },
        "read_only": True,
    }
    root_alias = {
        "A1": "low",
        "A2": "high",
        "B1": "state_b_high",
        "B2": "offset",
    }
    for state in ("A1", "A2", "B1", "B2"):
        state_dir = root / state.lower()
        rows = capture_state(opener, args.base_url, state,
                             state_dir, args.frames)
        alias = root_alias[state]
        mean_source = state_dir / f"{state.lower()}_mean_u16le.bin"
        (root / f"{alias}_mean_u16le.bin").write_bytes(
            mean_source.read_bytes()
        )
        last = rows[-1]
        manifest["states"][state] = {
            "rows": len(rows),
            "accepted": sum(row["accepted"] for row in rows),
            "ntc_min": min(row.get("ntc_adu", 0) for row in rows),
            "ntc_max": max(row.get("ntc_adu", 0) for row in rows),
            "vtemp_min": min(row.get("vtemp_adu", 0) for row in rows),
            "vtemp_max": max(row.get("vtemp_adu", 0) for row in rows),
        }
        manifest["captures"][alias] = {
            "complete": True,
            "after": {
                "anchor": last.get("anchor_adu", 0),
                "ntc_adu": last.get("ntc_adu", 0),
                "vtemp_adu": last.get("vtemp_adu", 0),
                "mean_diff": last.get("mean_diff", 0),
                "nuc_count": last.get("nuc_count", 0),
            },
        }
    manifest["state_a_reference"] = manifest["captures"]["low"]["after"]
    (root / "manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    print(f"Family characterization archive: {root}")


if __name__ == "__main__":
    main()
