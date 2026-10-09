import importlib.util
import json
import struct
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / (
    "prepare_tn160_radiometry_profile.py"
)
SPEC = importlib.util.spec_from_file_location("tn160_radiometry", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(MODULE)


class Tn160RadiometryProfileTests(unittest.TestCase):
    @staticmethod
    def write_mean(path: Path, value: int) -> None:
        path.write_bytes(struct.pack("<" + "H" * (160 * 120),
                                     *([value] * (160 * 120))))

    def archive(self, root: Path) -> None:
        manifest = {
            "board_id": "1",
            "sensor_id": "2",
            "firmware_sha256": "fw",
            "calibration": {
                "header_crc32": "1234abcd",
                "mean_diff_adu": 1000.0,
                "t_ref_low_c": 0,
                "t_ref_high_c": 50,
            },
            "captures": {},
        }
        for name, ntc, anchor, mean in (
            ("low", 2000, 1000, 2000),
            ("high", 2000, 1000, 3000),
            ("offset", 1800, 1000, 1900),
            ("state_b_high", 1800, 1000, 2990),
        ):
            manifest["captures"][name] = {
                "complete": True,
                "after": {
                    "anchor": anchor,
                    "ntc_adu": ntc,
                    "vtemp_adu": 12000 + (2000 - ntc),
                    "mean_diff": 1000.0,
                    "nuc_count": 3,
                },
            }
            self.write_mean(root / f"{name}_mean_u16le.bin", mean)
        (root / "manifest.json").write_text(
            json.dumps(manifest), encoding="utf-8"
        )

    def test_fits_reference_and_second_state(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.archive(root)
            profile = MODULE.fit_archive(root)
        self.assertEqual(profile["status"], "experimental-no-nuc-t")
        self.assertFalse(profile["product_ready"])
        self.assertEqual(len(profile["nodes"]), 2)
        reference = next(node for node in profile["nodes"]
                        if node["state_name"] == "reference")
        learned = next(node for node in profile["nodes"]
                       if node["state_name"] != "reference")
        self.assertEqual(reference["kt_q14"], 16384)
        self.assertLess(learned["kt_q14"], 16384)

    def test_rejects_small_response(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            self.archive(root)
            self.write_mean(root / "high_mean_u16le.bin", 2050)
            with self.assertRaisesRegex(ValueError, "response"):
                MODULE.fit_archive(root)

    def test_nuc_t_must_be_monotonic(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "nuc_t.bin"
            values = list(range(8192))
            values[100] = values[99] - 1
            path.write_bytes(struct.pack("<" + "H" * len(values), *values))
            with self.assertRaisesRegex(ValueError, "monotonic"):
                MODULE._load_nuc_t(path)

    def test_family_profile_can_emit_wildcard_calibration(self):
        self.assertEqual(MODULE._calibration_id(None), 0)


if __name__ == "__main__":
    unittest.main()
