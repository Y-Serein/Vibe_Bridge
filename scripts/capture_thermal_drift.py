#!/usr/bin/env python3
"""Continuously capture TN160 thermal-state diagnostics to a local CSV."""

import argparse
import csv
import json
import time
import urllib.error
import urllib.request
from datetime import datetime, timezone
from pathlib import Path


DEFAULT_BASE_URL = "http://192.168.17.1"
SAMPLE_INTERVAL_SECONDS = 0.5
REQUIRED_FIELDS = (
    "timestamp_ms",
    "frame_number",
    "center_roi_x10",
    "center_roi_adu",
    "center_raw_x10",
    "center_raw_adu",
    "ntc_adu",
    "vtemp_adu",
    "smooth_vtemp_adu",
    "row_ref_mean_adu",
    "row_ref_span_adu",
    "shutter_mean_adu",
    "mean_diff",
    "anchor_adu",
    "effective_anchor_adu",
    "warmup_comp_adu",
    "normal_comp_adu",
    "total_comp_adu",
    "nuc_count",
    "ffc_state",
    "shutter_state",
    "nuc_dg",
    "nuc_dv",
    "nuc_dn",
    "thermal_row_ref_model_valid",
    "thermal_row_ref_model_active",
    "thermal_row_ref_sample_valid",
    "thermal_row_ref_model_reference_adu",
    "thermal_row_ref_model_k_q8",
    "thermal_row_ref_model_limit_adu",
    "thermal_row_ref_comp_adu",
    "thermal_drift_model_active",
    "thermal_drift_model_ntc_clamped",
    "thermal_drift_model_gain_scale_q16",
    "thermal_radiometry_available",
    "thermal_radiometry_active",
    "thermal_radiometry_clamped",
    "thermal_radiometry_axis_adu",
    "thermal_radiometry_index",
    "thermal_radiometry_kt_q14",
    "thermal_radiometry_bt_adu",
    "thermal_radiometry_corrected_nuc",
    "thermal_radiometry_temperature_kelvin_x16",
)


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--label", default="cold",
                        help="output label, e.g. high_trace, state_a_low, state_b_low")
    parser.add_argument("--duration", type=int, default=360)
    parser.add_argument("--base-url", default=DEFAULT_BASE_URL)
    return parser.parse_args()


def read_diagnostics(opener, base_url):
    url = base_url.rstrip("/") + "/api/v1/diagnostics"
    with opener.open(url, timeout=2) as response:
        return json.load(response)


