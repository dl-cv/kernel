#!/usr/bin/env python3
"""Static integration checks for the unified IMX296/AR0521 RK3576 kernel."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
DTS = ROOT / "arch/arm64/boot/dts/rockchip"


def read(rel):
    return (ROOT / rel).read_text(encoding="utf-8")


class DualSensorIntegrationTest(unittest.TestCase):
    def test_config_builds_both_sensor_drivers(self):
        config = read("arch/arm64/configs/dlcvcam_rk3576_kernel_cut.config")
        self.assertIn("CONFIG_VIDEO_IMX296=y", config)
        self.assertIn("CONFIG_VIDEO_AR0521=y", config)
        self.assertNotIn("# CONFIG_VIDEO_IMX296 is not set", config)

    def test_board_dtbs_are_explicit_and_split(self):
        makefile = read("arch/arm64/boot/dts/rockchip/Makefile")
        self.assertIn("dlcvcam-rk3576.dtb", makefile)
        self.assertIn("dlcvcam-rk3576-ar0521.dtb", makefile)

        default_dts = read("arch/arm64/boot/dts/rockchip/dlcvcam-rk3576.dts")
        ar_dts = read("arch/arm64/boot/dts/rockchip/dlcvcam-rk3576-ar0521.dts")
        self.assertIn('#include "dlcvcam-rk3576-board.dtsi"', default_dts)
        self.assertIn('#include "dlcvcam-rk3576-csi0.dtsi"', default_dts)
        self.assertIn('#include "dlcvcam-rk3576-board.dtsi"', ar_dts)
        self.assertIn('#include "dlcvcam-rk3576-csi0-ar0521.dtsi"', ar_dts)
        self.assertNotIn("ar0521@36", default_dts)

    def test_sensor_link_parameters_do_not_leak(self):
        imx = read("arch/arm64/boot/dts/rockchip/dlcvcam-rk3576-csi0.dtsi")
        ar = read("arch/arm64/boot/dts/rockchip/dlcvcam-rk3576-csi0-ar0521.dtsi")

        for expected in (
            "imx296@1a",
            "reg = <0x1a>",
            "data-lanes = <1>",
            "PWM_POLARITY_INVERTED",
            "trigger-mode = <1>",
            'trigger-path = "/sys/bus/i2c/devices/4-001a/trigger"',
        ):
            self.assertIn(expected, imx)
        for forbidden in ("ar0521@36", "data-lanes = <1 2 3 4>", "27000000"):
            self.assertNotIn(forbidden, imx)

        for expected in (
            "ar0521@36",
            "reg = <0x36>",
            "data-lanes = <1 2 3 4>",
            "clock-frequency = <27000000>",
            "pwms = <&pwm1_6ch_1 0 10000000 0>",
            "trigger-mode = <0>",
            'trigger-path = "/sys/bus/i2c/devices/4-0036/trigger"',
        ):
            self.assertIn(expected, ar)
        for forbidden in ("imx296@1a", "PWM_POLARITY_INVERTED"):
            self.assertNotIn(forbidden, ar)

    def test_build_entry_packages_both_boards(self):
        script = read("build_bootimg.sh")
        self.assertIn("--board", script)
        self.assertIn("dlcvcam-rk3576.dtb", script)
        self.assertIn("dlcvcam-rk3576-ar0521.dtb", script)
        self.assertIn("imx296", script)
        self.assertIn("ar0521", script)
        self.assertIn("boot-rk3576-", script)

    def test_trigger_gate_is_one_shared_implementation(self):
        source = read("drivers/input/misc/trigger-dev.c")
        self.assertNotIn("<<<<<<<", source)
        self.assertNotIn(">>>>>>>", source)
        self.assertEqual(source.count("bool\t\t\tenabled;"), 1)
        self.assertEqual(source.count("static int trigger_dev_set_enabled("), 1)
        self.assertNotIn("enable_mutex", source)
        self.assertIn("struct mutex\t\tconfig_mutex;", source)
        self.assertIn("struct mutex\t\tgate_mutex;", source)
        self.assertIn("synchronize_irq(tdev->irq);", source)
        self.assertEqual(source.count("&dev_attr_enable.attr"), 1)
        self.assertIn("gate_pending_dropped", source)
        self.assertIn("gate_irq_count", source)
        self.assertIn("gpiod_set_value_cansleep(tdev->output_gpiod, 0);", source)

    def test_historical_sensor_fix_anchors_remain(self):
        imx = read("drivers/media/i2c/imx296.c")
        cif = read("drivers/media/platform/rockchip/cif/dev.c")
        hw = read("drivers/media/platform/rockchip/cif/hw.c")
        ar = read("drivers/media/i2c/ar0521.c")

        self.assertIn("imx296_trigger_base_period_ns", imx)
        self.assertIn("master_fast_trigger", imx)
        self.assertIn("rkcif_store_vicap_i0clk_reset_now", cif)
        self.assertIn("rkcif_hw_reset_vicap_i0clk", hw)

        self.assertIn("AR0521_REG_RESET_RESTART", ar)
        self.assertIn("ar0521_grr_frame_irq", ar)
        self.assertIn("ar0521_config_ers_locked", ar)
        self.assertIn("AR0521_ERS_CLOSING", ar)
        self.assertIn("event->terminal != sensor->sd.fwnode", ar)

        isp = read("drivers/media/platform/rockchip/isp/rkisp.c")
        self.assertIn("rkisp_cif_input_frame_event", isp)
        self.assertIn("rkisp_stream_buf_done_early_delayed", isp)


if __name__ == "__main__":
    unittest.main()