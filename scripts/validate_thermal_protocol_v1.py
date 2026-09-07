#!/usr/bin/env python3
"""Read-only conformance smoke test for Sipeed Thermal HTTP protocol v1."""

import argparse
import json
import math
import struct
import time
import urllib.error
import urllib.request
import zlib


META_BYTES = 120
PAYLOAD_BYTES = 160 * 120
ADC16_PAYLOAD_BYTES = PAYLOAD_BYTES * 2
CALIBRATION_HEADER_BYTES = 64
CALIBRATION_IMAGE_BYTES = PAYLOAD_BYTES * 2
CALIBRATION_GAIN_BYTES = PAYLOAD_BYTES * 4
CALIBRATION_BADPOINT_BYTES = 10
CALIBRATION_MEAN_DIFF_MIN = 100.0
NTC_MODEL_TAG = 0x4E54
NTC_MODEL_K_Q8_MAX = 8192
NTC_MODEL_LIMIT_MAX = 1500
STARTUP_MODEL_TAG = 0x4354
STARTUP_AMPLITUDE_MAX = 600
STARTUP_TAU_DS_MIN = 50
STARTUP_TAU_DS_MAX = 600
opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def validate_ffc_metadata(flags, meta):
    ffc_count = struct.unpack_from("<H", meta, 98)[0]
    ffc_state = meta[100]
    shutter_state = meta[101]
    assert ffc_state in (0, 1, 2, 3, 4, 255), \
        f"invalid ffc_state: {ffc_state}"
    assert shutter_state in (0, 1, 2, 3, 255), \
        f"invalid shutter_state: {shutter_state}"
    assert bool(flags & (1 << 11)) == (ffc_state in (2, 3)), \
        "FFC_ACTIVE disagrees with ffc_state"
    assert bool(flags & (1 << 12)) == (shutter_state == 1), \
        "SHUTTER_CLOSED disagrees with shutter_state"
    return ffc_count


def validate_ntc_model_header(header):
    tag, reference_adu, k_q8, limit_adu = struct.unpack_from(
        "<HHhH", header, 52
    )
    if tag == reference_adu == k_q8 == limit_adu == 0:
        return "disabled"
    if tag == STARTUP_MODEL_TAG:
        assert 8 < reference_adu < 4088, \
            f"invalid startup NTC gate: {reference_adu}"
        assert abs(k_q8) <= STARTUP_AMPLITUDE_MAX, \
            f"invalid startup amplitude: {k_q8}"
        assert STARTUP_TAU_DS_MIN <= limit_adu <= STARTUP_TAU_DS_MAX, \
            f"invalid startup tau: {limit_adu}"
        return "cold_start"
    assert tag == NTC_MODEL_TAG, f"invalid NTC model tag: 0x{tag:04x}"
    assert 8 < reference_adu < 4088, \
        f"invalid NTC model reference: {reference_adu}"
    assert abs(k_q8) <= NTC_MODEL_K_Q8_MAX, \
        f"invalid NTC model slope: {k_q8}"
    assert 0 < limit_adu <= NTC_MODEL_LIMIT_MAX, \
        f"invalid NTC model limit: {limit_adu}"
    return "legacy_ntc"


def validate_ntc_model_diagnostics(diagnostics):
    stored = bool(diagnostics["thermal_ntc_model_valid"])
    active = bool(diagnostics.get("thermal_ntc_model_active", stored))
    reference_adu = diagnostics["thermal_ntc_model_reference_adu"]
    k_q8 = diagnostics["thermal_ntc_model_k_q8"]
    limit_adu = diagnostics["thermal_ntc_model_limit_adu"]
    if not stored:
        assert (reference_adu, k_q8, limit_adu) == (0, 0, 0)
        return False
    assert 8 < reference_adu < 4088
    assert abs(k_q8) <= NTC_MODEL_K_Q8_MAX
    assert 0 < limit_adu <= NTC_MODEL_LIMIT_MAX
    if active:
        assert abs(diagnostics["total_comp_adu"]) <= limit_adu
    return active


