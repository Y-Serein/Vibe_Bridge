#!/usr/bin/env python3
"""Build the TN160 V2 batch-mean + per-device residual packet.

V1 remains the independent spatial/temperature baseline.  Firmware applies
the compiled batch mean (290/256 ADU per NTC code) whenever the active V1 has
a stored base NTC.  This tool derives only the state-B residual endpoint; it
does not fit or serialize free PWL nodes.
"""

import argparse
import csv
import json
import math
import statistics
import struct
import urllib.request
import zlib
from pathlib import Path


MODEL_BYTES = 64
MODEL_MAGIC = b"T160DM2\0"
MODEL_VERSION = 2
MODEL_FLAG_GAIN_RESIDUAL = 0x0001
MEAN_K_Q8 = 290
MEAN_LIMIT_ADU = 600
ANCHOR_LIMIT_ADU = 800
GAIN_Q16_LIMIT = 9830
MIN_NTC_SPAN_ADU = 32
TNC1_HEADER_BYTES = 64
TNC1_MAGIC = b"TNC1"
TNC1_MEAN_DIFF_MIN = 100.0


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gain-states", required=True, type=Path,
                        help="CSV: ntc_adu,low_adu,high_adu,anchor_adu")
    parser.add_argument("--input", type=Path,
                        help="existing full TNC1 packet; otherwise fetch header")
    parser.add_argument("--base-url", default="http://192.168.17.1")
    parser.add_argument("--base-ntc-adu", type=int,
                        help="V1 base NTC; fetch model state when omitted")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--upload", action="store_true")
    return parser.parse_args()


def fetch_bytes(base_url, path, timeout=5):
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    with opener.open(base_url.rstrip("/") + path, timeout=timeout) as response:
        return response.read()


def fetch_json(base_url, path, timeout=5):
    return json.loads(fetch_bytes(base_url, path, timeout))


def read_header(args):
    if args.input is not None:
        data = args.input.read_bytes()
        if len(data) < TNC1_HEADER_BYTES:
            raise ValueError("--input is shorter than a TNC1 header")
        return data[:TNC1_HEADER_BYTES]
    return fetch_bytes(args.base_url,
                       "/api/v1/calibration/v1/data/header")


def validate_tnc1_header(header):
    if len(header) != TNC1_HEADER_BYTES or header[:4] != TNC1_MAGIC:
        raise ValueError("active calibration is not a 64-byte TNC1 header")
    version, header_bytes = struct.unpack_from("<HH", header, 4)
    if (version, header_bytes) != (1, TNC1_HEADER_BYTES):
        raise ValueError("unsupported TNC1 version/header size")
    if zlib.crc32(header[:60]) != struct.unpack_from("<I", header, 60)[0]:
        raise ValueError("TNC1 header CRC is invalid")
    mean_diff, t_ref_low = struct.unpack_from("<ff", header, 28)
    if not math.isfinite(mean_diff) or mean_diff < TNC1_MEAN_DIFF_MIN:
        raise ValueError("TNC1 mean_diff is invalid")
    if not math.isfinite(t_ref_low):
        raise ValueError("TNC1 low reference is invalid")
    return zlib.crc32(header), mean_diff, t_ref_low


def load_gain_states(path):
    states = []
    with path.open(encoding="utf-8-sig", newline="") as source:
        for row in csv.DictReader(source):
            try:
                ntc = int(round(float(row["ntc_adu"])))
                low = float(row["low_adu"])
                high = float(row["high_adu"])
                anchor = float(row["anchor_adu"])
            except (KeyError, TypeError, ValueError):
                continue
            response = high - low
            if (8 < ntc < 4088 and response >= TNC1_MEAN_DIFF_MIN
                    and all(math.isfinite(value)
                            for value in (low, high, anchor))):
                states.append((ntc, low, high, anchor, response))
    if not states:
        raise ValueError("--gain-states contains no valid state")
    return sorted(states)


def mean_anchor_comp(base_ntc, current_ntc):
    value = round((current_ntc - base_ntc) * MEAN_K_Q8 / 256.0)
    return max(-MEAN_LIMIT_ADU, min(MEAN_LIMIT_ADU, value))


def choose_learned_state(states, base_ntc):
    state = max(states, key=lambda item: abs(item[0] - base_ntc))
    if abs(state[0] - base_ntc) < MIN_NTC_SPAN_ADU:
        raise ValueError(
            f"state-B NTC span is below {MIN_NTC_SPAN_ADU} ADU"
        )
    return state


def residual_from_state(state, base_ntc, mean_diff):
    learned_ntc, low, high, anchor, response = state
    required_anchor = low - anchor
    mean_anchor = mean_anchor_comp(base_ntc, learned_ntc)
    anchor_residual = round(required_anchor - mean_anchor)
    gain_residual = round((response / mean_diff - 1.0) * 65536.0)
    if abs(mean_anchor + anchor_residual) > ANCHOR_LIMIT_ADU:
        raise ValueError("total state-B anchor exceeds the ±800 ADU bound")
    if abs(anchor_residual) > ANCHOR_LIMIT_ADU:
        raise ValueError("state-B anchor residual exceeds the packet bound")
    if abs(gain_residual) > GAIN_Q16_LIMIT:
        raise ValueError("state-B gain residual exceeds the ±15% bound")
    return anchor_residual, gain_residual


