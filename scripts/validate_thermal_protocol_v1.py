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
opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def request(base, path, method="GET", timeout=3):
    req = urllib.request.Request(base.rstrip("/") + path, method=method)
    return opener.open(req, timeout=timeout)


def get_json(base, path):
    with request(base, path) as response:
        assert response.headers.get_content_type() == "application/json"
        return json.load(response)


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
    return struct.unpack_from("<I", meta, 16)[0], flags


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("base", nargs="?", default="http://192.168.17.1")
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

    network = get_json(base, "/api/v1/network")
    assert network["device_ip"] == f"192.168.{network['segment']}.1"
    get_json(base, "/api/v1/measurement")
    get_json(base, "/api/v1/thermal")
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
    }, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