def validate_startup_model_diagnostics(diagnostics):
    valid = bool(diagnostics["thermal_startup_model_valid"])
    active = bool(diagnostics["thermal_startup_model_active"])
    gate = diagnostics["thermal_startup_model_ntc_threshold_adu"]
    amplitude = diagnostics["thermal_startup_model_amplitude_adu"]
    tau_ds = diagnostics["thermal_startup_model_tau_ds"]
    if not valid:
        assert not active
        assert (gate, amplitude, tau_ds) == (0, 0, 0)
        return False
    assert 8 < gate < 4088
    assert abs(amplitude) <= STARTUP_AMPLITUDE_MAX
    assert STARTUP_TAU_DS_MIN <= tau_ds <= STARTUP_TAU_DS_MAX
    assert not active or diagnostics["warmup_comp_adu"] != 0
    return active


def request(base, path, method="GET", timeout=3):
    req = urllib.request.Request(base.rstrip("/") + path, method=method)
    return opener.open(req, timeout=timeout)


def get_json(base, path):
    with request(base, path) as response:
        assert response.headers.get_content_type() == "application/json"
        return json.load(response)


def get_bytes(base, path):
    with request(base, path) as response:
        return response.read()


def get_frame(base, path="/frame.temp8"):
    deadline = time.monotonic() + 5
    while True:
        try:
            with request(base, path) as response:
                return response.headers, response.read()
        except urllib.error.HTTPError as error:
            if error.code != 503 or time.monotonic() >= deadline:
                raise
            time.sleep(0.05)


def validate_meta(payload, meta):
    assert len(payload) == PAYLOAD_BYTES
    assert len(meta) == META_BYTES
    assert meta[:8] == b"SITHMETA"
    assert meta[8:10] == bytes((1, 0))
    assert struct.unpack_from("<H", meta, 10)[0] == META_BYTES
    assert zlib.crc32(meta[:116]) == struct.unpack_from("<I", meta, 116)[0]

    flags = struct.unpack_from("<I", meta, 12)[0]
    assert flags & (1 << 0), "IMAGE_VALID is not set"
    assert struct.unpack_from("<I", meta, 28)[0] == PAYLOAD_BYTES
    assert struct.unpack_from("<HHH", meta, 36) == (160, 120, 160)
    assert meta[42] == 5, "pixel_format is not temp8"
    assert meta[43] == 8 and meta[47] == 254
    if flags & (1 << 5):
        assert zlib.crc32(payload) == struct.unpack_from("<I", meta, 32)[0]
    if flags & (1 << 2):
        scale, offset = struct.unpack_from("<ff", meta, 48)
        assert math.isfinite(scale) and scale > 0 and math.isfinite(offset)
    if flags & (1 << 3):
        assert all(math.isfinite(value)
                   for value in struct.unpack_from("<fff", meta, 56))
    if flags & (1 << 4):
        coordinates = struct.unpack_from("<HHHHHH", meta, 68)
        assert all(x < 160 for x in coordinates[0::2])
        assert all(y < 120 for y in coordinates[1::2])
    validate_ffc_metadata(flags, meta)
    return struct.unpack_from("<I", meta, 16)[0], flags


