/*
 * Silead touchscreen driver DMI based configuration code
 *
 * Copyright (c) 2017 Red Hat Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Red Hat authors:
 * Hans de Goede <hdegoede@redhat.com>
 */

#include <linux/device.h>
#include <linux/dmi.h>
#include <linux/i2c.h>
#include <linux/property.h>

static const struct property_entry cube_iwork8_air_props[] = {
	PROPERTY_ENTRY_U32("touchscreen-size-x", 1660),
	PROPERTY_ENTRY_U32("touchscreen-size-y", 900),
	PROPERTY_ENTRY_BOOL("touchscreen-swapped-x-y"),
	PROPERTY_ENTRY_STRING("firmware-name", "gsl3670-cube-iwork8-air.fw"),
	PROPERTY_ENTRY_U32("silead,max-fingers", 10),
	{ }
};

static const struct property_entry jumper_ezpad_mini3_props[] = {
	PROPERTY_ENTRY_U32("touchscreen-size-x", 1700),
	PROPERTY_ENTRY_U32("touchscreen-size-y", 1150),
	PROPERTY_ENTRY_BOOL("touchscreen-swapped-x-y"),
	PROPERTY_ENTRY_STRING("firmware-name", "gsl3676-jumper-ezpad-mini3.fw"),
	PROPERTY_ENTRY_U32("silead,max-fingers", 10),
	{ }
};

static const struct dmi_system_id silead_ts_dmi_table[] = {
	{
	 .ident = "CUBE iwork8 Air",
	 .driver_data = (void *)&cube_iwork8_air_props,
	 .matches = {
		DMI_MATCH(DMI_SYS_VENDOR, "cube"),
		DMI_MATCH(DMI_PRODUCT_NAME, "i1-TF"),
		DMI_MATCH(DMI_BOARD_NAME, "Cherry Trail CR"),
		},
	},
	{
	 .ident = "Jumper EZpad mini3",
	 .driver_data = (void *)&jumper_ezpad_mini3_props,
	 .matches = {
		DMI_MATCH(DMI_SYS_VENDOR, "Insyde"),
		/* jumperx.T87.KFBNEEA02 with the version-nr dropped */
		DMI_MATCH(DMI_BIOS_VERSION, "jumperx.T87.KFBNEEA"),
		},
	},
	{ },
};

void silead_ts_dmi_add_props(struct i2c_client *client)
{
	const struct dmi_system_id *dmi_id;
	int ret;

	dmi_id = dmi_first_match(silead_ts_dmi_table);
	if (dmi_id) {
		ret = device_add_properties(&client->dev, dmi_id->driver_data);
		if (ret)
			dev_err(&client->dev, "Add properties error %d\n", ret);
	}
}