def pack_packet(calibration_id, base_ntc, learned_ntc,
                anchor_residual, gain_residual):
    prefix = struct.pack(
        "<8sHHIHHhhHhHH28s",
        MODEL_MAGIC, MODEL_VERSION, MODEL_BYTES, calibration_id,
        base_ntc, learned_ntc, anchor_residual, gain_residual,
        MODEL_FLAG_GAIN_RESIDUAL, MEAN_K_Q8, MEAN_LIMIT_ADU,
        ANCHOR_LIMIT_ADU, b"\0" * 28,
    )
    assert len(prefix) == MODEL_BYTES - 4
    return prefix + struct.pack("<I", zlib.crc32(prefix))


def validate_packet(packet, calibration_id):
    if len(packet) != MODEL_BYTES:
        raise ValueError("thermal residual packet size is invalid")
    values = struct.unpack("<8sHHIHHhhHhHH28sI", packet)
    (magic, version, size, packet_id, base_ntc, learned_ntc,
     anchor_residual, gain_residual, flags, mean_k_q8,
     mean_limit, anchor_limit, reserved, crc) = values
    if magic != MODEL_MAGIC or (version, size) != (MODEL_VERSION, MODEL_BYTES):
        raise ValueError("thermal residual magic/version is invalid")
    if packet_id != calibration_id or zlib.crc32(packet[:60]) != crc:
        raise ValueError("thermal residual calibration binding or CRC is invalid")
    if flags != MODEL_FLAG_GAIN_RESIDUAL or reserved != b"\0" * 28:
        raise ValueError("thermal residual control fields are invalid")
    if (mean_k_q8, mean_limit, anchor_limit) != (
            MEAN_K_Q8, MEAN_LIMIT_ADU, ANCHOR_LIMIT_ADU):
        raise ValueError("thermal residual batch-mean contract differs")
    if not (8 < base_ntc < 4088 and 8 < learned_ntc < 4088
            and abs(learned_ntc - base_ntc) >= MIN_NTC_SPAN_ADU):
        raise ValueError("thermal residual NTC interval is invalid")
    if abs(anchor_residual) > ANCHOR_LIMIT_ADU:
        raise ValueError("thermal residual anchor exceeds bound")
    if abs(gain_residual) > GAIN_Q16_LIMIT:
        raise ValueError("thermal residual gain exceeds bound")
    if abs(mean_anchor_comp(base_ntc, learned_ntc)
           + anchor_residual) > ANCHOR_LIMIT_ADU:
        raise ValueError("thermal residual total anchor exceeds bound")
    return {
        "base_ntc": base_ntc,
        "learned_ntc": learned_ntc,
        "anchor_residual": anchor_residual,
        "gain_residual": gain_residual,
        "mean_k_q8": mean_k_q8,
        "crc32": crc,
    }


def upload(base_url, packet):
    request = urllib.request.Request(
        base_url.rstrip("/") + "/api/v1/calibration/v1/thermal-model",
        data=packet, method="POST",
        headers={
            "X-Sipeed-Request": "1",
            "X-Sipeed-Thermal-Model-Confirm": "write-v2",
            "Content-Type":
                "application/vnd.sipeed.tn160-thermal-drift-model-v2",
        },
    )
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    with opener.open(request, timeout=10) as response:
        result = response.read().decode("utf-8", errors="replace").strip()
    readback = fetch_bytes(
        base_url, "/api/v1/calibration/v1/thermal-model")
    if readback != packet:
        raise ValueError("device thermal residual readback differs from upload")
    return result + " (readback verified)"


def main():
    args = parse_args()
    calibration_id, mean_diff, _ = validate_tnc1_header(read_header(args))
    base_ntc = args.base_ntc_adu
    if base_ntc is None:
        state = fetch_json(
            args.base_url,
            "/api/v1/calibration/v1/thermal-model/state")
        base_ntc = int(state.get("base_ntc_adu", 0))
    if not 8 < base_ntc < 4088:
        raise ValueError("active V1 does not expose a valid base NTC")
    learned = choose_learned_state(
        load_gain_states(args.gain_states), base_ntc)
    anchor_residual, gain_residual = residual_from_state(
        learned, base_ntc, mean_diff)
    packet = pack_packet(
        calibration_id, base_ntc, learned[0],
        anchor_residual, gain_residual)
    decoded = validate_packet(packet, calibration_id)
    output = args.output or args.gain_states.with_name(
        "thermal_residual_" + args.gain_states.stem + ".bin")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(packet)
    print(f"Calibration ID: {calibration_id}")
    print(f"Batch mean K_Q8: {MEAN_K_Q8} ({MEAN_K_Q8 / 256:.7f} ADU/NTC)")
    print(f"Base/learned NTC: {base_ntc}/{learned[0]} ADU")
    print(f"State-B anchor residual: {anchor_residual} ADU")
    print(f"State-B gain residual: {gain_residual} Q16")
    print(f"Model CRC32: {decoded['crc32']:08x}")
    print(f"Model: {output}")
    if args.upload:
        print("Upload: " + upload(args.base_url, packet))


if __name__ == "__main__":
    main()
