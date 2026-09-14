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


class ThermalStateDiagnosticsTests(unittest.TestCase):
    @staticmethod
    def diagnostics(row_ref_mean=8192, row_ref_span=48, shutter_mean=8010):
        return {
            "row_ref_mean_adu": row_ref_mean,
            "row_ref_span_adu": row_ref_span,
            "shutter_mean_adu": shutter_mean,
        }

    def test_ready_14_bit_state_is_accepted(self):
        self.assertTrue(MODULE.validate_thermal_state_diagnostics(
            self.diagnostics()
        ))

    def test_not_ready_zero_state_is_well_formed(self):
        self.assertFalse(MODULE.validate_thermal_state_diagnostics(
            self.diagnostics(row_ref_mean=0, row_ref_span=0, shutter_mean=0)
        ))

    def test_out_of_range_state_is_rejected(self):
        with self.assertRaises(AssertionError):
            MODULE.validate_thermal_state_diagnostics(
                self.diagnostics(row_ref_span=0x4000)
            )


class RowReferenceModelDiagnosticsTests(unittest.TestCase):
    @staticmethod
    def diagnostics(valid=1, active=1, sample_valid=1, reference=4948,
                    k_q8=256, limit=600, comp=-301):
        return {
            "thermal_row_ref_model_valid": valid,
            "thermal_row_ref_model_active": active,
            "thermal_row_ref_sample_valid": sample_valid,
            "thermal_row_ref_model_reference_adu": reference,
            "thermal_row_ref_model_k_q8": k_q8,
            "thermal_row_ref_model_limit_adu": limit,
            "thermal_row_ref_comp_adu": comp,
            "thermal_startup_model_active": 0,
            "thermal_ntc_model_active": False,
            "normal_comp_adu": comp,
        }

    def test_valid_active_model_is_accepted(self):
        self.assertTrue(MODULE.validate_row_ref_model_diagnostics(
            self.diagnostics()
        ))

    def test_missing_model_requires_zero_state(self):
        self.assertFalse(MODULE.validate_row_ref_model_diagnostics(
            self.diagnostics(valid=0, active=0, sample_valid=0,
                             reference=0, k_q8=0, limit=0, comp=0)
        ))

    def test_compensation_beyond_limit_is_rejected(self):
        with self.assertRaises(AssertionError):
            MODULE.validate_row_ref_model_diagnostics(
                self.diagnostics(comp=601)
            )

    def test_active_model_disables_startup_model(self):
        diagnostics = self.diagnostics()
        diagnostics["thermal_startup_model_active"] = 1
        with self.assertRaises(AssertionError):
            MODULE.validate_row_ref_model_diagnostics(diagnostics)


class ThermalDriftModelDiagnosticsTests(unittest.TestCase):
    @staticmethod
    def diagnostics(valid=1, active=1, mean_active=1, per_device=1,
                    clamped=0, mean_anchor=-300, device_anchor=-42,
                    gain_delta=0, gain_scale=65536):
        return {
            "valid": valid,
            "active": active,
            "mean_active": mean_active,
            "per_device": per_device,
            "ntc_clamped": clamped,
            "base_ntc_adu": 1800 if active else 0,
            "ntc_min_adu": 1650 if valid else 0,
            "ntc_max_adu": 1980 if valid else 0,
            "mean_k_q8": 290,
            "mean_limit_adu": 600,
            "mean_anchor_comp_adu": mean_anchor if active else 0,
            "device_anchor_residual_adu": device_anchor if per_device else 0,
            "anchor_comp_adu": mean_anchor + device_anchor if active else 0,
            "gain_delta_q16": gain_delta,
            "gain_scale_q16": gain_scale,
        }

    def test_no_record_uses_batch_mean_fallback(self):
        self.assertTrue(MODULE.validate_thermal_drift_model_diagnostics(
            self.diagnostics(valid=0, active=1, per_device=0,
                             device_anchor=0)
        ))

    def test_invalid_ntc_falls_back_to_v1_only(self):
        self.assertFalse(MODULE.validate_thermal_drift_model_diagnostics(
            self.diagnostics(valid=0, active=0, mean_active=0,
                             per_device=0)
        ))

    def test_active_model_requires_mean_layer(self):
        with self.assertRaises(AssertionError):
            MODULE.validate_thermal_drift_model_diagnostics(
                self.diagnostics(valid=0, active=1, mean_active=0,
                                 per_device=0)
            )

    def test_anchor_limit_is_enforced(self):
        with self.assertRaises(AssertionError):
            MODULE.validate_thermal_drift_model_diagnostics(
                self.diagnostics(mean_anchor=600, device_anchor=201)
            )

    def test_gain_scale_matches_reported_delta(self):
        with self.assertRaises(AssertionError):
            MODULE.validate_thermal_drift_model_diagnostics(
                self.diagnostics(gain_delta=100, gain_scale=65536)
            )

    def test_inactive_model_has_neutral_state(self):
        self.assertFalse(MODULE.validate_thermal_drift_model_diagnostics(
            self.diagnostics(valid=0, active=0, mean_active=0,
                             per_device=0, gain_scale=65536)
        ))


class ThermalDriftModelPacketTests(unittest.TestCase):
    @staticmethod
    def packet(calibration_id=0x12345678):
        prefix = struct.pack(
            "<8sHHIHHhhHhHH28s",
            b"T160DM2\0", 2, 64, calibration_id, 1800, 1650,
            -40, 5000, 1, 290, 600, 800, b"\0" * 28,
        )
        return prefix + struct.pack("<I", zlib.crc32(prefix))

    def test_valid_packet_is_accepted(self):
        packet = self.packet()
        decoded = MODULE.validate_thermal_drift_model_packet(
            packet, 0x12345678
        )
        self.assertEqual(decoded["learned_ntc"], 1650)
        self.assertEqual(decoded["anchor_residual"], -40)

    def test_packet_rejects_wrong_calibration_id(self):
        with self.assertRaises(AssertionError):
            MODULE.validate_thermal_drift_model_packet(
                self.packet(), 0x12345679
            )


if __name__ == "__main__":
    unittest.main()
