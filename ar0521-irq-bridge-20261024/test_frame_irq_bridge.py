import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "include/media/rk-camera-frame.h"
ISP = ROOT / "drivers/media/platform/rockchip/isp/rkisp.c"
ISP_DEV_C = ROOT / "drivers/media/platform/rockchip/isp/dev.c"
ISP_DEV_H = ROOT / "drivers/media/platform/rockchip/isp/dev.h"
SDITF_C = ROOT / "drivers/media/platform/rockchip/cif/subdev-itf.c"
SDITF_H = ROOT / "drivers/media/platform/rockchip/cif/subdev-itf.h"
CIF_DEV_C = ROOT / "drivers/media/platform/rockchip/cif/dev.c"
CIF_DEV_H = ROOT / "drivers/media/platform/rockchip/cif/dev.h"
CIF_CAPTURE = ROOT / "drivers/media/platform/rockchip/cif/capture.c"
FRAME_CHAIN_C = ROOT / "drivers/media/v4l2-core/rk-camera-frame.c"
V4L2_MAKEFILE = ROOT / "drivers/media/v4l2-core/Makefile"

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


def normalized_events(raw, receiver_error=False):
    """Reference event normalization used by the source contract tests."""
    if receiver_error:
        return [ERROR]
    has_start = bool(raw & (1 << 6))  # CIF_ISP_V_START, test-only model
    has_end = bool(raw & (1 << 0))    # CIF_ISP_FRAME, test-only model
    if has_start and has_end:
        return [ERROR]
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

    def test_combined_irq_is_single_error(self):
        self.assertEqual(normalized_events((1 << 6) | (1 << 0)), [ERROR])

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
    def test_input_bridge_uses_rcu_published_cif_input_only(self):
        text = read(ISP)
        helper = block_after(text, "rkisp_cif_input_frame_event")
        self.assertIn("isp_inp & INP_CIF", helper)
        self.assertIn("IS_HDR_RDBK(dev->rd_mode)", helper)
        self.assertIn("rcu_read_lock()", helper)
        self.assertIn("rcu_dereference(dev->cif_input_sd)", helper)
        self.assertIn("rcu_read_unlock()", helper)
        self.assertIn("interrupt_service_routine", helper)
        self.assertNotIn("isp_mis", helper)
        self.assertNotIn("CIF_ISP_", helper)
        self.assertNotIn("s_stream", helper)

    def test_input_bridge_does_not_use_vpss_output_or_walk_graph(self):
        helper = block_after(read(ISP), "rkisp_cif_input_frame_event")
        for forbidden in ("dev->sditf_dev", "remote_sd", "RKISP_VPSS",
                          "active_sensor", "sensors[", "get_remote_sensor",
                          "for_each"):
            self.assertNotIn(forbidden, helper)

    def test_active_selection_hands_sensor_to_checked_setter(self):
        update = block_after(read(ISP), "int rkisp_update_sensor_info")
        self.assertIn("rkisp_set_cif_input_sd(dev, sensor->sd)", update)
        binder = block_after(read(ISP_DEV_C), "void rkisp_bind_cif_input_sd")
        self.assertIn("MEDIA_ENT_F_PROC_VIDEO_COMPOSER", binder)

    def test_input_notifier_unbind_makes_input_unpublishable_before_sync(self):
        dev = read(ISP_DEV_C)
        unbind = block_after(dev, "static void subdev_notifier_unbind")
        self.assertIn("rkisp_unbind_cif_input_sd(isp_dev, subdev)", unbind)
        revoke = block_after(dev, "void rkisp_unbind_cif_input_sd")
        self.assertIn("spin_lock_irqsave", revoke)
        candidate = block_after(revoke, "if (dev->cif_input_bound_sd == sd)")
        self.assertIn("dev->cif_input_bound_sd = NULL", candidate)
        self.assertNotIn("RCU_INIT_POINTER", candidate)
        self.assertIn("RCU_INIT_POINTER(dev->cif_input_sd, NULL)", revoke)
        self.assertIn("synchronize_rcu()", revoke)

    def test_bound_selection_and_unbind_share_writer_lock(self):
        dev = read(ISP_DEV_C)
        bound = block_after(dev, "static int subdev_notifier_bound")
        self.assertIn("rkisp_bind_cif_input_sd(isp_dev, subdev)", bound)
        binder = block_after(dev, "void rkisp_bind_cif_input_sd")
        setter = block_after(dev, "void rkisp_set_cif_input_sd")
        for writer in (binder, setter):
            self.assertIn("spin_lock_irqsave", writer)
            self.assertIn("cif_input_bound_sd", writer)
        assign = setter.find("rcu_assign_pointer(dev->cif_input_sd, sd)")
        check = setter.find("dev->cif_input_bound_sd == sd")
        self.assertGreaterEqual(assign, 0)
        self.assertGreater(assign, check)

    def test_input_sd_has_bound_state_rcu_field_and_lock(self):
        header = read(ISP_DEV_H)
        self.assertRegex(header, r"struct v4l2_subdev\s+__rcu\s+\*cif_input_sd")
        self.assertRegex(header, r"struct v4l2_subdev\s+\*cif_input_bound_sd")
        self.assertRegex(header, r"spinlock_t\s+cif_input_lock")

    def test_input_lock_initialized_before_async_notifier_registration(self):
        isp = read(ISP)
        init = block_after(isp, "int rkisp_register_isp_subdev")
        self.assertIn("spin_lock_init(&isp_dev->cif_input_lock)", init)
        dev = read(ISP_DEV_C)
        platform = block_after(dev, "static int rkisp_register_platform_subdevs")
        register_isp = platform.find("ret = rkisp_register_isp_subdev(dev")
        register_notifier = platform.find("ret = isp_subdev_notifier(dev)")
        self.assertGreaterEqual(register_isp, 0)
        self.assertGreater(register_notifier, register_isp)

    def test_input_bridge_only_notifies_while_isp_started(self):
        helper = block_after(read(ISP), "rkisp_cif_input_frame_event")
        self.assertIn("READ_ONCE(dev->isp_state)", helper)
        self.assertIn("!(isp_state & ISP_START)", helper)
        self.assertIn("isp_state & ISP_STOP", helper)

    def test_registered_cif_subdev_is_async_unregistered_on_remove(self):
        source = read(SDITF_C)
        media_init = block_after(source, "static int rkcif_subdev_media_init")
        self.assertIn("if (priv->port_count > 1)", media_init)
        self.assertIn("sditf_subdev_notifier(priv)", media_init)

        remove = block_after(source, "static int rkcif_subdev_remove")
        private_notifier = block_after(remove, "if (priv->port_count > 1)")
        self.assertIn("v4l2_async_nf_unregister(&priv->notifier)", private_notifier)
        self.assertIn("v4l2_async_nf_cleanup(&priv->notifier)", private_notifier)
        self.assertIn("sditf_unregister_isp(priv)", remove)
        unregister = remove.find("sditf_unregister_isp(priv)")
        cleanup = remove.find("media_entity_cleanup(&sd->entity)")
        self.assertGreater(cleanup, unregister)

        helper = block_after(source, "void sditf_unregister_isp")
        self.assertIn("atomic_cmpxchg(&priv->is_async_registered, 1, 0)", helper)
        self.assertIn("v4l2_async_unregister_subdev(&priv->sd)", helper)

    def test_cif_uninit_unregisters_registered_sditf_before_notifier(self):
        dev = read(CIF_DEV_C)
        uninit = block_after(dev, "int rkcif_plat_uninit")
        clear = uninit.find("rkcif_set_terminal_sensor_fwnode(cif_dev, NULL)")
        unregister_sditf = uninit.find("sditf_unregister_isp(cif_dev->sditf[i])")
        nf_unregister = uninit.find("v4l2_async_nf_unregister(&cif_dev->notifier)")
        self.assertGreaterEqual(clear, 0)
        self.assertGreater(unregister_sditf, clear)
        self.assertGreater(nf_unregister, unregister_sditf)

    def test_sditf_registration_marks_isp_async_state(self):
        dev = read(CIF_DEV_C)
        register = block_after(dev, "static int subdev_asyn_register_itf")
        self.assertIn("v4l2_async_register_subdev_sensor(&sditf->sd)", register)
        self.assertIn("atomic_set(&sditf->is_async_registered, 1)", register)

        source = read(SDITF_C)
        multiport = block_after(source, "static int sditf_subdev_notifier")
        self.assertIn("v4l2_async_register_subdev(&sditf->sd)", multiport)
        self.assertIn("atomic_set(&sditf->is_async_registered, 1)", multiport)
        media_init = block_after(source, "static int rkcif_subdev_media_init")
        flag_init = media_init.find("atomic_set(&priv->is_async_registered, 0)")
        notifier_call = media_init.find("sditf_subdev_notifier(priv)")
        self.assertGreaterEqual(flag_init, 0)
        self.assertGreater(notifier_call, flag_init)
        self.assertEqual(dev.count("#include <linux/property.h>"), 1)
        self.assertRegex(dev, r"int rkcif_plat_init\([^)]*\)\n\{")

    def test_cif_input_link_object_is_composer(self):
        source = read(ISP_DEV_C)
        create_links = block_after(source, "static int rkisp_create_links")
        self.assertIn("MEDIA_ENT_F_PROC_VIDEO_COMPOSER", create_links)
        self.assertIn("dev->isp_inp = INP_CIF", create_links)

    def test_receiver_error_has_precedence_and_suppresses_success_events(self):
        text = read(ISP)
        isr = block_after(text, "void rkisp_isp_isr")
        self.assertIn("CIF_ISP_DATA_LOSS", isr)
        self.assertIn("CIF_ISP_PIC_SIZE_ERROR", isr)
        self.assertRegex(isr, r"frame_irq_error\s*=.*CIF_ISP_DATA_LOSS")
        self.assertRegex(isr, rf"{ERROR}\)")
        self.assertGreaterEqual(isr.count("!frame_irq_error"), 3)

    def test_combined_start_end_is_error_and_suppresses_success_events(self):
        text = read(ISP)
        isr = block_after(text, "void rkisp_isp_isr")
        decl_start = isr.find("bool frame_irq_error")
        decl_end = isr.find(";", decl_start)
        self.assertGreaterEqual(decl_start, 0)
        declaration = isr[decl_start:decl_end]
        self.assertIn("CIF_ISP_DATA_LOSS", declaration)
        self.assertIn("CIF_ISP_PIC_SIZE_ERROR", declaration)
        self.assertIn("(isp_mis & CIF_ISP_V_START)", declaration)
        self.assertIn("(isp_mis & CIF_ISP_FRAME)", declaration)
        self.assertRegex(isr, rf"{ERROR}\)")
        self.assertGreaterEqual(isr.count("!frame_irq_error"), 3)


