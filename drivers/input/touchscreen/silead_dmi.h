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

#ifndef _SILEAD_DMI_H_
#define _SILEAD_DMI_H_

#include <linux/i2c.h>

#ifdef CONFIG_DMI
void silead_ts_dmi_add_props(struct i2c_client *client);
#else
static inline void silead_ts_dmi_add_props(struct i2c_client *client) {}
#endif

#endif
