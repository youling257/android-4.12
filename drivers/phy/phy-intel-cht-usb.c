/*
 * Intel Cherrytrail USB OTG PHY driver
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

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/extcon.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/phy/phy.h>
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

#define AXP288_EXTCON_DEV_NAME		"axp288_extcon"
#define USB_HOST_EXTCON_DEV_NAME	"INT3496:00"

#define DRV_NAME			"intel_cht_usb_phy"

static const unsigned int vbus_cable_ids[] = {
	EXTCON_CHG_USB_SDP, EXTCON_CHG_USB_CDP, EXTCON_CHG_USB_DCP };
/* Strings matching the phy_mode enum labels */
static const char * const modestr[] = { "invalid", "host", "device", "otg" };

struct intel_cht_usb_phy {
	struct device *dev;
	void __iomem *base;
	struct phy *phy;
	enum phy_mode mode;
	struct extcon_dev *id_extcon;
	struct extcon_dev *vbus_extcon;
	struct notifier_block id_nb;
	struct notifier_block vbus_nb[ARRAY_SIZE(vbus_cable_ids)];
	struct work_struct work;
};

void intel_cht_usb_phy_mux_switch(struct intel_cht_usb_phy *phy,
				  bool idpin, bool vbus_valid)
{
	unsigned long timeout;
	u32 data;

	/* Check and set mux to SW controlled mode */
	data = readl(phy->base + DUAL_ROLE_CFG0);
	if (!(data & SW_IDPIN_EN)) {
		data |= SW_IDPIN_EN;
		writel(data, phy->base + DUAL_ROLE_CFG0);
	}

	/* Set idpin and vbus_valid as requested */
	data = readl(phy->base + DUAL_ROLE_CFG0);
	data &= ~(SW_IDPIN | SW_VBUS_VALID);
	data |= idpin ? SW_IDPIN : 0;
	data |= vbus_valid ? SW_VBUS_VALID : 0;
	writel(data, phy->base + DUAL_ROLE_CFG0);

	/* In most case it takes about 600ms to finish mode switching */
	timeout = jiffies + msecs_to_jiffies(DUAL_ROLE_CFG1_POLL_TIMEOUT);

	/* Polling on CFG1 register to confirm mode switch.*/
	while (1) {
		data = readl(phy->base + DUAL_ROLE_CFG1);
		/* idpin == 1 selects device-mode */
		if (idpin && !(data & HOST_MODE))
			break;
		/* idpin == 0 selects host-mode */
		if (!idpin && (data & HOST_MODE))
			break;

		/* Interval for polling is set to about 5 - 10 ms */
		usleep_range(5000, 10000);

		if (time_after(jiffies, timeout)) {
			dev_warn(phy->dev, "Timeout waiting for mux to switch\n");
			break;
		}
	}

	dev_dbg(phy->dev, "set idpin %d vbus_valid %d\n", idpin, vbus_valid);
}

static bool intel_cht_usb_phy_get_vbus_valid(struct intel_cht_usb_phy *phy)
{
	int i, ret;

	for (i = 0; i < ARRAY_SIZE(vbus_cable_ids); i++) {
		ret = extcon_get_state(phy->vbus_extcon, vbus_cable_ids[i]);
		if (ret > 0)
			return true;
	}

	return false;
}

