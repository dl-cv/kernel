import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "include/media/rk-camera-frame.h"
ISP = ROOT / "drivers/media/platform/rockchip/isp/rkisp.c"
SDITF_C = ROOT / "drivers/media/platform/rockchip/cif/subdev-itf.c"
SDITF_H = ROOT / "drivers/media/platform/rockchip/cif/subdev-itf.h"

START = "RK_CAMERA_FRAME_START"
END = "RK_CAMERA_FRAME_END"
ERROR = "RK_CAMERA_FRAME_ERROR"


def read(path):
    return path.read_text(encoding="utf-8")


def block_after(text, marker):
    start = text.find(marker)
    if start < 0:
        raise AssertionError(f"missing marker: {marker}")
    brace = text.find("{", start)
    if brace < 0:
        raise AssertionError(f"missing block for: {marker}")
    depth = 0
    for pos in range(brace, len(text)):
        if text[pos] == "{":
            depth += 1
        elif text[pos] == "}":
            depth -= 1
            if depth == 0:
                return text[brace + 1:pos]
    raise AssertionError(f"unterminated block for: {marker}")


def normalized_events(raw, combined_order_known=True, receiver_error=False):
    """Reference ordering used by the source contract tests."""
    if receiver_error:
        return [ERROR]
    has_start = bool(raw & (1 << 6))  # CIF_ISP_V_START, test-only model
    has_end = bool(raw & (1 << 0))    # CIF_ISP_FRAME, test-only model
    if has_start and has_end:
        return [END, START] if combined_order_known else [ERROR]
    if has_end:
        return [END]
    if has_start:
        return [START]
    return []


class EventOrderingModelTests(unittest.TestCase):
    def test_start_only(self):
        self.assertEqual(normalized_events(1 << 6), [START])

    def test_end_only(self):
        self.assertEqual(normalized_events(1 << 0), [END])

    def test_combined_irq_uses_existing_end_then_next_start_order(self):
        self.assertEqual(normalized_events((1 << 6) | (1 << 0)), [END, START])

    def test_combined_irq_without_known_order_is_error(self):
        self.assertEqual(normalized_events((1 << 6) | (1 << 0), False), [ERROR])

    def test_receiver_error_never_emits_success(self):
        events = normalized_events((1 << 6) | (1 << 0), receiver_error=True)
        self.assertEqual(events, [ERROR])


class SharedHeaderContractTests(unittest.TestCase):
    def test_private_normalized_event_bits(self):
        text = read(HEADER)
        self.assertRegex(text, rf"#define\s+{START}\s+BIT\(0\)")
        self.assertRegex(text, rf"#define\s+{END}\s+BIT\(1\)")
        self.assertRegex(text, rf"#define\s+{ERROR}\s+BIT\(2\)")
        self.assertIn("include", HEADER.parts)
        self.assertEqual(HEADER.parent.name, "media")
        self.assertNotIn("uapi", str(HEADER).lower())


class IspBridgeContractTests(unittest.TestCase):
    def test_isp_uses_normalized_helper_and_online_cif_non_readback_guard(self):
        text = read(ISP)
        helper = block_after(text, "rkisp_sditf_frame_event")
        self.assertIn("isp_inp & INP_CIF", helper)
        self.assertIn("IS_HDR_RDBK(dev->rd_mode)", helper)
        self.assertIn("sditf->is_on", helper)
        self.assertIn("interrupt_service_routine", helper)
        self.assertNotIn("isp_mis", helper)
        self.assertNotIn("CIF_ISP_", helper)
        self.assertNotIn("s_stream", helper)

    def test_receiver_error_has_precedence_and_suppresses_success_events(self):
        text = read(ISP)
        isr = block_after(text, "void rkisp_isp_isr")
        self.assertIn("CIF_ISP_DATA_LOSS", isr)
        self.assertIn("CIF_ISP_PIC_SIZE_ERROR", isr)
        self.assertRegex(isr, r"frame_irq_error\s*=.*CIF_ISP_DATA_LOSS")
        self.assertRegex(isr, rf"{ERROR}\)")
        self.assertGreaterEqual(isr.count("!frame_irq_error"), 3)

    def test_combined_start_end_is_deferred_until_after_end(self):
        text = read(ISP)
        isr = block_after(text, "void rkisp_isp_isr")
        end_pos = isr.find(f"{END})")
        start_positions = [m.start() for m in re.finditer(re.escape(f"{START})"), isr)]
        self.assertGreaterEqual(end_pos, 0)
        self.assertGreaterEqual(len(start_positions), 2)
        self.assertIn("if (dev->vs_irq < 0 && !sof_event_later)", isr)
        self.assertGreater(start_positions[-1], end_pos)
        self.assertIn("end of the current frame", isr)
        self.assertIn("next frame start", isr)


class SditfRelayContractTests(unittest.TestCase):
    def test_rcu_registration_and_revocation(self):
        header = read(SDITF_H)
        source = read(SDITF_C)
        self.assertRegex(header, r"struct v4l2_subdev\s+__rcu\s+\*terminal_sensor_sd")
        self.assertIn("rcu_assign_pointer", source)
        self.assertRegex(source, r"rcu_dereference\w*\([^\n]*terminal_sensor_sd")
        self.assertIn("synchronize_rcu()", source)

    def test_relay_callback_is_irq_safe_and_singleton_only(self):
        source = read(SDITF_C)
        callback = block_after(source, "sditf_interrupt_service_routine")
        for forbidden in ("mutex_lock", "mutex_unlock", "msleep", "usleep",
                          "i2c_", "list_for_each", "schedule_work"):
            self.assertNotIn(forbidden, callback)
        self.assertIn("status != RK_CAMERA_FRAME_START", callback)
        self.assertIn("status != RK_CAMERA_FRAME_END", callback)
        self.assertIn("status != RK_CAMERA_FRAME_ERROR", callback)
        self.assertIn("interrupt_service_routine", callback)
        self.assertNotIn("terminal_sensor.sd", callback)
        self.assertNotIn("CIF_ISP_", callback)
        self.assertNotIn("s_stream", callback)

    def test_relay_registered_in_core_ops(self):
        source = read(SDITF_C)
        core_ops = block_after(source, "static const struct v4l2_subdev_core_ops sditf_core_ops")
        self.assertIn(".interrupt_service_routine = sditf_interrupt_service_routine", core_ops)


if __name__ == "__main__":
    unittest.main(verbosity=2)
