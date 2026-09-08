#!/usr/bin/env python3
"""Build a TNC1 package containing a per-device RR dynamic-anchor model."""

import argparse
import csv
import math
import statistics
import struct
import urllib.request
import zlib
from datetime import datetime
from pathlib import Path


HEADER_BYTES = 64
IMAGE_BYTES = 160 * 120 * 2
GAIN_BYTES = 160 * 120 * 4
BADPOINT_BYTES = 10
PACKET_BYTES = HEADER_BYTES + IMAGE_BYTES + GAIN_BYTES + BADPOINT_BYTES
ROW_REF_MODEL_TAG = 0x5252
ROW_REF_REFERENCE_MIN = 512
ROW_REF_REFERENCE_MAX = 15872
ROW_REF_K_Q8_MAX = 512
ROW_REF_LIMIT_MAX = 1500


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--trace", required=True, type=Path)
    parser.add_argument("--input", type=Path,
                        help="existing TNC1 packet; otherwise read from device")
    parser.add_argument("--base-url", default="http://192.168.17.1")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--k-q8", type=int, default=256)
    parser.add_argument("--limit-adu", type=int, default=600)
    return parser.parse_args()


def fetch_packet(base_url):
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    prefix = base_url.rstrip("/") + "/api/v1/calibration/v1/data/"
    parts = []
    for name in ("header", "image", "gain", "badpoints"):
        with opener.open(prefix + name, timeout=5) as response:
            parts.append(response.read())
    return b"".join(parts)


def validate_packet(packet):
    if len(packet) != PACKET_BYTES:
        raise ValueError(f"TNC1 packet size is {len(packet)}, expected {PACKET_BYTES}")
    header = packet[:HEADER_BYTES]
    if header[:4] != b"TNC1":
        raise ValueError("TNC1 magic is missing")
    if struct.unpack_from("<HHHHHH", header, 4) != (1, 64, 160, 120, 5, 0):
        raise ValueError("unsupported TNC1 header geometry or version")
    if struct.unpack_from("<III", header, 16) != (
            IMAGE_BYTES, GAIN_BYTES, BADPOINT_BYTES):
        raise ValueError("TNC1 payload sizes are invalid")
    if zlib.crc32(header[:60]) != struct.unpack_from("<I", header, 60)[0]:
        raise ValueError("TNC1 header CRC is invalid")
    image = packet[HEADER_BYTES:HEADER_BYTES + IMAGE_BYTES]
    gain_start = HEADER_BYTES + IMAGE_BYTES
    gain = packet[gain_start:gain_start + GAIN_BYTES]
    badpoints = packet[gain_start + GAIN_BYTES:]
    expected = struct.unpack_from("<IIII", header, 36)
    actual = (
        zlib.crc32(image),
        zlib.crc32(gain),
        zlib.crc32(badpoints),
        zlib.crc32(badpoints, zlib.crc32(gain, zlib.crc32(image))),
    )
    if actual != expected:
        raise ValueError("TNC1 payload CRC is invalid")


def fit_reference(trace_path, k_q8):
    k = k_q8 / 256.0
    if not 0 < k_q8 <= ROW_REF_K_Q8_MAX:
        raise ValueError(f"k_q8 must be between 1 and {ROW_REF_K_Q8_MAX}")
    candidates = []
    residuals = []
    mean_diffs = []
    max_elapsed = 0.0
    with trace_path.open(encoding="utf-8-sig", newline="") as trace_file:
        for row in csv.DictReader(trace_file):
            elapsed = float(row["elapsed_s"])
            max_elapsed = max(max_elapsed, elapsed)
            row_ref = int(row["row_ref_mean_adu"])
            row_span = int(row["row_ref_span_adu"])
            if elapsed < 6.0 or int(row["ffc_state"]) != 0:
                continue
            if not (ROW_REF_REFERENCE_MIN <= row_ref <= ROW_REF_REFERENCE_MAX
                    and 0 < row_span <= 2048):
                continue
            center = int(row["center_roi_adu"])
            anchor = int(row["anchor_adu"])
            mean_diff = float(row["mean_diff"])
            if not math.isfinite(mean_diff) or mean_diff < 100.0:
                continue
            candidates.append(row_ref + (anchor + mean_diff - center) / k)
            residuals.append(center - k * row_ref)
            mean_diffs.append(mean_diff)
    if max_elapsed < 180.0:
        raise ValueError("trace must cover at least 180 seconds")
    if len(candidates) < 100:
        raise ValueError("trace has fewer than 100 usable scene samples")
    reference = round(statistics.median(candidates))
    if not ROW_REF_REFERENCE_MIN <= reference <= ROW_REF_REFERENCE_MAX:
        raise ValueError(f"fitted row-reference value is invalid: {reference}")
    if max(mean_diffs) - min(mean_diffs) > 0.5:
        raise ValueError("trace mean_diff is not stable")
    return (reference, len(candidates), max(residuals) - min(residuals),
            statistics.median(mean_diffs))


def apply_model(packet, reference, k_q8, limit_adu):
    if not 0 < limit_adu <= ROW_REF_LIMIT_MAX:
        raise ValueError(f"limit_adu must be between 1 and {ROW_REF_LIMIT_MAX}")
    result = bytearray(packet)
    struct.pack_into("<HHhH", result, 52, ROW_REF_MODEL_TAG,
                     reference, k_q8, limit_adu)
    struct.pack_into("<I", result, 60, zlib.crc32(result[:60]))
    validate_packet(result)
    return bytes(result)


def main():
    args = parse_args()
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    packet = args.input.read_bytes() if args.input else fetch_packet(args.base_url)
    validate_packet(packet)
    reference, sample_count, residual_pp, trace_mean_diff = fit_reference(
        args.trace, args.k_q8
    )
    packet_mean_diff = struct.unpack_from("<f", packet, 28)[0]
    if (not math.isfinite(packet_mean_diff)
            or abs(packet_mean_diff - trace_mean_diff) > 0.5):
        raise ValueError(
            "trace and TNC1 belong to different calibration data: "
            f"mean_diff {trace_mean_diff:.3f} != {packet_mean_diff:.3f}"
        )
    result = apply_model(packet, reference, args.k_q8, args.limit_adu)
    output = args.output
    if output is None:
        output = args.trace.parent / f"calibration_rr_{reference}_{stamp}.bin"
    output.parent.mkdir(parents=True, exist_ok=True)
    backup = None
    if args.input is None:
        backup = output.parent / f"calibration_before_rr_{stamp}.bin"
        backup.write_bytes(packet)
    output.write_bytes(result)
    print(f"RR reference: {reference} ADU")
    print(f"RR k_q8: {args.k_q8}")
    print(f"RR limit: {args.limit_adu} ADU")
    print(f"Usable samples: {sample_count}")
    print(f"Training residual P-P: {residual_pp:.3f} ADU")
    if backup is not None:
        print(f"Original TNC1 backup: {backup}")
    print(f"TNC1: {output}")


if __name__ == "__main__":
    main()
