import importlib.util
import struct
import sys
import unittest
import zlib
from pathlib import Path


SCRIPT_PATH = (
    Path(__file__).resolve().parents[1] / "scripts" / "validate_thermal_protocol_v1.py"
)
SPEC = importlib.util.spec_from_file_location(
    "validate_thermal_protocol_v1", SCRIPT_PATH
)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


def make_valid_tnc1(ntc_model=None, mean_diff=1000.0):
    image = struct.pack("<H", 8192) * (160 * 120)
    gain = struct.pack("<f", 1.0) * (160 * 120)
    badpoints = b"\xff\xff" * 5
    header = bytearray(64)
    header[:4] = b"TNC1"
    struct.pack_into("<HHHHHHIIIff", header, 4,
                     1, 64, 160, 120, 5, 0,
                     len(image), len(gain), len(badpoints), mean_diff, 0.0)
    struct.pack_into("<IIII", header, 36,
                     zlib.crc32(image), zlib.crc32(gain),
                     zlib.crc32(badpoints),
                     zlib.crc32(badpoints, zlib.crc32(gain, zlib.crc32(image))))
    if ntc_model is not None:
        struct.pack_into("<HHhH", header, 52, *ntc_model)
    struct.pack_into("<I", header, 60, zlib.crc32(header[:60]))
    manifest = {
        "valid": True,
        "format": "tn160-calibration-v1",
        "packet_bytes": len(header) + len(image) + len(gain) + len(badpoints),
        "header_bytes": len(header),
        "image_bytes": len(image),
        "gain_bytes": len(gain),
        "badpoint_bytes": len(badpoints),
        "calibration_id": zlib.crc32(header),
        "crc32": {
            "image": f"{zlib.crc32(image):08x}",
            "gain": f"{zlib.crc32(gain):08x}",
            "badpoints": f"{zlib.crc32(badpoints):08x}",
            "payload": f"{zlib.crc32(badpoints, zlib.crc32(gain, zlib.crc32(image))):08x}",
        },
    }
    return manifest, {
        "/api/v1/calibration/v1/data/header": bytes(header),
        "/api/v1/calibration/v1/data/image": image,
        "/api/v1/calibration/v1/data/gain": gain,
        "/api/v1/calibration/v1/data/badpoints": badpoints,
    }


class CalibrationV1ContractTests(unittest.TestCase):
    def run_validation(self, manifest, payloads):
        old_json = MODULE.get_json
        old_bytes = MODULE.get_bytes
        try:
            MODULE.get_json = lambda _base, _path: manifest
            MODULE.get_bytes = lambda _base, path: payloads[path]
            return MODULE.validate_calibration_v1("http://tn160.test")
        finally:
            MODULE.get_json = old_json
            MODULE.get_bytes = old_bytes

    def test_valid_tnc1_contract_passes(self):
        manifest, payloads = make_valid_tnc1()
        self.assertEqual(self.run_validation(manifest, payloads), manifest)

    def test_header_crc_mismatch_is_rejected(self):
        manifest, payloads = make_valid_tnc1()
        header = bytearray(payloads["/api/v1/calibration/v1/data/header"])
        header[28] ^= 1
        payloads["/api/v1/calibration/v1/data/header"] = bytes(header)
        with self.assertRaises(AssertionError):
            self.run_validation(manifest, payloads)

    def test_insufficient_two_point_response_is_rejected(self):
        manifest, payloads = make_valid_tnc1(mean_diff=1.0)
        with self.assertRaises(AssertionError):
            self.run_validation(manifest, payloads)

    def test_minimum_two_point_response_is_accepted(self):
        manifest, payloads = make_valid_tnc1(
            mean_diff=MODULE.CALIBRATION_MEAN_DIFF_MIN
        )
        self.assertEqual(self.run_validation(manifest, payloads), manifest)

    def test_valid_ntc_model_extension_is_accepted(self):
        model = (MODULE.NTC_MODEL_TAG, 2048, 128, 600)
        manifest, payloads = make_valid_tnc1(model)
        self.assertEqual(self.run_validation(manifest, payloads), manifest)

    def test_invalid_ntc_model_extension_is_rejected(self):
        model = (MODULE.NTC_MODEL_TAG, 2048, 128, MODULE.NTC_MODEL_LIMIT_MAX + 1)
        manifest, payloads = make_valid_tnc1(model)
        with self.assertRaises(AssertionError):
            self.run_validation(manifest, payloads)

    def test_valid_cold_start_model_extension_is_accepted(self):
        model = (MODULE.STARTUP_MODEL_TAG, 1900, 202, 172)
        manifest, payloads = make_valid_tnc1(model)
        self.assertEqual(self.run_validation(manifest, payloads), manifest)

    def test_valid_negative_cold_start_model_extension_is_accepted(self):
        model = (MODULE.STARTUP_MODEL_TAG, 1900, -202, 172)
        manifest, payloads = make_valid_tnc1(model)
        self.assertEqual(self.run_validation(manifest, payloads), manifest)

    def test_zero_cold_start_amplitude_is_a_valid_noop_model(self):
        model = (MODULE.STARTUP_MODEL_TAG, 1900, 0, 172)
        manifest, payloads = make_valid_tnc1(model)
        self.assertEqual(self.run_validation(manifest, payloads), manifest)

    def test_invalid_cold_start_model_extension_is_rejected(self):
        model = (MODULE.STARTUP_MODEL_TAG, 1900,
                 MODULE.STARTUP_AMPLITUDE_MAX + 1, 172)
        manifest, payloads = make_valid_tnc1(model)
        with self.assertRaises(AssertionError):
            self.run_validation(manifest, payloads)

    def test_valid_row_reference_model_extension_is_accepted(self):
        model = (MODULE.ROW_REF_MODEL_TAG, 4948, 256, 600)
        manifest, payloads = make_valid_tnc1(model)
        self.assertEqual(self.run_validation(manifest, payloads), manifest)

    def test_invalid_row_reference_slope_is_rejected(self):
        model = (MODULE.ROW_REF_MODEL_TAG, 4948, 0, 600)
        manifest, payloads = make_valid_tnc1(model)
        with self.assertRaises(AssertionError):
            self.run_validation(manifest, payloads)

    def test_invalid_row_reference_reference_is_rejected(self):
        model = (MODULE.ROW_REF_MODEL_TAG,
                 MODULE.ROW_REF_REFERENCE_MAX + 1, 256, 600)
        manifest, payloads = make_valid_tnc1(model)
        with self.assertRaises(AssertionError):
            self.run_validation(manifest, payloads)


if __name__ == "__main__":
    unittest.main()
