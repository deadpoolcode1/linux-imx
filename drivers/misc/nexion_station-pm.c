/*
 * Copyright (C) 2019 Kamacode.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/err.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>

struct nexion_station {
	int	gpo1;
	int	gpo2;
	int	gpo3;
	int	gpo4;
	int	gpo5;
	int	gpo6;
	int	gpo7;
	int	gpo8;
	int	gpo9;
	int	gpi1;
	int	gpi2;
	int	gpi3;

};

static int nexion_station_request_export(struct device *dev, int *gpio, int flags,
	const char *name_of, const char *name)
{
	struct device_node *np = dev->of_node;
	int ret;

	*gpio = of_get_named_gpio(np, name_of, 0);
	if (!gpio_is_valid(*gpio)) {
		dev_err(dev, "GPIO for pin %s is not valid\n", name_of);
		return -EINVAL;
	}

	ret = devm_gpio_request_one(dev, *gpio, flags, name);
	if (ret) {
		dev_err(dev, "Failed to request GPIO %d, error %d\n",
			*gpio, ret);
		return ret;
	}
	gpio_export(*gpio, 1);
	gpio_export_link(dev, name, *gpio);

	return 0;
}

static int nexion_station_parse_dt(struct device *dev, struct nexion_station *priv)
{
	int ret;

	ret = nexion_station_request_export(dev, &priv->gpo1,
		GPIOF_OUT_INIT_LOW, "gpo1-gpio", "gpo1");
	if (ret)
		return ret;
	ret = nexion_station_request_export(dev, &priv->gpo2,
		GPIOF_OUT_INIT_LOW, "gpo2-gpio", "gpo2");
	if (ret)
		return ret;
	ret = nexion_station_request_export(dev, &priv->gpo3,
		GPIOF_OUT_INIT_LOW, "gpo3-gpio", "gpo3");
	if (ret)
		return ret;
	ret = nexion_station_request_export(dev, &priv->gpo4,
		GPIOF_OUT_INIT_LOW, "gpo4-gpio", "gpo4");
	if (ret)
		return ret;
	ret = nexion_station_request_export(dev, &priv->gpo5,
		GPIOF_OUT_INIT_LOW, "gpo5-gpio", "gpo5");
	if (ret)
		return ret;
	ret = nexion_station_request_export(dev, &priv->gpo6,
		GPIOF_OUT_INIT_LOW, "gpo6-gpio", "gpo6");
	if (ret)
		return ret;
	ret = nexion_station_request_export(dev, &priv->gpo7,
		GPIOF_OUT_INIT_LOW, "gpo7-gpio", "gpo7");
	if (ret)
		return ret;
	ret = nexion_station_request_export(dev, &priv->gpo8,
		GPIOF_OUT_INIT_LOW, "gpo8-gpio", "gpo8");
	if (ret)
		return ret;
	ret = nexion_station_request_export(dev, &priv->gpo9,
		GPIOF_OUT_INIT_LOW, "gpo9-gpio", "gpo9");
	if (ret)
		return ret;


	ret = nexion_station_request_export(dev, &priv->gpi1,
		GPIOF_IN, "gpi1-gpio", "gpi1");
	if (ret)
		return ret;
	ret = nexion_station_request_export(dev, &priv->gpi2,
		GPIOF_IN, "gpi2-gpio", "gpi2");
	if (ret)
		return ret;
	ret = nexion_station_request_export(dev, &priv->gpi3,
		GPIOF_IN, "gpi3-gpio", "gpi3");
	if (ret)
		return ret;
	
	return 0;
}

static int nexion_station_probe(struct platform_device *pdev)
{
	struct nexion_station *priv;
	int ret;

	if (!pdev->dev.of_node) {
		dev_err(&pdev->dev, "This driver support only DT init\n");
		return -EINVAL;
	}
	priv = devm_kzalloc(&pdev->dev,
			sizeof(struct nexion_station), GFP_KERNEL);
	if (!priv) {
		dev_err(&pdev->dev, "Failed to allocate memory\n");
		return -ENOMEM;
	}

	ret = nexion_station_parse_dt(&pdev->dev, priv);
	if (ret) {
		dev_err(&pdev->dev, "Parsing DT failed(%d)", ret);
		return ret;
	}

	platform_set_drvdata(pdev, priv);
	dev_info(&pdev->dev, "Probed\n");

	return 0;
}

static int nexion_station_remove(struct platform_device *pdev)
{
	platform_set_drvdata(pdev, NULL);

	return 0;
}

static const struct of_device_id nexion_station_match[] = {
	{ .compatible = "nexion,nexion_station-pm", },
	{ },
};

static struct platform_driver nexion_station_driver = {
	.probe		= nexion_station_probe,
	.remove		= nexion_station_remove,
	.driver		= {
		.name	= "nexion_station-pm",
		.of_match_table = of_match_ptr(nexion_station_match),
	},
};

static int __init nexion_station_init(void)
{
	return platform_driver_register(&nexion_station_driver);
}

static void __exit nexion_station_exit(void)
{
	platform_driver_unregister(&nexion_station_driver);
}

module_init(nexion_station_init);
module_exit(nexion_station_exit);

MODULE_ALIAS("platform:nexion_station-pm");
MODULE_DESCRIPTION("nexion Station Board Driver");
MODULE_LICENSE("GPL");
