import importlib.util
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


PROFILE = load("profile", ROOT / "scripts" / "prepare_tn160_radiometry_profile.py")
FAMILY = load("family", ROOT / "scripts" / "fit_tn160_family_profile.py")
DEVICE = load("device", ROOT / "scripts" / "fit_tn160_device_residual.py")


class FamilyDeviceProfileTests(unittest.TestCase):
    @staticmethod
    def write_mean(path: Path, value: int) -> None:
        path.write_bytes(struct.pack("<" + "H" * (160 * 120),
                                     *([value] * (160 * 120))))

    def archive(self, root: Path, scale: float = 1.0) -> None:
        manifest = {
            "board_id": "1",
            "sensor_id": root.name,
            "firmware_sha256": "fw",
            "calibration": {
                "header_crc32": "1234abcd",
                "mean_diff_adu": 1000.0,
                "t_ref_low_c": 0,
                "t_ref_high_c": 50,
            },
            "captures": {},
            "state_a_reference": {"ntc_adu": 2000, "vtemp_adu": 12000},
        }
        for name, ntc, anchor, mean in (
            ("low", 2000, 1000, 2000),
            ("high", 2000, 1000, int(3000 * scale)),
            ("offset", 1800, 1000, int(1900 * scale)),
            ("state_b_high", 1800, 1000, int(2990 * scale)),
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

    def profile(self, root: Path, gain=65536, bias=0):
        result = PROFILE.fit_archive(root)
        result["nuc_t"] = {
            "present": True,
            "sha256": "same",
            "count": 8192,
        }
        result["device_residual"] = {"gain_q16": gain, "bias_adu": bias}
        return result

    def test_family_uses_median_nodes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            profiles = []
            for index, scale in enumerate((1.0, 1.01, 0.99)):
                archive = root / f"a{index}"
                archive.mkdir()
                self.archive(archive, scale)
                profile = self.profile(archive)
                path = root / f"p{index}.json"
                path.write_text(json.dumps(profile), encoding="utf-8")
                profiles.append(path)
            family = FAMILY.fit(profiles)
        self.assertFalse(family["product_ready"])
        self.assertEqual(family["nuc_t"]["sha256"], "same")

    def test_device_residual_is_ratio_to_family(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            family_archive = root / "family"
            device_archive = root / "device"
            family_archive.mkdir()
            device_archive.mkdir()
            self.archive(family_archive)
            self.archive(device_archive)
            family = self.profile(family_archive)
            family["schema"] = "tn160-radiometry-family-profile-v1"
            family["family_id"] = "test-family"
            family_path = root / "family.json"
            family_path.write_text(json.dumps(family), encoding="utf-8")
            result = DEVICE.fit_device(family_path, device_archive)
        self.assertEqual(result["device_residual"]["gain_q16"], 65536)
        self.assertEqual(result["device_residual"]["bias_adu"], 0)


if __name__ == "__main__":
    unittest.main()
