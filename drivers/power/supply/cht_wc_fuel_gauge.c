/*
 * Intel CHT Whiskey Cove Fuel Gauge driver
 * Copyright (C) 2017 Hans de Goede <hdegoede@redhat.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/acpi.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/mfd/intel_soc_pmic.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#define FG_CHARGE_NOW			0x05
#define FG_VOLTAGE_NOW			0x09
#define FG_CURRENT_NOW			0x0a
#define FG_CURRENT_AVG			0x0b
#define FG_CHARGE_FULL			0x10
#define FG_CHARGE_DESIGN		0x18
#define FG_VOLTAGE_AVG			0x19
#define FG_VOLTAGE_OCV			0x1b /* Only updated during charging */

#define PMIC_USBPATH			0x19
#define PMIC_USBPATH_BAT		BIT(0)
#define PMIC_USBPATH_NOT_VBUS		BIT(1)
#define PMIC_CHGRSTATUS			0x1a
#define PMIC_CHGRSTATUS_NOT_CHARGING	BIT(0)

#define CHT_WC_FG_PTYPE		4

struct cht_wc_fg_data {
	struct device *dev;
	/*
	 * The ACPI _CRS table contains info for 4 clients, 1 for the charger-
	 * manager part of the pmic and 3 for the actual fuel-gauge (which has
	 * 3 i2c addresses) note we use only 1 fg address/client. 
	 */
	struct i2c_client *pmic_client;
	struct i2c_client *fg_client;
	struct power_supply *battery;
	struct delayed_work changed_work;
};

static int cht_wc_fg_read(struct cht_wc_fg_data *fg, u8 reg,
			  union power_supply_propval *val, int scale,
			  int sign_extend)
{
	int ret;

	ret = i2c_smbus_read_word_data(fg->fg_client, reg);
	if (ret < 0)
		return ret;

	if (sign_extend)
		ret = sign_extend32(ret, 15);

	val->intval = ret * scale;

	return 0;
}

static int cht_wc_fg_get_status(struct cht_wc_fg_data *fg,
				union power_supply_propval *val)
{
	int ret;

	ret = i2c_smbus_read_byte_data(fg->pmic_client, PMIC_USBPATH);
	if (ret < 0)
		return ret;

	if (ret & PMIC_USBPATH_NOT_VBUS) {
		val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
		return 0;
	}

	ret = i2c_smbus_read_byte_data(fg->pmic_client, PMIC_CHGRSTATUS);
	if (ret < 0)
		return ret;

	/* Not charging while we have Vbus means the battery is full */
	if (ret & PMIC_CHGRSTATUS_NOT_CHARGING)
		val->intval = POWER_SUPPLY_STATUS_FULL;
	else
		val->intval = POWER_SUPPLY_STATUS_CHARGING;

	return 0;
}

static int cht_wc_fg_get_online(struct cht_wc_fg_data *fg,
				union power_supply_propval *val)
{
	int ret;

	ret = i2c_smbus_read_byte_data(fg->pmic_client, PMIC_USBPATH);
	if (ret < 0)
		return ret;

	val->intval = !!(ret & PMIC_USBPATH_BAT);

	return 0;
}

static int cht_wc_fg_get_property(struct power_supply *psy,
	enum power_supply_property prop, union power_supply_propval *val)
{
	struct cht_wc_fg_data *fg = power_supply_get_drvdata(psy);
	int ret = 0;

	switch (prop) {
	case POWER_SUPPLY_PROP_STATUS:
		ret = cht_wc_fg_get_status(fg, val);
		break;
	case POWER_SUPPLY_PROP_ONLINE:
		ret = cht_wc_fg_get_online(fg, val);
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LION;
		ret = 0;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		ret = cht_wc_fg_read(fg, FG_VOLTAGE_NOW, val, 75, 0);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_AVG:
		ret = cht_wc_fg_read(fg, FG_VOLTAGE_AVG, val, 75, 0);
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_OCV:
		ret = cht_wc_fg_read(fg, FG_VOLTAGE_OCV, val, 75, 0);
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		ret = cht_wc_fg_read(fg, FG_CURRENT_NOW, val, 150, 1);
		break;
	case POWER_SUPPLY_PROP_CURRENT_AVG:
		ret = cht_wc_fg_read(fg, FG_CURRENT_AVG, val, 150, 1);
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		ret = cht_wc_fg_read(fg, FG_CHARGE_DESIGN, val, 500, 0);
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		ret = cht_wc_fg_read(fg, FG_CHARGE_FULL, val, 500, 0);
		break;
	case POWER_SUPPLY_PROP_CHARGE_NOW:
		ret = cht_wc_fg_read(fg, FG_CHARGE_NOW, val, 500, 0);
		break;
	case POWER_SUPPLY_PROP_SCOPE:
		val->intval = POWER_SUPPLY_SCOPE_SYSTEM;
		ret = 0;
		break;
	default:
		ret = -ENODATA;
	}