def make_row(diagnostics, elapsed_seconds):
    return {
        "host_time_utc": datetime.now(timezone.utc).isoformat(),
        "elapsed_s": round(elapsed_seconds, 3),
        "timestamp_ms": diagnostics["timestamp_ms"],
        "frame_number": diagnostics["frame_number"],
        "center_roi_c": diagnostics["center_roi_x10"] / 10.0,
        "center_roi_adu": diagnostics["center_roi_adu"],
        "center_raw_c": diagnostics["center_raw_x10"] / 10.0,
        "center_raw_adu": diagnostics["center_raw_adu"],
        "ntc_adu": diagnostics["ntc_adu"],
        "vtemp_adu": diagnostics["vtemp_adu"],
        "smooth_vtemp_adu": diagnostics["smooth_vtemp_adu"],
        "row_ref_mean_adu": diagnostics["row_ref_mean_adu"],
        "row_ref_span_adu": diagnostics["row_ref_span_adu"],
        "shutter_mean_adu": diagnostics["shutter_mean_adu"],
        "mean_diff": diagnostics["mean_diff"],
        "anchor_adu": diagnostics["anchor_adu"],
        "effective_anchor_adu": diagnostics["effective_anchor_adu"],
        "warmup_comp_adu": diagnostics["warmup_comp_adu"],
        "normal_comp_adu": diagnostics["normal_comp_adu"],
        "total_comp_adu": diagnostics["total_comp_adu"],
        "nuc_count": diagnostics["nuc_count"],
        "ffc_state": diagnostics["ffc_state"],
        "shutter_state": diagnostics["shutter_state"],
        "nuc_dg": diagnostics["nuc_dg"],
        "nuc_dv": diagnostics["nuc_dv"],
        "nuc_dn": diagnostics["nuc_dn"],
        "thermal_row_ref_model_valid":
            diagnostics["thermal_row_ref_model_valid"],
        "thermal_row_ref_model_active":
            diagnostics["thermal_row_ref_model_active"],
        "thermal_row_ref_sample_valid":
            diagnostics["thermal_row_ref_sample_valid"],
        "thermal_row_ref_model_reference_adu":
            diagnostics["thermal_row_ref_model_reference_adu"],
        "thermal_row_ref_model_k_q8":
            diagnostics["thermal_row_ref_model_k_q8"],
        "thermal_row_ref_model_limit_adu":
            diagnostics["thermal_row_ref_model_limit_adu"],
        "thermal_row_ref_comp_adu":
            diagnostics["thermal_row_ref_comp_adu"],
        "thermal_drift_model_active":
            diagnostics["thermal_drift_model_active"],
        "thermal_drift_model_ntc_clamped":
            diagnostics["thermal_drift_model_ntc_clamped"],
        "thermal_drift_model_gain_scale_q16":
            diagnostics["thermal_drift_model_gain_scale_q16"],
        "thermal_radiometry_available":
            diagnostics["thermal_radiometry_available"],
        "thermal_radiometry_active":
            diagnostics["thermal_radiometry_active"],
        "thermal_radiometry_clamped":
            diagnostics["thermal_radiometry_clamped"],
        "thermal_radiometry_axis_adu":
            diagnostics["thermal_radiometry_axis_adu"],
        "thermal_radiometry_index":
            diagnostics["thermal_radiometry_index"],
        "thermal_radiometry_kt_q14":
            diagnostics["thermal_radiometry_kt_q14"],
        "thermal_radiometry_bt_adu":
            diagnostics["thermal_radiometry_bt_adu"],
        "thermal_radiometry_corrected_nuc":
            diagnostics["thermal_radiometry_corrected_nuc"],
        "thermal_radiometry_temperature_kelvin_x16":
            diagnostics["thermal_radiometry_temperature_kelvin_x16"],
    }


def main():
    args = parse_args()
    if not 30 <= args.duration <= 1800:
        raise SystemExit("--duration must be between 30 and 1800 seconds")

    project_root = Path(__file__).resolve().parents[1]
    output_dir = project_root / "out" / "thermal_drift_capture"
    output_dir.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    output_path = output_dir / f"tn160_{args.label}_{stamp}.csv"
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    start_time = None
    sample_count = 0
    waiting_shown = False
    output_file = None

    print("TN160 thermal-state capture")
    print(f"Endpoint: {args.base_url.rstrip('/')}/api/v1/diagnostics")
    print("Waiting for the device; press Ctrl+C to stop.", flush=True)

    try:
        while True:
            try:
                diagnostics = read_diagnostics(opener, args.base_url)
            except (OSError, urllib.error.URLError, json.JSONDecodeError):
                if not waiting_shown:
                    print("Device is not reachable yet. Connect or reset it now.",
                          flush=True)
                    waiting_shown = True
                time.sleep(0.25)
                continue

            missing = [name for name in REQUIRED_FIELDS
                       if name not in diagnostics]
            if missing:
                raise SystemExit(
                    "Connected firmware is missing diagnostic fields: "
                    + ", ".join(missing)
                )

            now = time.monotonic()
            if start_time is None:
                start_time = now
                output_file = output_path.open("x", newline="", encoding="utf-8")
                print(f"Connected. Recording {args.label} for "
                      f"{args.duration} seconds.", flush=True)
                if args.label == "hot_rst":
                    print("Press RST now; keep the device powered and aimed "
                          "at 50 C.", flush=True)

            elapsed = now - start_time
            if elapsed >= args.duration:
                break

            row = make_row(diagnostics, elapsed)
            if sample_count == 0:
                writer = csv.DictWriter(output_file, fieldnames=row.keys())
                writer.writeheader()
            writer.writerow(row)
            output_file.flush()
            sample_count += 1
            waiting_shown = False
            time.sleep(SAMPLE_INTERVAL_SECONDS)
    except KeyboardInterrupt:
        print("Capture interrupted; keeping collected samples.", flush=True)
    finally:
        if output_file is not None:
            output_file.close()

    if sample_count == 0:
        raise SystemExit("No samples captured")
    print(f"Capture complete: {sample_count} samples")
    print(f"CSV: {output_path}")


if __name__ == "__main__":
    main()