static void intel_cht_usb_phy_work(struct work_struct *work)
{
	struct intel_cht_usb_phy *phy =
		container_of(work, struct intel_cht_usb_phy, work);
	bool idpin, vbus_valid;
	u32 data;

	/* In host-mode idpin == 0 */
	idpin = extcon_get_state(phy->id_extcon, EXTCON_USB_HOST) <= 0;
	vbus_valid = intel_cht_usb_phy_get_vbus_valid(phy);

	dev_dbg(phy->dev, "det idpin %d vbus_valid %d\n", idpin, vbus_valid);

	/*
	 * On some boards the 5v boost converter is hardwired to the idpin.
	 * So the idpin value always specifies which side is supplying Vbus
	 * independent of the requested data direction / mode;
	 * and in order for the pmic to properly detect the type of connected
	 * charger, the data pins *must* be muxed to the device controller.
	 * This means that if the idpin is high (device / charge mode) we
	 * must keep the data pins muxed to the device controller until
	 * the pmic is done with its charger detection and vbus_valid
	 * becomes true.
	 */
	if (idpin && !vbus_valid) {
		intel_cht_usb_phy_mux_switch(phy, idpin, vbus_valid);
		return;
	}

	switch (phy->mode) {
	case PHY_MODE_USB_HOST:
		data = readl(phy->base + DUAL_ROLE_CFG0);
		/*
		 * If we are transitioning from both id + vbus valid true,
		 * first set vbus_valid to false.
		 */
		if ((data & (SW_IDPIN | SW_VBUS_VALID)) ==
			    (SW_IDPIN | SW_VBUS_VALID)) {
			intel_cht_usb_phy_mux_switch(phy, true, false);
			msleep(200);
		}
		intel_cht_usb_phy_mux_switch(phy, false, false);
		break;
	case PHY_MODE_USB_DEVICE:
		intel_cht_usb_phy_mux_switch(phy, true, true);
		break;
	case PHY_MODE_USB_OTG:
	default:
		intel_cht_usb_phy_mux_switch(phy, idpin, vbus_valid);
	}
}

/*
 * We need 3 copies of this, because there is no way to find out for which
 * cable id we are being called from the passed in arguments; and we must
 * have a separate nb for each extcon_register_notifier call.
 */
static int intel_cht_usb_phy_vbus_cable0_evt(struct notifier_block *nb,
					     unsigned long event, void *param)
{
	struct intel_cht_usb_phy *phy =
		container_of(nb, struct intel_cht_usb_phy, vbus_nb[0]);
	schedule_work(&phy->work);
	return NOTIFY_OK;
}

static int intel_cht_usb_phy_vbus_cable1_evt(struct notifier_block *nb,
					     unsigned long event, void *param)
{
	struct intel_cht_usb_phy *phy =
		container_of(nb, struct intel_cht_usb_phy, vbus_nb[1]);
	schedule_work(&phy->work);
	return NOTIFY_OK;
}

static int intel_cht_usb_phy_vbus_cable2_evt(struct notifier_block *nb,
					     unsigned long event, void *param)
{
	struct intel_cht_usb_phy *phy =
		container_of(nb, struct intel_cht_usb_phy, vbus_nb[2]);
	schedule_work(&phy->work);
	return NOTIFY_OK;
}

static int intel_cht_usb_phy_id_cable_evt(struct notifier_block *nb,
					  unsigned long event, void *param)
{
	struct intel_cht_usb_phy *phy =
		container_of(nb, struct intel_cht_usb_phy, id_nb);
	schedule_work(&phy->work);
	return NOTIFY_OK;
}

static int intel_cht_usb_phy_set_mode(struct phy *_phy, enum phy_mode mode)
{
	struct intel_cht_usb_phy *phy = phy_get_drvdata(_phy);

	phy->mode = mode;
	schedule_work(&phy->work);

	return 0;
}

static const struct phy_ops intel_cht_usb_phy_ops = {
	.set_mode	= intel_cht_usb_phy_set_mode,
	.owner		= THIS_MODULE,
};

static ssize_t intel_cht_mode_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct intel_cht_usb_phy *phy = dev_get_drvdata(dev);

	return sprintf(buf, "%s\n", modestr[phy->mode]);
}

static ssize_t intel_cht_mode_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t n)
{
	struct intel_cht_usb_phy *phy = dev_get_drvdata(dev);
	int i;

	for (i = 0; i < ARRAY_SIZE(modestr); i++) {
		if (sysfs_streq(buf, modestr[i])) {
			dev_info(phy->dev, "changing mode to %s\n", modestr[i]);
			intel_cht_usb_phy_set_mode(phy->phy, i);
			return n;
		}
	}

	return -EINVAL;
}
static DEVICE_ATTR(mode, 0644, intel_cht_mode_show, intel_cht_mode_store);

