// SPDX-License-Identifier: GPL-2.0
/*
 * Sophgo SG2042 PWM Controller Driver
 *
 * Copyright (C) 2024 Sophgo Technology Inc.
 * Copyright (C) 2024 Chen Wang <unicorn_wang@outlook.com>
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/export.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>
#include <linux/slab.h>

#define REG_HLPERIOD	0x0
#define REG_PERIOD	0x4
#define REG_GROUP	0x8

#define SG2042_PWM_CHANNELNUM	4

/**
 * struct sg2042_pwm_channel - private data of PWM channel
 * @period_ns:	current period in nanoseconds programmed to the hardware
 * @duty_ns:	current duty time in nanoseconds programmed to the hardware
 */
struct sg2042_pwm_channel {
	u32 period;
	u32 hlperiod;
};

/**
 * struct sg2042_pwm_chip - private data of PWM chip
 * @chip:		generic PWM chip
 * @base:		base address of mapped PWM registers
 * @base_clk:		base clock used to drive the timers
 * @channel:		per channel driver data
 */
struct sg2042_pwm_chip {
	struct pwm_chip chip;
	void __iomem *base;
	struct clk *base_clk;
	struct sg2042_pwm_channel channel[SG2042_PWM_CHANNELNUM];
};


static inline
struct sg2042_pwm_chip *to_sg2042_pwm_chip(struct pwm_chip *chip)
{
	return pwmchip_get_drvdata(chip);
}

static int pwm_sg2042_request(struct pwm_chip *chip, struct pwm_device *pwm_dev)
{
	struct sg2042_pwm_chip *sg2042_pwm = to_sg2042_pwm_chip(chip);

	memset(&sg2042_pwm->channel[pwm_dev->hwpwm], 0, sizeof(sg2042_pwm->channel[pwm_dev->hwpwm]));

	return 0;
}

#if 0
static void pwm_sg2042_free(struct pwm_chip *chip, struct pwm_device *pwm_dev)
{
	struct sg2042_pwm_channel *channel = pwm_get_chip_data(pwm_dev);

	pwm_set_chip_data(pwm_dev, NULL);
	kfree(channel);
}
#endif

static int pwm_sg2042_config(struct pwm_chip *chip, struct pwm_device *pwm_dev,
			     int duty_ns, int period_ns)
{
	struct sg2042_pwm_chip *sg2042_pwm = to_sg2042_pwm_chip(chip);
	struct sg2042_pwm_channel *channel = &sg2042_pwm->channel[pwm_dev->hwpwm];
	u64 cycles;

	cycles = clk_get_rate(sg2042_pwm->base_clk);
	cycles *= period_ns;
	do_div(cycles, NSEC_PER_SEC);

	channel->period = cycles;
	cycles = cycles * duty_ns;
	do_div(cycles, period_ns);
	channel->hlperiod = channel->period - cycles;

	return 0;
}

static int pwm_sg2042_enable(struct pwm_chip *chip, struct pwm_device *pwm_dev)
{
	struct sg2042_pwm_chip *sg2042_pwm = to_sg2042_pwm_chip(chip);
	struct sg2042_pwm_channel *channel = &sg2042_pwm->channel[pwm_dev->hwpwm];

	writel(channel->period, sg2042_pwm->base + REG_GROUP * pwm_dev->hwpwm + REG_PERIOD);
	writel(channel->hlperiod, sg2042_pwm->base + REG_GROUP * pwm_dev->hwpwm + REG_HLPERIOD);

	return 0;
}

static void pwm_sg2042_disable(struct pwm_chip *chip,
			       struct pwm_device *pwm_dev)
{
	struct sg2042_pwm_chip *sg2042_pwm = to_sg2042_pwm_chip(chip);

	writel(0, sg2042_pwm->base + REG_GROUP * pwm_dev->hwpwm + REG_PERIOD);
	writel(0, sg2042_pwm->base + REG_GROUP * pwm_dev->hwpwm + REG_HLPERIOD);
}

static int pwm_sg2042_apply(struct pwm_chip *chip, struct pwm_device *pwm,
			    const struct pwm_state *state)
{
	int ret;

	bool enabled = pwm->state.enabled;

	if (state->polarity != pwm->state.polarity && pwm->state.enabled) {
		pwm_sg2042_disable(chip, pwm);
		enabled = false;
	}

	if (!state->enabled) {
		if (enabled)
			pwm_sg2042_disable(chip, pwm);
		return 0;
	}

	ret = pwm_sg2042_config(chip, pwm, state->duty_cycle, state->period);
	if (ret) {
		dev_err(pwmchip_parent(chip), "pwm apply err\n");
		return ret;
	}
	dev_dbg(pwmchip_parent(chip), "%s tate->enabled =%d\n", __func__, state->enabled);
	if (state->enabled)
		ret = pwm_sg2042_enable(chip, pwm);
	else
		pwm_sg2042_disable(chip, pwm);

	if (ret) {
		dev_err(pwmchip_parent(chip), "pwm apply failed\n");
		return ret;
	}
	return ret;
}

static const struct pwm_ops pwm_sg2042_ops = {
	.request	= pwm_sg2042_request,
	.apply		= pwm_sg2042_apply,
};

static const struct of_device_id sg2042_pwm_match[] = {
	{ .compatible = "sophgo,sg2042-pwm" },
	{ },
};
MODULE_DEVICE_TABLE(of, sg2042_pwm_match);

static int pwm_sg2042_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct sg2042_pwm_chip *sg2042_pwm;
	struct pwm_chip *chip;
	int ret;

	chip = devm_pwmchip_alloc(&pdev->dev, SG2042_PWM_CHANNELNUM, sizeof(*sg2042_pwm));
	if (IS_ERR(chip))
		return PTR_ERR(chip);
	sg2042_pwm = to_sg2042_pwm_chip(chip);

	chip->ops = &pwm_sg2042_ops;

	sg2042_pwm->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(sg2042_pwm->base))
		return PTR_ERR(sg2042_pwm->base);

	sg2042_pwm->base_clk = devm_clk_get_enabled(&pdev->dev, "apb");
	if (IS_ERR(sg2042_pwm->base_clk))
		return dev_err_probe(dev, PTR_ERR(sg2042_pwm->base_clk),
				     "failed to get base clk\n");

	platform_set_drvdata(pdev, chip);

	ret = devm_pwmchip_add(&pdev->dev, chip);
	if (ret < 0)
		return dev_err_probe(dev, ret, "failed to register PWM chip\n");

	return 0;
}

static void pwm_sg2042_remove(struct platform_device *pdev)
{
	struct pwm_chip *chip = platform_get_drvdata(pdev);
	struct sg2042_pwm_chip *sg2042_pwm = to_sg2042_pwm_chip(chip);

	pwmchip_remove(chip);

	clk_disable_unprepare(sg2042_pwm->base_clk);
}

static struct platform_driver pwm_sg2042_driver = {
	.driver	= {
		.name	= "sg2042-pwm",
		.of_match_table = of_match_ptr(sg2042_pwm_match),
	},
	.probe = pwm_sg2042_probe,
	.remove_new = pwm_sg2042_remove,
};
module_platform_driver(pwm_sg2042_driver);

MODULE_AUTHOR("Chen Wang");
MODULE_DESCRIPTION("Sophgo SG2042 PWM driver");
MODULE_LICENSE("GPL");
