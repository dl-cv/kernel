// SPDX-License-Identifier: GPL-2.0-only
/*
 * Minimal GD32 MCU I2C placeholder driver.
 *
 * This driver only turns a board-described GD32 MCU into a managed
 * Linux I2C device so the address is formally owned by the kernel.
 * Protocol-specific register access is intentionally left for a later
 * driver extension once the MCU command set is confirmed.
 */

#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of_device.h>

static int gd32_mcu_probe(struct i2c_client *client,
			  const struct i2c_device_id *id)
{
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EOPNOTSUPP;

	dev_info(&client->dev,
		 "GD32 MCU placeholder bound on %s at 0x%02x\n",
		 dev_name(&client->adapter->dev), client->addr);

	return 0;
}

static int gd32_mcu_remove(struct i2c_client *client)
{
	return 0;
}

static const struct of_device_id gd32_mcu_of_match[] = {
	{ .compatible = "gd,gd32-mcu" },
	{ }
};
MODULE_DEVICE_TABLE(of, gd32_mcu_of_match);

static const struct i2c_device_id gd32_mcu_id[] = {
	{ "gd32-mcu", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, gd32_mcu_id);

static struct i2c_driver gd32_mcu_driver = {
	.driver = {
		.name = "gd32-mcu",
		.of_match_table = of_match_ptr(gd32_mcu_of_match),
	},
	.probe = gd32_mcu_probe,
	.remove = gd32_mcu_remove,
	.id_table = gd32_mcu_id,
};
module_i2c_driver(gd32_mcu_driver);

MODULE_DESCRIPTION("Minimal GD32 MCU I2C placeholder driver");
MODULE_LICENSE("GPL");