static int intel_cht_usb_phy_probe(struct platform_device *pdev)
{
	struct intel_cht_usb_phy *phy;
	struct device *dev = &pdev->dev;
	struct resource *res;
	resource_size_t size;
	int i, ret;

	phy = devm_kzalloc(dev, sizeof(*phy), GFP_KERNEL);
	if (!phy)
		return -ENOMEM;

	phy->dev = dev;
	phy->mode = PHY_MODE_USB_OTG;
	INIT_WORK(&phy->work, intel_cht_usb_phy_work);
	platform_set_drvdata(pdev, phy);

	phy->id_extcon = extcon_get_extcon_dev(USB_HOST_EXTCON_DEV_NAME);
	if (phy->id_extcon == NULL) {
		dev_dbg(dev, "id_extcon is not ready, probe deferred\n");
		return -EPROBE_DEFER;
	}

	phy->vbus_extcon = extcon_get_extcon_dev(AXP288_EXTCON_DEV_NAME);
	if (phy->vbus_extcon == NULL) {
		dev_dbg(dev, "vbus_extcon is not ready, probe deferred\n");
		return -EPROBE_DEFER;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	size = (res->end + 1) - res->start;
	phy->base = devm_ioremap_nocache(dev, res->start, size);
	if (IS_ERR(phy->base)) {
		ret = PTR_ERR(phy->base);
		dev_err(dev, "can't iomap registers: %d\n", ret);
		return ret;
	}

	phy->phy = devm_phy_create(dev, NULL, &intel_cht_usb_phy_ops);
	if (IS_ERR(phy->phy)) {
		ret = PTR_ERR(phy->phy);
		dev_err(dev, "can't create PHY: %d\n", ret);
		return ret;
	}
	phy_set_drvdata(phy->phy, phy);

	/* Register for id notification */
	phy->id_nb.notifier_call = intel_cht_usb_phy_id_cable_evt;
	ret = devm_extcon_register_notifier(dev, phy->id_extcon,
					    EXTCON_USB_HOST, &phy->id_nb);
	if (ret) {
		dev_err(dev, "can't register id extcon notifier: %d\n", ret);
		return ret;
	}

	/* Register for vbus notification */
	phy->vbus_nb[0].notifier_call = intel_cht_usb_phy_vbus_cable0_evt;
	phy->vbus_nb[1].notifier_call = intel_cht_usb_phy_vbus_cable1_evt;
	phy->vbus_nb[2].notifier_call = intel_cht_usb_phy_vbus_cable2_evt;
	for (i = 0; i < ARRAY_SIZE(vbus_cable_ids); i++) {
		ret = devm_extcon_register_notifier(dev, phy->vbus_extcon,
					vbus_cable_ids[i], &phy->vbus_nb[i]);
		if (ret) {
			dev_err(dev, "can't register extcon notifier for %u: %d\n",
				vbus_cable_ids[i], ret);
			return ret;
		}
	}

	/* Get and process initial cable states */
	schedule_work(&phy->work);

	device_create_file(dev, &dev_attr_mode);

	return phy_create_lookup(phy->phy, "dwc3.0", "usb3-phy");
}

static int intel_cht_usb_phy_remove(struct platform_device *pdev)
{
	struct intel_cht_usb_phy *phy = platform_get_drvdata(pdev);

	phy_remove_lookup(phy->phy, "dwc3.0", "usb3-phy");
	device_remove_file(phy->dev, &dev_attr_mode);

	return 0;
}

static const struct platform_device_id intel_cht_usb_phy_table[] = {
	{ .name = DRV_NAME },
	{},
};
MODULE_DEVICE_TABLE(platform, intel_cht_usb_phy_table);

static struct platform_driver intel_cht_usb_phy_driver = {
	.driver = {
		.name = DRV_NAME,
	},
	.id_table = intel_cht_usb_phy_table,
	.probe = intel_cht_usb_phy_probe,
	.remove	= intel_cht_usb_phy_remove,
};

module_platform_driver(intel_cht_usb_phy_driver);

MODULE_AUTHOR("Hans de Goede <hdegoede@redhat.com>");
MODULE_DESCRIPTION("Intel Cherrytrail USB PHY driver");
MODULE_LICENSE("GPL");
