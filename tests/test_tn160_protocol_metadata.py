import importlib.util
import struct
import sys
import unittest
from pathlib import Path


SCRIPT_PATH = (
    Path(__file__).resolve().parents[1] / "scripts" / "validate_thermal_protocol_v1.py"
)
SPEC = importlib.util.spec_from_file_location(
    "validate_thermal_protocol_v1_metadata", SCRIPT_PATH
)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class FfcMetadataTests(unittest.TestCase):
    @staticmethod
    def metadata(ffc_count=0x1234, ffc_state=0, shutter_state=0):
        meta = bytearray(MODULE.META_BYTES)
        struct.pack_into("<H", meta, 98, ffc_count)
        meta[100] = ffc_state
        meta[101] = shutter_state
        return meta

    def test_full_width_ffc_count_is_preserved(self):
        self.assertEqual(
            MODULE.validate_ffc_metadata(0, self.metadata()), 0x1234
        )

    def test_active_and_closed_flags_match_states(self):
        flags = (1 << 11) | (1 << 12)
        self.assertEqual(
            MODULE.validate_ffc_metadata(
                flags, self.metadata(ffc_state=2, shutter_state=1)
            ),
            0x1234,
        )

    def test_mismatched_active_flag_is_rejected(self):
        with self.assertRaises(AssertionError):
            MODULE.validate_ffc_metadata(0, self.metadata(ffc_state=3))


class NtcModelDiagnosticsTests(unittest.TestCase):
    @staticmethod
    def diagnostics(valid=1, reference=2048, k_q8=-256, limit=600,
                    total_comp=-120, active=True):
        return {
            "thermal_ntc_model_valid": valid,
            "thermal_ntc_model_active": active,
            "thermal_ntc_model_reference_adu": reference,
            "thermal_ntc_model_k_q8": k_q8,
            "thermal_ntc_model_limit_adu": limit,
            "total_comp_adu": total_comp,
        }

    def test_valid_bounded_model_is_accepted(self):
        self.assertTrue(MODULE.validate_ntc_model_diagnostics(
            self.diagnostics()
        ))

    def test_disabled_model_requires_zero_parameters(self):
        self.assertFalse(MODULE.validate_ntc_model_diagnostics(
            self.diagnostics(valid=0, reference=0, k_q8=0,
                             limit=0, total_comp=0)
        ))

    def test_stored_model_can_be_inactive(self):
        self.assertFalse(MODULE.validate_ntc_model_diagnostics(
            self.diagnostics(active=False)
        ))

    def test_compensation_beyond_declared_limit_is_rejected(self):
        with self.assertRaises(AssertionError):
            MODULE.validate_ntc_model_diagnostics(
                self.diagnostics(total_comp=601)
            )


class StartupModelDiagnosticsTests(unittest.TestCase):
    @staticmethod
    def diagnostics(valid=1, active=1, gate=1900, amplitude=202,
                    tau_ds=172, warmup_comp=120):
        return {
            "thermal_startup_model_valid": valid,
            "thermal_startup_model_active": active,
            "thermal_startup_model_ntc_threshold_adu": gate,
            "thermal_startup_model_amplitude_adu": amplitude,
            "thermal_startup_model_tau_ds": tau_ds,
            "warmup_comp_adu": warmup_comp,
        }

    def test_valid_active_model_is_accepted(self):
        self.assertTrue(MODULE.validate_startup_model_diagnostics(
            self.diagnostics()
        ))

    def test_valid_negative_active_model_is_accepted(self):
        self.assertTrue(MODULE.validate_startup_model_diagnostics(
            self.diagnostics(amplitude=-202, warmup_comp=-120)
        ))

    def test_inactive_model_can_remain_stored(self):
        self.assertFalse(MODULE.validate_startup_model_diagnostics(
            self.diagnostics(active=0, warmup_comp=0)
        ))

    def test_zero_amplitude_is_a_valid_inactive_model(self):
        self.assertFalse(MODULE.validate_startup_model_diagnostics(
            self.diagnostics(active=0, amplitude=0, warmup_comp=0)
        ))

    def test_missing_model_requires_zero_fields(self):
        self.assertFalse(MODULE.validate_startup_model_diagnostics(
            self.diagnostics(valid=0, active=0, gate=0, amplitude=0,
                             tau_ds=0, warmup_comp=0)
        ))


if __name__ == "__main__":
    unittest.main()
