// SPDX-License-Identifier: GPL-2.0
/*
 * Sophgo SG2042 PWM Controller Driver
 *
 * Copyright (C) 2024 Sophgo Technology Inc.
 * Copyright (C) 2024 Chen Wang <unicorn_wang@outlook.com>
 *
 * Limitations:
 * - After reset, the output of the PWM channel is always high.
 *   The value of HLPERIOD/PERIOD is 0.
 * - When HLPERIOD or PERIOD is reconfigured, PWM will start to
 *   output waveforms with the new configuration after completing
 *   the running period.
 * - When PERIOD and HLPERIOD is set to 0, the PWM wave output will
 *   be stopped and the output is pulled to high.
 */

#include <linux/clk.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>

#include <asm/div64.h>

/*
 * Offset RegisterName
 * 0x0000 HLPERIOD0
 * 0x0004 PERIOD0
 * 0x0008 HLPERIOD1
 * 0x000C PERIOD1
 * 0x0010 HLPERIOD2
 * 0x0014 PERIOD2
 * 0x0018 HLPERIOD3
 * 0x001C PERIOD3
 * Four groups and every group is composed of HLPERIOD & PERIOD
 */
#define SG2042_HLPERIOD(chan) ((chan) * 8 + 0)
#define SG2042_PERIOD(chan) ((chan) * 8 + 4)

#define SG2042_PWM_CHANNELNUM	4

/**
 * struct sg2042_pwm_ddata - private driver data
 * @base:		base address of mapped PWM registers
 * @clk_rate_hz:	rate of base clock in HZ
 */
struct sg2042_pwm_ddata {
	void __iomem *base;
	unsigned long clk_rate_hz;
};

static void pwm_sg2042_config(void __iomem *base, unsigned int chan, u32 period, u32 hlperiod)
{
	writel(period, base + SG2042_PERIOD(chan));
	writel(hlperiod, base + SG2042_HLPERIOD(chan));
}

static int pwm_sg2042_apply(struct pwm_chip *chip, struct pwm_device *pwm,
			    const struct pwm_state *state)
{
	struct sg2042_pwm_ddata *ddata = pwmchip_get_drvdata(chip);
	u32 hlperiod;
	u32 period;

	if (state->polarity == PWM_POLARITY_INVERSED)
		return -EINVAL;

	if (!state->enabled) {
		pwm_sg2042_config(ddata->base, pwm->hwpwm, 0, 0);
		return 0;
	}

	/*
	 * Period of High level (duty_cycle) = HLPERIOD x Period_clk
	 * Period of One Cycle (period) = PERIOD x Period_clk
	 */
	period = min(mul_u64_u64_div_u64(ddata->clk_rate_hz, state->period, NSEC_PER_SEC), U32_MAX);
	hlperiod = min(mul_u64_u64_div_u64(ddata->clk_rate_hz, state->duty_cycle, NSEC_PER_SEC), U32_MAX);

	if (hlperiod > period) {
		dev_err(pwmchip_parent(chip), "period < hlperiod, failed to apply current setting\n");
		return -EINVAL;
	}

	dev_dbg(pwmchip_parent(chip), "chan[%u]: period=%u, hlperiod=%u\n",
		pwm->hwpwm, period, hlperiod);

	pwm_sg2042_config(ddata->base, pwm->hwpwm, period, hlperiod);

	return 0;
}

static int pwm_sg2042_get_state(struct pwm_chip *chip, struct pwm_device *pwm,
				struct pwm_state *state)
{
	struct sg2042_pwm_ddata *ddata = pwmchip_get_drvdata(chip);
	unsigned int chan = pwm->hwpwm;
	u32 hlperiod;
	u32 period;

	period = readl(ddata->base + SG2042_PERIOD(chan));
	hlperiod = readl(ddata->base + SG2042_HLPERIOD(chan));

	if (!period && !hlperiod)
		state->enabled = false;
	else
		state->enabled = true;

	state->period = DIV_ROUND_UP_ULL((u64)period * NSEC_PER_SEC, ddata->clk_rate_hz);
	state->duty_cycle = DIV_ROUND_UP_ULL((u64)hlperiod * NSEC_PER_SEC, ddata->clk_rate_hz);

	state->polarity = PWM_POLARITY_NORMAL;

	return 0;
}

static const struct pwm_ops pwm_sg2042_ops = {
	.apply = pwm_sg2042_apply,
	.get_state = pwm_sg2042_get_state,
};

static const struct of_device_id sg2042_pwm_ids[] = {
	{ .compatible = "sophgo,sg2042-pwm" },
	{ }
};
MODULE_DEVICE_TABLE(of, sg2042_pwm_ids);

static int pwm_sg2042_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct sg2042_pwm_ddata *ddata;
	struct pwm_chip *chip;
	struct clk *clk;
	int ret;

	chip = devm_pwmchip_alloc(dev, SG2042_PWM_CHANNELNUM, sizeof(*ddata));
	if (IS_ERR(chip))
		return PTR_ERR(chip);
	ddata = pwmchip_get_drvdata(chip);

	ddata->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(ddata->base))
		return PTR_ERR(ddata->base);

	clk = devm_clk_get_enabled(dev, "apb");
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk), "failed to get base clk\n");

	ret = devm_clk_rate_exclusive_get(dev, clk);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get exclusive rate\n");

	ddata->clk_rate_hz = clk_get_rate(clk);
	if (!ddata->clk_rate_hz || ddata->clk_rate_hz > NSEC_PER_SEC)
		return dev_err_probe(dev, -EINVAL,
				     "Invalid clock rate: %lu\n", ddata->clk_rate_hz);

	chip->ops = &pwm_sg2042_ops;

	ret = devm_pwmchip_add(dev, chip);
	if (ret < 0)
		return dev_err_probe(dev, ret, "failed to register PWM chip\n");

	return 0;
}

static struct platform_driver pwm_sg2042_driver = {
	.driver	= {
		.name = "sg2042-pwm",
		.of_match_table = sg2042_pwm_ids,
	},
	.probe = pwm_sg2042_probe,
};
module_platform_driver(pwm_sg2042_driver);

MODULE_AUTHOR("Chen Wang");
MODULE_DESCRIPTION("Sophgo SG2042 PWM driver");
MODULE_LICENSE("GPL");