class CifTerminalSensorRouteTests(unittest.TestCase):
    def test_public_frame_notifier_api(self):
        header = read(HEADER)
        self.assertIn("struct rk_camera_frame_event", header)
        self.assertIn("struct fwnode_handle *terminal", header)
        self.assertIn("u32 mask", header)
        self.assertIn("int rk_camera_frame_register(struct notifier_block *nb)", header)
        self.assertIn("int rk_camera_frame_unregister(struct notifier_block *nb)", header)
        self.assertIn("void rk_camera_frame_notify(struct fwnode_handle *terminal, u32 mask)", header)

    def test_frame_notifier_chain_lives_in_videodev(self):
        chain = read(FRAME_CHAIN_C)
        self.assertIn("ATOMIC_NOTIFIER_HEAD", chain)
        self.assertIn("atomic_notifier_chain_register", chain)
        self.assertIn("atomic_notifier_chain_unregister", chain)
        self.assertIn("atomic_notifier_call_chain", chain)
        self.assertIn(".terminal = terminal", chain)
        self.assertIn(".mask = mask", chain)
        self.assertIn("EXPORT_SYMBOL_GPL(rk_camera_frame_register)", chain)
        self.assertIn("EXPORT_SYMBOL_GPL(rk_camera_frame_unregister)", chain)
        self.assertIn("EXPORT_SYMBOL_GPL(rk_camera_frame_notify)", chain)
        self.assertIn("rk-camera-frame.o", read(V4L2_MAKEFILE))

    def test_update_sensor_info_caches_terminal_fwnode(self):
        update = block_after(read(CIF_CAPTURE), "int rkcif_update_sensor_info")
        self.assertIn(
            "rkcif_set_terminal_sensor_fwnode(stream->cifdev, terminal_sensor->sd->fwnode)",
            update,
        )

    def test_cif_fwnode_cache_holds_ref_and_uses_rcu_grace(self):
        setter = block_after(read(CIF_DEV_C), "void rkcif_set_terminal_sensor_fwnode")
        self.assertIn("fwnode_handle_get", setter)
        self.assertIn("spin_lock_irqsave", setter)
        self.assertIn("rcu_assign_pointer", setter)
        self.assertIn("synchronize_rcu()", setter)
        self.assertIn("fwnode_handle_put", setter)

    def test_cif_fwnode_setter_has_dying_gate_before_publish(self):
        setter = block_after(read(CIF_DEV_C), "void rkcif_set_terminal_sensor_fwnode")
        lock_pos = setter.find("spin_lock_irqsave")
        dying_pos = setter.find("terminal_sensor_dying")
        assign_pos = setter.find("rcu_assign_pointer")
        self.assertGreaterEqual(lock_pos, 0)
        self.assertGreater(dying_pos, lock_pos)
        self.assertGreater(assign_pos, dying_pos)

    def test_cif_uninit_saves_mbus_before_notifier_unregister(self):
        uninit = block_after(read(CIF_DEV_C), "int rkcif_plat_uninit")
        save = uninit.find("mbus_type = cif_dev->active_sensor->mbus.type")
        notifier = uninit.find("v4l2_async_nf_unregister(&cif_dev->notifier)")
        self.assertGreaterEqual(save, 0)
        self.assertGreater(notifier, save)
        self.assertIn("mbus_type == V4L2_MBUS_CCP2", uninit)
        self.assertIn("mbus_type == V4L2_MBUS_BT656", uninit)
        self.assertNotIn("cif_dev->active_sensor->mbus.type ==", uninit)

    def test_cif_terminal_fwnode_fields_and_lock(self):
        header = read(CIF_DEV_H)
        self.assertRegex(header, r"struct fwnode_handle\s+__rcu\s+\*terminal_sensor_fwnode")
        self.assertRegex(header, r"spinlock_t\s+terminal_fwnode_lock")
        self.assertRegex(header, r"bool\s+terminal_sensor_dying")

    def test_sditf_relay_broadcasts_fwnode_event_without_sensor_deref(self):
        callback = block_after(read(SDITF_C), "sditf_interrupt_service_routine")
        self.assertIn("rcu_dereference(priv->cif_dev->terminal_sensor_fwnode)", callback)
        self.assertIn("rk_camera_frame_notify(", callback)
        for forbidden in ("terminal_sensor_sd", "terminal_sensor.sd",
                          "interrupt_service_routine", "fwnode_property",
                          "fwnode_graph"):
            self.assertNotIn(forbidden, callback)

    def test_cif_remove_clears_fwnode_and_unregisters_notifier(self):
        uninit = block_after(read(CIF_DEV_C), "int rkcif_plat_uninit")
        clear = uninit.find("rkcif_set_terminal_sensor_fwnode(cif_dev, NULL)")
        unregister = uninit.find("v4l2_async_nf_unregister(&cif_dev->notifier)")
        cleanup = uninit.find("v4l2_async_nf_cleanup(&cif_dev->notifier)")
        self.assertGreaterEqual(clear, 0)
        self.assertGreater(unregister, clear)
        self.assertGreater(cleanup, unregister)
