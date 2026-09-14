import importlib.util
import struct
import sys
import unittest
from pathlib import Path


SCRIPT_PATH = (
    Path(__file__).resolve().parents[1]
    / "scripts"
    / "prepare_thermal_drift_model.py"
)
SPEC = importlib.util.spec_from_file_location(
    "prepare_thermal_drift_model", SCRIPT_PATH
)
MODULE = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class ThermalResidualPacketTests(unittest.TestCase):
    def valid_packet(self, calibration_id=0x12345678):
        return MODULE.pack_packet(
            calibration_id, 1950, 1750, -40, 5400
        ), calibration_id

    def test_packet_round_trip(self):
        packet, calibration_id = self.valid_packet()
        decoded = MODULE.validate_packet(packet, calibration_id)
        self.assertEqual(decoded["base_ntc"], 1950)
        self.assertEqual(decoded["learned_ntc"], 1750)
        self.assertEqual(decoded["anchor_residual"], -40)
        self.assertEqual(decoded["gain_residual"], 5400)
        self.assertEqual(decoded["mean_k_q8"], 290)

    def test_packet_crc_is_checked(self):
        packet, calibration_id = self.valid_packet()
        corrupt = bytearray(packet)
        corrupt[20] ^= 1
        with self.assertRaises(ValueError):
            MODULE.validate_packet(bytes(corrupt), calibration_id)

    def test_packet_is_bound_to_exact_v1(self):
        packet, calibration_id = self.valid_packet()
        with self.assertRaises(ValueError):
            MODULE.validate_packet(packet, calibration_id + 1)

    def test_packet_binds_batch_mean_contract(self):
        packet, calibration_id = self.valid_packet()
        corrupt = bytearray(packet)
        struct.pack_into("<h", corrupt, 26, 291)
        struct.pack_into("<I", corrupt, 60,
                         MODULE.zlib.crc32(corrupt[:60]))
        with self.assertRaisesRegex(ValueError, "batch-mean"):
            MODULE.validate_packet(bytes(corrupt), calibration_id)

    def test_ntc_span_must_cover_32_codes(self):
        packet = MODULE.pack_packet(1, 1800, 1770, 0, 0)
        with self.assertRaisesRegex(ValueError, "NTC interval"):
            MODULE.validate_packet(packet, 1)


class ThreeLayerMathTests(unittest.TestCase):
    def test_batch_mean_is_zero_at_v1_reference(self):
        self.assertEqual(MODULE.mean_anchor_comp(1800, 1800), 0)

    def test_batch_mean_uses_confirmed_q8_coefficient(self):
        self.assertEqual(MODULE.mean_anchor_comp(1900, 1800), -113)
        self.assertEqual(MODULE.MEAN_K_Q8, 290)

    def test_batch_mean_is_bounded(self):
        self.assertEqual(
            MODULE.mean_anchor_comp(3000, 1000),
            -MODULE.MEAN_LIMIT_ADU,
        )

    def test_state_b_residual_subtracts_batch_mean(self):
        state = (1700, 800.0, 1900.0, 1000.0, 1100.0)
        anchor_residual, gain_residual = MODULE.residual_from_state(
            state, 1800, 1000.0
        )
        # Required state-B total anchor is -200 ADU. The batch mean already
        # contributes -113 ADU, leaving -87 ADU per-device residual.
        self.assertEqual(anchor_residual, -87)
        self.assertEqual(gain_residual, 6554)

    def test_farthest_valid_state_is_the_learned_endpoint(self):
        states = [
            (1790, 0.0, 1000.0, 0.0, 1000.0),
            (1650, 0.0, 1000.0, 0.0, 1000.0),
        ]
        self.assertEqual(MODULE.choose_learned_state(states, 1800)[0], 1650)


class ThermalRuntimeWiringTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        root = Path(__file__).resolve().parents[1]
        cls.main_source = (root / "src" / "main.c").read_text()
        cls.model_source = (
            root / "src" / "thermal_drift_model.c"
        ).read_text()
        cls.model_header = (
            root / "src" / "thermal_drift_model.h"
        ).read_text()
        cls.ota_source = (root / "src" / "thermal_ota.c").read_text()
        cls.ncm_source = (root / "src" / "thermal_ncm.c").read_text()

    def test_main_path_applies_anchor_and_gain(self):
        self.assertIn(
            "thermal_drift_model_evaluate(g_cur_ntc_adc)",
            self.main_source,
        )
        self.assertIn(
            "(float)drift_model.gain_scale_q16 / 65536.0f",
            self.main_source,
        )

    def test_runtime_gate_defaults_on_and_can_disable_all_drift_layers(self):
        self.assertIn("THERMAL_DRIFT_RUNTIME_ENABLE", self.model_header)
        self.assertIn("#define THERMAL_DRIFT_RUNTIME_ENABLE 1",
                      self.model_header)
        self.assertIn("#if !THERMAL_DRIFT_RUNTIME_ENABLE",
                      self.model_source)
        self.assertIn("return result;", self.model_source)

    def test_ffc_uses_one_model_snapshot(self):
        self.assertIn(
            "thermal_drift_model_evaluate(cur_ntc_at_ffc)",
            self.main_source,
        )
        self.assertIn(
            "candidate_temp_anchor + model_anchor_comp_at_ffc",
            self.main_source,
        )

    def test_mean_fallback_precedes_optional_residual(self):
        mean_pos = self.model_source.index("mean_anchor_comp_for(")
        residual_pos = self.model_source.index("residual_for(")
        self.assertLess(mean_pos, residual_pos)
        self.assertIn("g_thermal_drift_model_mean_active = 1", self.model_source)

    def test_no_free_pwl_nodes_remain(self):
        self.assertNotIn("ntc_knot_adu", self.model_header)
        self.assertNotIn("interpolate_i16", self.model_source)
        self.assertIn("T160DM2", self.model_header)

    def test_flash_verify_flushes_xip_cache_and_retries(self):
        self.assertIn("flash_flush_cache();", self.ota_source)
        self.assertIn("for (unsigned attempt = 0; attempt < 2u; ++attempt)",
                      self.model_source)

    def test_clear_endpoint_and_tombstone_are_explicit(self):
        self.assertIn("THERMAL_DRIFT_MODEL_FLAG_CLEAR", self.model_header)
        self.assertIn("thermal_drift_model_clear()", self.ncm_source)
        self.assertIn("thermal-model/clear", self.ncm_source)
        self.assertIn("selected->packet.flags & THERMAL_DRIFT_MODEL_FLAG_CLEAR",
                      self.model_source)


if __name__ == "__main__":
    unittest.main()
