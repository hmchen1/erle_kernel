/*
 * TI ADC MFD driver
 *
 * Copyright (C) 2012 Texas Instruments Incorporated - http://www.ti.com/
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation version 2.
 *
 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 * kind, whether express or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/iio/iio.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/iio/machine.h>
#include <linux/iio/driver.h>
#include <linux/regmap.h>

#include <linux/io.h>
#include <linux/mfd/ti_am335x_tscadc.h>
#include <linux/platform_data/ti_am335x_adc.h>

struct tiadc_device {
	struct ti_tscadc_dev *mfd_tscadc;
	int channels;
	char *buf;
	struct iio_map *map;
};

static unsigned int tiadc_readl(struct tiadc_device *adc, unsigned int reg)
{
	unsigned int val;

	val = (unsigned int)-1;
	regmap_read(adc->mfd_tscadc->regmap_tscadc, reg, &val);
	return val;
}

static void tiadc_writel(struct tiadc_device *adc, unsigned int reg,
					unsigned int val)
{
	regmap_write(adc->mfd_tscadc->regmap_tscadc, reg, val);
}

static void tiadc_step_config(struct tiadc_device *adc_dev)
{
	unsigned int stepconfig;
	int i, channels = 0, steps;

	/*
	 * There are 16 configurable steps and 8 analog input
	 * lines available which are shared between Touchscreen and ADC.
	 *
	 * Steps backwards i.e. from 16 towards 0 are used by ADC
	 * depending on number of input lines needed.
	 * Channel would represent which analog input
	 * needs to be given to ADC to digitalize data.
	 */

	steps = TOTAL_STEPS - adc_dev->channels;
	channels = TOTAL_CHANNELS - adc_dev->channels;

	stepconfig = STEPCONFIG_AVG_16 | STEPCONFIG_FIFO1;

	for (i = (steps + 1); i <= TOTAL_STEPS; i++) {
		tiadc_writel(adc_dev, REG_STEPCONFIG(i),
				stepconfig | STEPCONFIG_INP(channels));
		tiadc_writel(adc_dev, REG_STEPDELAY(i),
				STEPCONFIG_OPENDLY);
		channels++;
	}
	tiadc_writel(adc_dev, REG_SE, STPENB_STEPENB);
}

static int tiadc_channel_init(struct iio_dev *indio_dev,
		struct tiadc_device *adc_dev)
{
	struct iio_chan_spec *chan_array;
	struct iio_chan_spec *chan;
	char *s;
	int i, len, size, ret;
	int channels = adc_dev->channels;

	size = channels * (sizeof(struct iio_chan_spec) + 6);
	chan_array = kzalloc(size, GFP_KERNEL);
	if (chan_array == NULL)
		return -ENOMEM;

	/* buffer space is after the array */
	s = (char *)(chan_array + channels);
	chan = chan_array;
	for (i = 0; i < channels; i++, chan++, s += len + 1) {

		len = sprintf(s, "AIN%d", i);

		chan->type = IIO_VOLTAGE;
		chan->indexed = 1;
		chan->channel = i;
		chan->info_mask = IIO_CHAN_INFO_RAW_SEPARATE_BIT;
		chan->datasheet_name = s;
		chan->scan_type.sign = 'u';
		chan->scan_type.realbits = 12;
		chan->scan_type.storagebits = 32;
		chan->scan_type.shift = 0;
	}

	indio_dev->channels = chan_array;
	indio_dev->num_channels = channels;

	size = (channels + 1) * sizeof(struct iio_map);
	adc_dev->map = kzalloc(size, GFP_KERNEL);
	if (adc_dev->map == NULL) {
		kfree(chan_array);
		return -ENOMEM;
	}

	for (i = 0; i < indio_dev->num_channels; i++) {
		adc_dev->map[i].adc_channel_label = chan_array[i].datasheet_name;
		adc_dev->map[i].consumer_dev_name = "any";
		adc_dev->map[i].consumer_channel = chan_array[i].datasheet_name;
	}
	adc_dev->map[i].adc_channel_label = NULL;
	adc_dev->map[i].consumer_dev_name = NULL;
	adc_dev->map[i].consumer_channel = NULL;

	ret = iio_map_array_register(indio_dev, adc_dev->map);
	if (ret != 0) {
		kfree(adc_dev->map);
		kfree(chan_array);
		return -ENOMEM;
	}

	return indio_dev->num_channels;
}

static void tiadc_channels_remove(struct iio_dev *indio_dev)
{
	kfree(indio_dev->channels);
}

