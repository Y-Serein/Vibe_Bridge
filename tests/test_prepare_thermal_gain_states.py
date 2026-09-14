import csv
import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT_PATH = (
    Path(__file__).resolve().parents[1]
    / "scripts"
    / "prepare_thermal_gain_states.py"
)
SPEC = importlib.util.spec_from_file_location(
    "prepare_thermal_gain_states", SCRIPT_PATH
)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class ThermalGainStateTests(unittest.TestCase):
    @staticmethod
    def write_capture(path, ntc, center, anchor):
        with path.open("w", encoding="utf-8", newline="") as output:
            writer = csv.DictWriter(
                output,
                fieldnames=("elapsed_s", "ntc_adu", "center_roi_adu",
                            "anchor_adu", "ffc_state"),
            )
            writer.writeheader()
            for index in range(8):
                writer.writerow({
                    "elapsed_s": index,
                    "ntc_adu": ntc,
                    "center_roi_adu": center,
                    "anchor_adu": anchor,
                    "ffc_state": 0,
                })

    def test_make_state_interpolates_high_trace_at_low_ntc(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            low = root / "low.csv"
            self.write_capture(low, 1900, 10000, 9900)
            high = root / "high.csv"
            with high.open("w", encoding="utf-8", newline="") as output:
                writer = csv.DictWriter(
                    output,
                    fieldnames=("elapsed_s", "ntc_adu", "center_roi_adu",
                                "anchor_adu", "mean_diff", "ffc_state"),
                )
                writer.writeheader()
                for index, ntc in enumerate(range(1892, 1908, 2)):
                    writer.writerow({
                        "elapsed_s": index,
                        "ntc_adu": ntc,
                        "center_roi_adu": 11400 + 10 * (ntc - 1890),
                        "anchor_adu": 9910 + (ntc - 1890),
                        "mean_diff": 1480,
                        "ffc_state": 0,
                    })
            high_points = MODULE.high_trace_points(high)
            result = MODULE.make_state(
                "A", MODULE.stable_tail(low, 8), high_points,
            )
        self.assertEqual(result["ntc_adu"], 1900)
        self.assertEqual(result["anchor_adu"], 9920)
        self.assertEqual(result["low_adu"], 10020)
        self.assertEqual(result["high_adu"], 11500)
        self.assertEqual(result["response_adu"], 1480)

    def test_make_state_rejects_low_state_outside_high_trace(self):
        low = {"ntc": 1800.0, "adu": 10000.0, "anchor": 9900.0}
        high = [(1890, 11400.0, 9910.0), (1910, 11600.0, 9930.0)]
        with self.assertRaises(ValueError):
            MODULE.make_state("A", low, high)

    def test_path_hysteresis_is_rejected(self):
        samples = []
        elapsed = 0.0
        for ntc in range(1920, 1879, -1):
            samples.append((elapsed, ntc, 2000.0 + ntc, 1000.0))
            elapsed += 1.0
        for ntc in range(1880, 1921):
            samples.append((elapsed, ntc, 2050.0 + ntc, 1000.0))
            elapsed += 1.0
        with self.assertRaisesRegex(ValueError, "path hysteresis"):
            MODULE.validate_no_path_hysteresis(samples)

    def test_small_bidirectional_difference_is_accepted(self):
        samples = []
        elapsed = 0.0
        for ntc in range(1920, 1879, -1):
            samples.append((elapsed, ntc, 2000.0 + ntc, 1000.0))
            elapsed += 1.0
        for ntc in range(1880, 1921):
            samples.append((elapsed, ntc, 2005.0 + ntc, 1000.0))
            elapsed += 1.0
        MODULE.validate_no_path_hysteresis(samples)

    def test_mixed_v1_trace_is_rejected(self):
        samples = [
            (0.0, 1900, 1000.0, 1000.0),
            (1.0, 1899, 999.0, 1001.0),
        ]
        with self.assertRaisesRegex(ValueError, "mixes different V1"):
            MODULE.validate_no_path_hysteresis(samples)


if __name__ == "__main__":
    unittest.main()
