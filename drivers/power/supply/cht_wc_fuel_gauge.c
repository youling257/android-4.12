/*
 * Intel CHT Whiskey Cove Fuel Gauge driver
 * Copyright (C) 2017 Hans de Goede <hdegoede@redhat.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/mfd/intel_soc_pmic.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/power/bq24190_charger.h>
#include <linux/slab.h>

/*
 * Cherrytrail Whiskey Cove devices have 2 functional blocks which interact
 * with the battery.
 *
 * 1) The fuel-gauge which is build into the Whiskey Cove PMIC, but has its
 * own i2c bus and i2c client addresses separately from the rest of the PMIC.
 * That block is what this driver is for.
 *
 * 2) An external charger IC, which is connected to the SMBUS controller
 * which is part of the rest of the Whiskey Cove PMIC, mfd/intel_cht_wc.c
 * registers a platform device for the SMBUS controller and
 * i2c/busses/i2c-cht-wc.c contains the i2c-adapter driver for this.
 *
 * However we want to present this as a single power_supply device to
 * userspace. So this driver offers a callback to get the fuel-gauge
 * power_supply properties, which gets passed to the external charger driver
 * via platform_data.
 *
 * To be able to pass the platform_data this file also contains a driver for
 * the "cht_wcove_ext_charger" registered by the mfd driver, gets the irq
 * for the external-charger from there and registers the i2c client passing in
 * the platform_data with the callback for the extra power_supply properties.
 */

/* The CHT-WC i2c-adapter has a fixed nr, so that this driver can find it */
#define CHT_WC_I2C_ADAPTER_NR		63


/******** "cht_wcove_ext_charger" driver ********/
static const struct bq24190_platform_data bq24190_pdata = {
	.no_register_reset = true,
};

static int cht_wc_ext_ch_i2c_probe(struct platform_device *pdev)
{
	struct i2c_adapter *adap;
	struct i2c_client *client;
	struct i2c_board_info board_info = {
		.type = "bq24190",
		.addr = 0x6b,
		.platform_data = (void *)&bq24190_pdata,
	};

	board_info.irq = platform_get_irq(pdev, 0);
	if (board_info.irq < 0) {
		dev_err(&pdev->dev, "Error missing irq resource\n");
		return -ENODEV;
	}

	adap = i2c_get_adapter(CHT_WC_I2C_ADAPTER_NR);
	if (!adap)
		return -EPROBE_DEFER;

	client = i2c_new_device(adap, &board_info);
	i2c_put_adapter(adap);
	if (!client)
		return -ENODEV;

	platform_set_drvdata(pdev, client);
	return 0;
}

static int cht_wc_ext_ch_i2c_remove(struct platform_device *pdev)
{
	struct i2c_client *client = platform_get_drvdata(pdev);

	i2c_unregister_device(client);

	return 0;
}

struct platform_driver cht_wc_ext_ch_driver = {
	.probe = cht_wc_ext_ch_i2c_probe,
	.remove = cht_wc_ext_ch_i2c_remove,
	.driver = {
		.name = "cht_wcove_ext_charger",
	},
};
module_platform_driver(cht_wc_ext_ch_driver);

MODULE_DESCRIPTION("Intel CHT Whiskey Cove PMIC I2C Master driver");
MODULE_AUTHOR("Hans de Goede <hdegoede@redhat.com>");
MODULE_LICENSE("GPL");