def validate_adc16(payload, meta):
    assert len(payload) == ADC16_PAYLOAD_BYTES
    assert len(meta) == META_BYTES
    assert meta[:8] == b"SITHMETA"
    assert meta[8:10] == bytes((1, 0))
    assert struct.unpack_from("<H", meta, 10)[0] == META_BYTES
    assert zlib.crc32(meta[:116]) == struct.unpack_from("<I", meta, 116)[0]

    flags = struct.unpack_from("<I", meta, 12)[0]
    assert flags & (1 << 0), "IMAGE_VALID is not set for adc16"
    assert flags & (1 << 5), "PAYLOAD_CRC_VALID is not set for adc16"
    assert not flags & ((1 << 1) | (1 << 2) | (1 << 3) | (1 << 4)), \
        "adc16 must not claim calibration or temperature results"
    assert struct.unpack_from("<I", meta, 28)[0] == ADC16_PAYLOAD_BYTES
    assert struct.unpack_from("<HHH", meta, 36) == (160, 120, 320)
    assert meta[42] == 1, "pixel_format is not adc16"
    assert meta[43] == 14 and meta[47] == 0
    assert zlib.crc32(payload) == struct.unpack_from("<I", meta, 32)[0]
    assert all(value <= 0x3fff
               for (value,) in struct.iter_unpack("<H", payload)), \
        "adc16 sample is not right-aligned to 14 effective bits"
    validate_ffc_metadata(flags, meta)
    return struct.unpack_from("<I", meta, 16)[0], flags