static int tiadc_read_raw(struct iio_dev *indio_dev,
		struct iio_chan_spec const *chan,
		int *val, int *val2, long mask)
{
	struct tiadc_device *adc_dev = iio_priv(indio_dev);
	int i;
	unsigned int fifo1count, readx1;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		/*
		 * When the sub-system is first enabled,
		 * the sequencer will always start with the
		 * lowest step (1) and continue until step (16).
		 * For ex: If we have enabled 4 ADC channels and
		 * currently use only 1 out of them, the
		 * sequencer still configures all the 4 steps,
		 * leading to 3 unwanted data.
		 * Hence we need to flush out this data.
		 */

		fifo1count = tiadc_readl(adc_dev, REG_FIFO1CNT);
		for (i = 0; i < fifo1count; i++) {
			readx1 = tiadc_readl(adc_dev, REG_FIFO1);
			if (i == chan->channel)
				*val = readx1 & 0xfff;
		}
		tiadc_writel(adc_dev, REG_SE, STPENB_STEPENB);

		if (fifo1count <= chan->channel)
			return -EINVAL;

		return IIO_VAL_INT;
	default:
		return -EINVAL;
	}
}

static const struct iio_info tiadc_info = {
	.read_raw = &tiadc_read_raw,
};

static int tiadc_probe(struct platform_device *pdev)
{
	struct iio_dev		*indio_dev;
	struct tiadc_device	*adc_dev;
	struct ti_tscadc_dev	*tscadc_dev = pdev->dev.platform_data;
	struct mfd_tscadc_board	*pdata = tscadc_dev->dev->platform_data;
	struct device_node	*node = tscadc_dev->dev->of_node;
	int			err;
	u32			val32;

	if (!pdata && !node) {
		dev_err(&pdev->dev, "Could not find platform data\n");
		return -EINVAL;
	}

	indio_dev = iio_device_alloc(sizeof(struct tiadc_device));
	if (indio_dev == NULL) {
		dev_err(&pdev->dev, "failed to allocate iio device\n");
		err = -ENOMEM;
		goto err_ret;
	}
	adc_dev = iio_priv(indio_dev);

	adc_dev->mfd_tscadc = tscadc_dev;

	if (pdata)
		adc_dev->channels = pdata->adc_init->adc_channels;
	else {
		node = of_get_child_by_name(node, "adc");
		if (!node)
			return	-EINVAL;
		else {
			err = of_property_read_u32(node,
					"ti,adc-channels", &val32);
			if (err < 0)
				goto err_free_device;
			else
				adc_dev->channels = val32;
		}
	}

	indio_dev->dev.parent = &pdev->dev;
	indio_dev->name = dev_name(&pdev->dev);
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->info = &tiadc_info;

	tiadc_step_config(adc_dev);

	err = tiadc_channel_init(indio_dev, adc_dev);
	if (err < 0)
		goto err_free_device;

	err = iio_device_register(indio_dev);
	if (err)
		goto err_free_channels;

	platform_set_drvdata(pdev, indio_dev);

	dev_info(&pdev->dev, "Initialized\n");

	return 0;

err_free_channels:
	tiadc_channels_remove(indio_dev);
err_free_device:
	iio_device_free(indio_dev);
err_ret:
	return err;
}

static int tiadc_remove(struct platform_device *pdev)
{
	struct iio_dev *indio_dev = platform_get_drvdata(pdev);

	iio_device_unregister(indio_dev);
	tiadc_channels_remove(indio_dev);

	iio_device_free(indio_dev);

	return 0;
}

#ifdef CONFIG_PM
static int tiadc_suspend(struct device *dev)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct tiadc_device *adc_dev = iio_priv(indio_dev);
	struct ti_tscadc_dev *tscadc_dev = dev->platform_data;
	unsigned int idle;

	if (!device_may_wakeup(tscadc_dev->dev)) {
		idle = tiadc_readl(adc_dev, REG_CTRL);
		idle &= ~(CNTRLREG_TSCSSENB);
		tiadc_writel(adc_dev, REG_CTRL, (idle |
				CNTRLREG_POWERDOWN));
	}

	return 0;
}

static int tiadc_resume(struct device *dev)
{
	struct iio_dev *indio_dev = dev_get_drvdata(dev);
	struct tiadc_device *adc_dev = iio_priv(indio_dev);
	unsigned int restore;

	/* Make sure ADC is powered up */
	restore = tiadc_readl(adc_dev, REG_CTRL);
	restore &= ~(CNTRLREG_POWERDOWN);
	tiadc_writel(adc_dev, REG_CTRL, restore);

	tiadc_step_config(adc_dev);

	return 0;
}

static const struct dev_pm_ops tiadc_pm_ops = {
	.suspend = tiadc_suspend,
	.resume = tiadc_resume,
};
#define TIADC_PM_OPS (&tiadc_pm_ops)
#else
#define TIADC_PM_OPS NULL
#endif

static struct platform_driver tiadc_driver = {
	.driver = {
		.name	= "tiadc",
		.owner	= THIS_MODULE,
		.pm	= TIADC_PM_OPS,
	},
	.probe	= tiadc_probe,
	.remove	= tiadc_remove,
};

module_platform_driver(tiadc_driver);

MODULE_DESCRIPTION("TI ADC controller driver");
MODULE_AUTHOR("Rachna Patil <rachna@ti.com>");
MODULE_LICENSE("GPL");