class InputLifecycleEvidenceTests(unittest.TestCase):
    def test_cif_input_stop_stream_disables_relay_mode(self):
        cif_stream = block_after(read(SDITF_C), "static int sditf_s_stream")
        self.assertIn("sditf_stop_stream(priv)", cif_stream)
        self.assertIn("priv->mode.rdbk_mode = RKISP_VICAP_RDBK_AIQ", cif_stream)


class SditfRelayContractTests(unittest.TestCase):
    def test_relay_callback_is_irq_safe_and_singleton_only(self):
        source = read(SDITF_C)
        callback = block_after(source, "sditf_interrupt_service_routine")
        for forbidden in ("mutex_lock", "mutex_unlock", "msleep", "usleep",
                          "i2c_", "list_for_each", "schedule_work"):
            self.assertNotIn(forbidden, callback)
        self.assertIn("status != RK_CAMERA_FRAME_START", callback)
        self.assertIn("status != RK_CAMERA_FRAME_END", callback)
        self.assertIn("status != RK_CAMERA_FRAME_ERROR", callback)
        self.assertIn("rk_camera_frame_notify(", callback)
        self.assertNotIn("terminal_sensor.sd", callback)
        self.assertNotIn("CIF_ISP_", callback)
        self.assertNotIn("s_stream", callback)

    def test_relay_registered_in_core_ops(self):
        source = read(SDITF_C)
        core_ops = block_after(source, "static const struct v4l2_subdev_core_ops sditf_core_ops")
        self.assertIn(".interrupt_service_routine = sditf_interrupt_service_routine", core_ops)


if __name__ == "__main__":
    unittest.main(verbosity=2)
