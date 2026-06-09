// SPDX-License-Identifier: GPL-2.0+
/*
 * Driver for Maxio PHYs
 *
 * Copyright (c) 2021 maxio technology, Inc.
 *
 * Modified for DLCVCAM RK3576 — LED config adjusted for dual-LED RJ45
 */

#include <linux/bitops.h>
#include <linux/phy.h>
#include <linux/module.h>
#include <linux/delay.h>

#define MAXIO_PAGE_SELECT			0x1f

/* LED Control Register (Page 0xD04, Address 0x10) */
#define MAXIO_MAE0621A_LCR_LED0_ACT		BIT(4)
#define MAXIO_MAE0621A_LCR_LED0_LINK1000	BIT(3)
#define MAXIO_MAE0621A_LCR_LED0_LINK100		BIT(1)
#define MAXIO_MAE0621A_LCR_LED0_LINK10		BIT(0)

#define MAXIO_MAE0621A_LCR_LED1_ACT		BIT(9)
#define MAXIO_MAE0621A_LCR_LED1_LINK1000	BIT(8)
#define MAXIO_MAE0621A_LCR_LED1_LINK100		BIT(6)
#define MAXIO_MAE0621A_LCR_LED1_LINK10		BIT(5)

#define MAXIO_MAE0621A_LCR_LED2_ACT		BIT(14)
#define MAXIO_MAE0621A_LCR_LED2_LINK1000	BIT(13)
#define MAXIO_MAE0621A_LCR_LED2_LINK100		BIT(11)
#define MAXIO_MAE0621A_LCR_LED2_LINK10		BIT(10)

static int maxio_write_paged(struct phy_device *phydev, int page, u32 regnum, u16 val)
{
	int ret = 0, oldpage;

	oldpage = phy_read(phydev, MAXIO_PAGE_SELECT);
	if (oldpage >= 0) {
		phy_write(phydev, MAXIO_PAGE_SELECT, page);
		ret = phy_write(phydev, regnum, val);
	}
	phy_write(phydev, MAXIO_PAGE_SELECT, oldpage);
	return ret;
}

/*
 * LED configuration for DLCVCAM RK3576 dual-LED RJ45:
 *   PHY LED1 / CFG_LDO0  →  RJ45 Green LED  (Activity)
 *   PHY LED2 / CFG_LDO1  →  RJ45 Yellow LED (Link)
 *   PHY LED0 / CFG_EXT   →  not connected to RJ45
 */
static void maxio_mae0621a_led_init(struct phy_device *phydev)
{
	u16 led_val = 0;

	/* LED0 — not connected, leave default */
	led_val |= MAXIO_MAE0621A_LCR_LED0_LINK10;

	/* LED1 (Green) — Link 1000Mbps + Activity at 1000Mbps */
	led_val |= MAXIO_MAE0621A_LCR_LED1_LINK1000;
	led_val |= MAXIO_MAE0621A_LCR_LED1_ACT;

	/* LED2 (Yellow) — Link indicator for 10/100/1000 Mbps (steady on) */
	led_val |= MAXIO_MAE0621A_LCR_LED2_LINK10;
	led_val |= MAXIO_MAE0621A_LCR_LED2_LINK100;
	led_val |= MAXIO_MAE0621A_LCR_LED2_LINK1000;

	maxio_write_paged(phydev, 0xd04, 0x10, led_val);
}

static int maxio_mae0621aq3ci_config_init(struct phy_device *phydev)
{
	int ret = 0;

	/* MDC timing set (from vendor driver v1.8.1.2) */
	ret = maxio_write_paged(phydev, 0xdab, 0x17, 0xf13);

	/* CLKOUT 125 MHz (from vendor driver) */
	ret |= maxio_write_paged(phydev, 0xa43, 0x19, 0x823);

	/* LED configuration for dual-LED RJ45 */
	maxio_mae0621a_led_init(phydev);

	/* Soft reset */
	ret |= maxio_write_paged(phydev, 0x0, 0x0, 0x9140);

	/* Back to page 0 */
	ret |= phy_write(phydev, MAXIO_PAGE_SELECT, 0);

	return ret;
}

static int maxio_mae0621aq3ci_resume(struct phy_device *phydev)
{
	return genphy_resume(phydev);
}

static int maxio_mae0621aq3ci_suspend(struct phy_device *phydev)
{
	return genphy_suspend(phydev);
}

static struct phy_driver maxio_phy_drvs[] = {
	{
		.phy_id		= 0x7b744412,
		.phy_id_mask	= 0x7fffffff,
		.name		= "MAE0621A-Q3C Gigabit Ethernet",
		.features	= PHY_GBIT_FEATURES,
		.config_init	= maxio_mae0621aq3ci_config_init,
		.suspend	= maxio_mae0621aq3ci_suspend,
		.resume		= maxio_mae0621aq3ci_resume,
	},
};

module_phy_driver(maxio_phy_drvs);

static struct mdio_device_id __maybe_unused maxio_tbl[] = {
	{ 0x7b744412, 0x7fffffff },
	{ }
};

MODULE_DEVICE_TABLE(mdio, maxio_tbl);

MODULE_DESCRIPTION("Maxio PHY driver");
MODULE_AUTHOR("Zhao Yang / modified for DLCVCAM");
MODULE_LICENSE("GPL");
