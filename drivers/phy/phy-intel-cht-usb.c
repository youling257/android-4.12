/*
 * Intel Cherrytrail USB OTG MUX driver
 *
 * Copyright (c) 2016 Hans de Goede <hdegoede@redhat.com>
 *
 * Loosely based on android x86 kernel code which is:
 *
 * Copyright (C) 2014 Intel Corp.
 *
 * Author: Wu, Hao
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program;
 */

#include <linux/acpi.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/extcon.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>

/* register definition */
#define DUAL_ROLE_CFG0			0x68
#define SW_VBUS_VALID			(1 << 24)
#define SW_IDPIN_EN			(1 << 21)
#define SW_IDPIN			(1 << 20)

#define DUAL_ROLE_CFG1			0x6c
#define HOST_MODE			(1 << 29)

#define DUAL_ROLE_CFG1_POLL_TIMEOUT	1000

#define DRV_NAME			"intel_cht_usb_phy"

enum mux_select { MUX_SEL_DEVICE, MUX_SEL_HOST };

struct intel_cht_usb_mux {
	struct device *dev;
	void __iomem *base;
	enum mux_select mux;
	struct extcon_dev *extcon;
	struct notifier_block id_nb;
	struct work_struct work;
};

struct intel_cht_usb_id_provider {
	const char *hid;
	const char *extcon;
};

struct intel_cht_usb_id_provider intel_cht_usb_id_providers[] = {
	{ "INT3496", "INT3496:00" },
	{ "INT34D3", "cht_wcove_pwrsrc" },
};

static void intel_cht_usb_mux_work(struct work_struct *work)
{
	struct intel_cht_usb_mux *mux =
		container_of(work, struct intel_cht_usb_mux, work);
	unsigned long timeout;
	u32 data;

	/* Check and set mux to SW controlled mode */
	data = readl(mux->base + DUAL_ROLE_CFG0);
	if (!(data & SW_IDPIN_EN)) {
		data |= SW_IDPIN_EN;
		writel(data, mux->base + DUAL_ROLE_CFG0);
	}

	/* Set idpin and vbus_valid as requested */
	data = readl(mux->base + DUAL_ROLE_CFG0);
	if (mux->mux == MUX_SEL_DEVICE)
		data |= (SW_IDPIN | SW_VBUS_VALID);
	else
		data &= ~(SW_IDPIN | SW_VBUS_VALID);
	writel(data, mux->base + DUAL_ROLE_CFG0);

	/* In most case it takes about 600ms to finish mode switching */
	timeout = jiffies + msecs_to_jiffies(DUAL_ROLE_CFG1_POLL_TIMEOUT);

	/* Polling on CFG1 register to confirm mode switch.*/
	while (1) {
		data = readl(mux->base + DUAL_ROLE_CFG1);
		if (mux->mux == MUX_SEL_DEVICE && !(data & HOST_MODE))
			break;
		if (mux->mux == MUX_SEL_HOST && (data & HOST_MODE))
			break;

		/* Interval for polling is set to about 5 - 10 ms */
		usleep_range(5000, 10000);

		if (time_after(jiffies, timeout)) {
			dev_warn(mux->dev, "Timeout waiting for mux to switch\n");
			break;
		}
	}
}

static int intel_cht_usb_mux_id_cable_evt(struct notifier_block *nb,
					  unsigned long event, void *param)
{
	struct intel_cht_usb_mux *mux =
		container_of(nb, struct intel_cht_usb_mux, id_nb);

	if (event == 1)
		mux->mux = MUX_SEL_HOST;
	else
		mux->mux = MUX_SEL_DEVICE;

	schedule_work(&mux->work);

	return NOTIFY_OK;
}

static ssize_t intel_cht_mode_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct intel_cht_usb_mux *mux = dev_get_drvdata(dev);

	if (mux->mux == MUX_SEL_DEVICE)
		return sprintf(buf, "device\n");
	else
		return sprintf(buf, "host\n");
}

static ssize_t intel_cht_mode_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t n)
{
	struct intel_cht_usb_mux *mux = dev_get_drvdata(dev);

	/* For debugging purposes */
	if (sysfs_streq(buf, "dumpregs")) {
		dev_info(mux->dev, "mux cfg %08x %08x\n",
			 readl(mux->base + DUAL_ROLE_CFG0),
			 readl(mux->base + DUAL_ROLE_CFG1));
		return n;
	}

	if (sysfs_streq(buf, "device"))
		mux->mux = MUX_SEL_DEVICE;
	else if (sysfs_streq(buf, "host"))
		mux->mux = MUX_SEL_HOST;
	else
		return -EINVAL;

	dev_info(mux->dev, "changing mode to %s\n", buf);
	schedule_work(&mux->work);

	return n;
}

static DEVICE_ATTR(mode, 0644, intel_cht_mode_show, intel_cht_mode_store);

static int intel_cht_usb_mux_probe(struct platform_device *pdev)
{
	struct intel_cht_usb_mux *mux;
	struct device *dev = &pdev->dev;
	struct resource *res;
	resource_size_t size;
	int i, ret;

	mux = devm_kzalloc(dev, sizeof(*mux), GFP_KERNEL);
	if (!mux)
		return -ENOMEM;

	mux->dev = dev;
	INIT_WORK(&mux->work, intel_cht_usb_mux_work);

	for (i = 0 ; i < ARRAY_SIZE(intel_cht_usb_id_providers); i++) {
		if (!acpi_dev_present(intel_cht_usb_id_providers[i].hid))
			continue;

		mux->extcon = extcon_get_extcon_dev(
					intel_cht_usb_id_providers[i].extcon);
		if (mux->extcon == NULL)
			return -EPROBE_DEFER;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	size = (res->end + 1) - res->start;
	mux->base = devm_ioremap_nocache(dev, res->start, size);
	if (IS_ERR(mux->base)) {
		ret = PTR_ERR(mux->base);
		dev_err(dev, "can't iomap registers: %d\n", ret);
		return ret;
	}

	/*
	 * mux->extcon may be NULL if no providers are present, in that case
	 * we still offer mux access through the sysfs mode attr.
	 */
	if (mux->extcon) {
		mux->id_nb.notifier_call = intel_cht_usb_mux_id_cable_evt;
		ret = devm_extcon_register_notifier(dev, mux->extcon,
					    EXTCON_USB_HOST, &mux->id_nb);
		if (ret) {
			dev_err(dev, "can't register id extcon notifier: %d\n", ret);
			return ret;
		}

		/* Sync initial mode */
		if (extcon_get_state(mux->extcon, EXTCON_USB_HOST) == 1)
			mux->mux = MUX_SEL_HOST;
		else
			mux->mux = MUX_SEL_DEVICE;

		schedule_work(&mux->work);
	}

	platform_set_drvdata(pdev, mux);
	device_create_file(dev, &dev_attr_mode);

	return 0;
}

static const struct platform_device_id intel_cht_usb_mux_table[] = {
	{ .name = DRV_NAME },
	{},
};
MODULE_DEVICE_TABLE(platform, intel_cht_usb_mux_table);

static struct platform_driver intel_cht_usb_mux_driver = {
	.driver = {
		.name = DRV_NAME,
	},
	.id_table = intel_cht_usb_mux_table,
	.probe = intel_cht_usb_mux_probe,
};

module_platform_driver(intel_cht_usb_mux_driver);

MODULE_AUTHOR("Hans de Goede <hdegoede@redhat.com>");
MODULE_DESCRIPTION("Intel Cherrytrail USB PHY driver");
MODULE_LICENSE("GPL");
