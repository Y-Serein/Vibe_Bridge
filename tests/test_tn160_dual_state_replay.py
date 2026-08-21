import csv
import importlib.util
import sys
import tempfile
import unittest
from pathlib import Path


SCRIPT_PATH = (
    Path(__file__).resolve().parents[1] / "scripts" / "tn160_dual_state_replay.py"
)
SPEC = importlib.util.spec_from_file_location("tn160_dual_state_replay", SCRIPT_PATH)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class DualStateReplayTests(unittest.TestCase):
    def write_csv(self, directory: Path, *, omit: str | None = None) -> Path:
        path = directory / "diagnostic.csv"
        fieldnames = [
            "t_s",
            "center_roi_adu",
            "mean_diff",
            "ntc_adu",
            "ntc_c",
            "nuc_decision",
            "nuc_count",
            "nuc_dg",
        ]
        if omit is not None:
            fieldnames.remove(omit)
        shutter_delta = 0
        nuc_count = 0
        with path.open("w", encoding="utf-8", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=fieldnames)
            writer.writeheader()
            for t_s in range(200):
                decision = 255
                nuc_dg = 0
                if t_s == 2:
                    nuc_count = 1
                    decision = 0
                    nuc_dg = 500
                elif t_s > 2 and (t_s - 2) % 30 == 0:
                    nuc_count += 1
                    decision = 0
                    nuc_dg = -10
                    shutter_delta += nuc_dg
                ntc_adu = 2000.0 - t_s
                required_anchor = 8000.0 + 1.2 * ntc_adu + shutter_delta
                row = {
                    "t_s": t_s,
                    "center_roi_adu": required_anchor + 1000.0,
                    "mean_diff": 1000.0,
                    "ntc_adu": ntc_adu,
                    "ntc_c": 25.0 + t_s / 20.0,
                    "nuc_decision": decision,
                    "nuc_count": nuc_count,
                    "nuc_dg": nuc_dg,
                }
                if omit is not None:
                    del row[omit]
                writer.writerow(row)
        return path

    def test_exact_model_replays_later_window(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = self.write_csv(Path(directory))
            report = MODULE.build_report(
                path,
                target_c=50.0,
                low_c=0.0,
                high_c=50.0,
                state_a=MODULE.Window(10.0, 20.0),
                state_b=MODULE.Window(90.0, 120.0),
                verify=MODULE.Window(120.0, 199.0),
                shutter_coefficient=1.0,
            )
        self.assertAlmostEqual(
            report.fitted_ntc_coefficient_adu_per_code, 1.2, places=9
        )
        self.assertLess(report.metrics.maximum_absolute_error_c, 1e-9)
        self.assertTrue(report.metrics.peak_to_peak_pass)
        self.assertFalse(report.dynamic_mean_diff_identifiable)

    def test_missing_required_column_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = self.write_csv(Path(directory), omit="nuc_dg")
            with self.assertRaisesRegex(ValueError, "nuc_dg"):
                MODULE.load_samples(path, 50.0, 0.0, 50.0)

    def test_t4_endpoints(self) -> None:
        self.assertAlmostEqual(
            MODULE.temperature_from_adu(9000.0, 9000.0, 1000.0, 0.0, 50.0),
            0.0,
            places=9,
        )
        self.assertAlmostEqual(
            MODULE.temperature_from_adu(10000.0, 9000.0, 1000.0, 0.0, 50.0),
            50.0,
            places=9,
        )


if __name__ == "__main__":
    unittest.main()