	return ret;
}

static void cht_wc_fg_changed_work(struct work_struct *work)
{
	struct cht_wc_fg_data *fg =
		container_of(work, struct cht_wc_fg_data, changed_work.work);

	power_supply_changed(fg->battery);
}

static void cht_wc_fg_external_power_changed(struct power_supply *psy)
{
	struct cht_wc_fg_data *fg = power_supply_get_drvdata(psy);

	/* Wait a bit to allow the fuel-gauge to also detect the new status */
	queue_delayed_work(system_wq, &fg->changed_work, msecs_to_jiffies(200));
}

static enum power_supply_property cht_wc_fg_properties[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_AVG,
	POWER_SUPPLY_PROP_VOLTAGE_OCV,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CURRENT_AVG,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_SCOPE,
};

static const struct power_supply_desc bat_desc = {
	/* Matches charger.supplied_to for external_power_changed callback */
	.name			= "main-battery",
	.type			= POWER_SUPPLY_TYPE_BATTERY,
	.properties		= cht_wc_fg_properties,
	.num_properties		= ARRAY_SIZE(cht_wc_fg_properties),
	.get_property		= cht_wc_fg_get_property,
	.external_power_changed = cht_wc_fg_external_power_changed,
};

static int cht_wc_fg_probe(struct i2c_client *client,
			const struct i2c_device_id *i2c_id)
{
	struct device *dev = &client->dev;
	struct power_supply_config bat_cfg = {};
	struct cht_wc_fg_data *fg;
	acpi_status status;
	unsigned long long ptyp;

	fg = devm_kzalloc(dev, sizeof(*fg), GFP_KERNEL);
	if (!fg)
		return -ENOMEM;

	status = acpi_evaluate_integer(ACPI_HANDLE(dev), "PTYP", NULL, &ptyp);
	if (ACPI_FAILURE(status)) {
		dev_err(dev, "Failed to get PTYPE\n");
		return -ENODEV;
	}

	/*
	 * The same ACPI HID is used with different PMICs check PTYP to
	 * ensure that we are dealing with a Whiskey Cove PMIC.
	 */
	if (ptyp != CHT_WC_FG_PTYPE)
		return -ENODEV;

	fg->dev = dev;
	fg->pmic_client = client;
	INIT_DELAYED_WORK(&fg->changed_work, cht_wc_fg_changed_work);

	/*
	 * The current resource settings table for the fuel gauge contains
	 * multiple i2c devices on 2 different i2c-busses.
	 */
	fg->fg_client = i2c_acpi_new_device(dev, 1);
	if (!fg->fg_client)
		return -EPROBE_DEFER;

	bat_cfg.drv_data = fg;
	fg->battery = devm_power_supply_register(dev, &bat_desc, &bat_cfg);
	if (IS_ERR(fg->battery)) {
		i2c_unregister_device(fg->fg_client);
		return PTR_ERR(fg->battery);
	}

	i2c_set_clientdata(client, fg);

	return 0;
}

static int cht_wc_fg_remove(struct i2c_client *i2c)
{
	struct cht_wc_fg_data *fg = i2c_get_clientdata(i2c);

	i2c_unregister_device(fg->fg_client);

	return 0;
}

static const struct i2c_device_id cht_wc_fg_i2c_id[] = {
	{ }
};
MODULE_DEVICE_TABLE(i2c, cht_wc_fg_i2c_id);

static const struct acpi_device_id cht_wc_fg_acpi_ids[] = {
	{ "INT33FE", },
	{ }
};
MODULE_DEVICE_TABLE(acpi, cht_wc_fg_acpi_ids);

static struct i2c_driver cht_wc_fg_driver = {
	.driver	= {
		.name	= "CHT Whiskey Cove PMIC Fuel Gauge",
		.acpi_match_table = ACPI_PTR(cht_wc_fg_acpi_ids),
	},
	.probe = cht_wc_fg_probe,
	.remove = cht_wc_fg_remove,
	.id_table = cht_wc_fg_i2c_id,
	.irq_index = 1,
};

module_i2c_driver(cht_wc_fg_driver);

MODULE_DESCRIPTION("Intel CHT Whiskey Cove PMIC Fuel Gauge driver");
MODULE_AUTHOR("Hans de Goede <hdegoede@redhat.com>");
MODULE_LICENSE("GPL");