def validate_calibration_v1(base):
    manifest = get_json(base, "/api/v1/calibration/v1/manifest")
    assert manifest["valid"] is True
    assert manifest["format"] == "tn160-calibration-v1"
    assert manifest["packet_bytes"] == (
        CALIBRATION_HEADER_BYTES + CALIBRATION_IMAGE_BYTES
        + CALIBRATION_GAIN_BYTES + CALIBRATION_BADPOINT_BYTES
    )
    assert manifest["header_bytes"] == CALIBRATION_HEADER_BYTES
    assert manifest["image_bytes"] == CALIBRATION_IMAGE_BYTES
    assert manifest["gain_bytes"] == CALIBRATION_GAIN_BYTES
    assert manifest["badpoint_bytes"] == CALIBRATION_BADPOINT_BYTES

    header = get_bytes(base, "/api/v1/calibration/v1/data/header")
    image = get_bytes(base, "/api/v1/calibration/v1/data/image")
    gain = get_bytes(base, "/api/v1/calibration/v1/data/gain")
    badpoints = get_bytes(base, "/api/v1/calibration/v1/data/badpoints")
    assert len(header) == CALIBRATION_HEADER_BYTES
    assert len(image) == CALIBRATION_IMAGE_BYTES
    assert len(gain) == CALIBRATION_GAIN_BYTES
    assert len(badpoints) == CALIBRATION_BADPOINT_BYTES
    assert header[:4] == b"TNC1"
    assert struct.unpack_from("<HHHHHH", header, 4) == (1, 64, 160, 120, 5, 0)
    assert struct.unpack_from("<III", header, 16) == (
        CALIBRATION_IMAGE_BYTES, CALIBRATION_GAIN_BYTES,
        CALIBRATION_BADPOINT_BYTES,
    )
    mean_diff = struct.unpack_from("<f", header, 28)[0]
    assert math.isfinite(mean_diff) and mean_diff >= CALIBRATION_MEAN_DIFF_MIN, \
        f"invalid calibration mean_diff: {mean_diff}"
    validate_ntc_model_header(header)
    assert zlib.crc32(header[:60]) == struct.unpack_from("<I", header, 60)[0]
    assert all(value <= 0x3fff for (value,) in struct.iter_unpack("<H", image))
    assert all(math.isfinite(value) and 0.2 <= value <= 4.0
               for (value,) in struct.iter_unpack("<f", gain))

    seen_terminator = False
    for y, x in zip(badpoints[::2], badpoints[1::2]):
        if (y, x) == (0xff, 0xff):
            seen_terminator = True
        else:
            assert not seen_terminator
            assert x < 160 and y < 120

    crc = manifest["crc32"]
    image_crc = zlib.crc32(image)
    gain_crc = zlib.crc32(gain)
    badpoint_crc = zlib.crc32(badpoints)
    payload_crc = zlib.crc32(badpoints, zlib.crc32(gain, image_crc))
    assert image_crc == struct.unpack_from("<I", header, 36)[0]
    assert gain_crc == struct.unpack_from("<I", header, 40)[0]
    assert badpoint_crc == struct.unpack_from("<I", header, 44)[0]
    assert payload_crc == struct.unpack_from("<I", header, 48)[0]
    assert image_crc == int(crc["image"], 16)
    assert gain_crc == int(crc["gain"], 16)
    assert badpoint_crc == int(crc["badpoints"], 16)
    assert payload_crc == int(crc["payload"], 16)
    assert zlib.crc32(header) == manifest["calibration_id"]
    return manifest


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("base", nargs="?", default="http://192.168.17.1")
    parser.add_argument("--require-calibration", action="store_true",
                        help="还要求存在有效 TNC1 标定并完整只读回检")
    args = parser.parse_args()
    base = args.base.rstrip("/")

    device = get_json(base, "/api/v1/device")
    assert device["api"] == {
        "name": "sipeed-thermal-http", "major": 1, "minor": 0
    }
    assert device["device"]["model"] == "T160"
    temp8 = next(item for item in device["formats"] if item["id"] == "temp8")
    assert temp8["frame"] == "/frame.temp8"
    assert temp8["stream"] == "/stream.temp8"
    adc16 = next(item for item in device["formats"] if item["id"] == "adc16")
    assert adc16["frame"] == "/frame.adc16"
    assert "stream" not in adc16
    assert adc16["effective_bits"] == 14
    assert device["calibration"]["supported"] is True
    assert device["calibration"]["format"] == "tn160-calibration-v1"
    assert device["calibration"]["packet_bytes"] == 115274
    assert device["diagnostics"]["supported"] is True

    network = get_json(base, "/api/v1/network")
    assert network["device_ip"] == f"192.168.{network['segment']}.1"
    get_json(base, "/api/v1/measurement")
    get_json(base, "/api/v1/thermal")
    diagnostics = get_json(base, "/api/v1/diagnostics")
    validate_ntc_model_diagnostics(diagnostics)
    validate_startup_model_diagnostics(diagnostics)
    get_json(base, "/api/v1/ota/status")

    for path in ("/frame.temp8", "/stream.temp8"):
        with request(base, path, method="HEAD") as response:
            assert response.headers["X-Sipeed-Protocol"] == "1.0"
            assert response.headers["X-Sipeed-Format"] == "temp8"
            assert int(response.headers["X-Sipeed-Record-Bytes"]) == 19320
            assert response.read() == b""

    with request(base, "/frame.adc16", method="HEAD") as response:
        assert response.headers["X-Sipeed-Protocol"] == "1.0"
        assert response.headers["X-Sipeed-Format"] == "adc16"
        assert int(response.headers["X-Sipeed-Stride-Bytes"]) == 320
        assert int(response.headers["X-Sipeed-Payload-Bytes"]) == 38400
        assert int(response.headers["X-Sipeed-Record-Bytes"]) == 38520
        assert response.read() == b""

    headers, record = get_frame(base)
    assert int(headers["Content-Length"]) == 19320
    assert len(record) == 19320
    frame_number, flags = validate_meta(record[:PAYLOAD_BYTES],
                                        record[PAYLOAD_BYTES:])
    adc_headers, adc_record = get_frame(base, "/frame.adc16")
    assert int(adc_headers["Content-Length"]) == 38520
    assert len(adc_record) == 38520
    adc_frame_number, adc_flags = validate_adc16(
        adc_record[:ADC16_PAYLOAD_BYTES],
        adc_record[ADC16_PAYLOAD_BYTES:])
    calibration = (validate_calibration_v1(base)
                   if args.require_calibration else None)
    print(json.dumps({
        "ok": True,
        "base": base,
        "model": device["device"]["model"],
        "firmware": device["device"]["firmware"],
        "frame_number": frame_number,
        "flags": f"0x{flags:08x}",
        "record_bytes": len(record),
        "adc16_frame_number": adc_frame_number,
        "adc16_flags": f"0x{adc_flags:08x}",
        "adc16_record_bytes": len(adc_record),
        "calibration_id": (calibration["calibration_id"]
                           if calibration is not None else None),
    }, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
