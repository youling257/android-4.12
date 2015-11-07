/*
 * DRI driver for mali400 GPU
 * (C) Copyright 2015 Hans de Goede <hdegoede@redhat.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/err.h>

#include <linux/clk.h>
#include <linux/gpio.h>
#include <linux/platform_device.h>
#include <linux/reset.h>

#include <linux/of_address.h>
#include <linux/of_gpio.h>
#include <linux/of_platform.h>

struct mali400_data {
	void __iomem		*reg_base;
	struct reset_control	*reset;
	struct clk		*clk_ahb;
	struct clk		*clk_module;
};

static const struct of_device_id mali400_of_match[] = {
	{ .compatible = "arm,mali400", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mali400_of_match);

static int mali400_resource_request(struct mali400_data *mali400,
				    struct platform_device *pdev)
{
	int ret;

	mali400->reg_base = devm_ioremap_resource(&pdev->dev,
			      platform_get_resource(pdev, IORESOURCE_MEM, 0));
	if (IS_ERR(mali400->reg_base))
		return PTR_ERR(mali400->reg_base);

	mali400->clk_ahb = devm_clk_get(&pdev->dev, "ahb");
	if (IS_ERR(mali400->clk_ahb)) {
		dev_err(&pdev->dev, "Could not get ahb clock\n");
		return PTR_ERR(mali400->clk_ahb);
	}

	mali400->clk_module = devm_clk_get(&pdev->dev, "mali400");
	if (IS_ERR(mali400->clk_module)) {
		dev_err(&pdev->dev, "Could not get mali400 clock\n");
		return PTR_ERR(mali400->clk_module);
	}

	mali400->reset = devm_reset_control_get(&pdev->dev, NULL);
	if (IS_ERR(mali400->reset))
		return PTR_ERR(mali400->reset);

	ret = clk_prepare_enable(mali400->clk_ahb);
	if (ret) {
		dev_err(&pdev->dev, "Enable ahb clk err %d\n", ret);
		return ret;
	}

	ret = clk_prepare_enable(mali400->clk_module);
	if (ret) {
		dev_err(&pdev->dev, "Enable mali400 clk err %d\n", ret);
		goto error_disable_clk_ahb;
	}

	ret = reset_control_deassert(mali400->reset);
	if (ret) {
		dev_err(&pdev->dev, "reset err %d\n", ret);
		goto error_disable_clk_module;
	}

	return 0;

error_disable_clk_module:
	clk_disable_unprepare(mali400->clk_module);
error_disable_clk_ahb:
	clk_disable_unprepare(mali400->clk_ahb);
	return ret;
}

static int mali400_probe(struct platform_device *pdev)
{
	struct mali400_data *mali400;
	int ret;

	mali400 = devm_kzalloc(&pdev->dev, sizeof(struct mali400_data),
			       GFP_KERNEL);
	if (!mali400)
		return -ENOMEM;

	ret = mali400_resource_request(mali400, pdev);
	if (ret)
		return ret;

	dev_info(&pdev->dev, "base:0x%p\n", mali400->reg_base);

	platform_set_drvdata(pdev, mali400);

	return 0;
}

static int mali400_remove(struct platform_device *pdev)
{
	struct mali400_data *mali400 = platform_get_drvdata(pdev);

	reset_control_assert(mali400->reset);
	clk_disable_unprepare(mali400->clk_module);
	clk_disable_unprepare(mali400->clk_ahb);

	return 0;
}

static struct platform_driver mali400_driver = {
	.driver = {
		.name	= "mali400",
		.of_match_table = of_match_ptr(mali400_of_match),
	},
	.probe		= mali400_probe,
	.remove		= mali400_remove,
};
module_platform_driver(mali400_driver);

MODULE_DESCRIPTION("Mali400 GPU driver");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Hans de Goede <hdegoede@redhat.com>");
