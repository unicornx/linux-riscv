/*
 * Copyright (c) 2022 SOPHGO
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/mfd/syscon.h>
#include <linux/io.h>
#include <linux/of_address.h>
#include <linux/string.h>
#include <linux/log2.h>

#include "clk.h"

#define KHZ 1000L
#define MHZ (KHZ * KHZ)

#define REFDIV_MIN 1
#define REFDIV_MAX 64
#define FBDIV_MIN 16
#define FBDIV_MAX 321

#define PLL_FREQ_MIN (16 * MHZ)
#define PLL_FREQ_MAX (3200 * MHZ)

#define div_mask(width) ((1 << (width)) - 1)
#define ENCODE_PLL_CTRL(fbdiv, p1, p2, refdiv) \
	(((fbdiv & 0xfff) << 16) | ((p2 & 0x7) << 12) | ((p1 & 0x7) << 8) | (refdiv & 0x3f))

static inline int __mango_pll_enable(struct mango_pll_clock *pll, bool en)
{
	unsigned int value;
	unsigned long enter;
	struct regmap *map = pll->map;

	if (en) {
		/* wait pll lock */
		enter = jiffies;
		regmap_read(map, pll->offset_status, &value);
		while (!((value >> pll->shift_status_lock) & 0x1)) {
			regmap_read(map, pll->shift_status_lock, &value);
			if (time_after(jiffies, enter + HZ / 10))
				pr_warn("%s not locked\n", pll->name);
		}
		/* wait pll updating */
		enter = jiffies;
		regmap_read(map, pll->shift_status_updating, &value);
		while (((value >> pll->shift_status_updating) & 0x1)) {
			regmap_read(map, pll->shift_status_updating, &value);
			if (time_after(jiffies, enter + HZ / 10))
				pr_warn("%s still updating\n", pll->name);
		}
		/* enable pll */
		regmap_read(map, pll->offset_enable, &value);
		regmap_write(map, pll->offset_enable, value | (1 << pll->shift_enable));
	} else {
		/* disable pll */
		regmap_read(map, pll->offset_enable, &value);
		regmap_write(map, pll->offset_enable, value & (~(1 << pll->shift_enable)));
	}

	return 0;
}

static unsigned int __mango_get_table_div(const struct clk_div_table *table,
				   unsigned int val)
{
	const struct clk_div_table *clkt;

	for (clkt = table; clkt->div; clkt++)
		if (clkt->val == val)
			return clkt->div;
	return 0;
}

static unsigned int __mango_get_div(const struct clk_div_table *table,
			     unsigned int val, unsigned long flags, u8 width)
{
	if (flags & CLK_DIVIDER_ONE_BASED)
		return val;
	if (flags & CLK_DIVIDER_POWER_OF_TWO)
		return 1 << val;
	if (flags & CLK_DIVIDER_MAX_AT_ZERO)
		return val ? val : div_mask(width) + 1;
	if (table)
		return __mango_get_table_div(table, val);
	return val + 1;
}

/* Below array is the total combination lists of POSTDIV1 and POSTDIV2
 * for example:
 * postdiv1_2[0] = {1, 1, 1}
 *           ==> div1 = 1, div2 = 1 , div1 * div2 = 1
 * postdiv1_2[22] = {6, 7, 42}
 *           ==> div1 = 6, div2 = 7 , div1 * div2 = 42
 *
 * And POSTDIV_RESULT_INDEX point to 3rd element in the array
 */
#define	POSTDIV_RESULT_INDEX	2
int postdiv1_2[][3] = {
	{2, 4,  8}, {3, 3,  9}, {2, 5, 10}, {2, 6, 12},
	{2, 7, 14}, {3, 5, 15}, {4, 4, 16}, {3, 6, 18},
	{4, 5, 20}, {3, 7, 21}, {4, 6, 24}, {5, 5, 25},
	{4, 7, 28}, {5, 6, 30}, {5, 7, 35}, {6, 6, 36},
	{6, 7, 42}, {7, 7, 49}
};

/*
 * @reg_value: current register value
 * @parent_rate: parent frequency
 *
 * This function is used to calculate below "rate" in equation
 * rate = (parent_rate/REFDIV) x FBDIV/POSTDIV1/POSTDIV2
 *      = (parent_rate x FBDIV) / (REFDIV x POSTDIV1 x POSTDIV2)
 */
static unsigned long __mango_pll_recalc_rate(unsigned int reg_value,
				       unsigned long parent_rate)
{
	unsigned int fbdiv, refdiv;
	unsigned int postdiv1, postdiv2;
	u64 rate, numerator, denominator;

	fbdiv = (reg_value >> 16) & 0xfff;
	refdiv = reg_value & 0x3f;
	postdiv1 = (reg_value >> 8) & 0x7;
	postdiv2 = (reg_value >> 12) & 0x7;

	numerator = parent_rate * fbdiv;
	denominator = refdiv * postdiv1 * postdiv2;
	do_div(numerator, denominator);
	rate = numerator;

	return rate;
}

/*
 * @reg_value: current register value
 * @rate: request rate
 * @prate: parent rate
 * @pctrl_table: use to save div1/div2/fbdiv/refdiv
 *
 * We use below equation to get POSTDIV1 and POSTDIV2
 * POSTDIV = (parent_rate/REFDIV) x FBDIV/input_rate
 * above POSTDIV = POSTDIV1*POSTDIV2
 */
static int __mango_pll_get_postdiv_1_2(unsigned long rate, unsigned long prate,
				 unsigned int fbdiv, unsigned int refdiv, unsigned int *postdiv1,
				 unsigned int *postdiv2)
{
	int index = 0;
	int ret = 0;
	u64 tmp0;

	/* calculate (parent_rate/refdiv)
	 * and result save to prate
	 */
	tmp0 = prate;
	do_div(tmp0, refdiv);

	/* calcuate ((parent_rate/REFDIV) x FBDIV)
	 * and result save to prate
	 */
	tmp0 *= fbdiv;

	/* calcuate (((parent_rate/REFDIV) x FBDIV)/input_rate)
	 * and result save to prate
	 * here *prate is (POSTDIV1*POSTDIV2)
	 */
	do_div(tmp0, rate);

	/* calculate div1 and div2 value */
	if (tmp0 <= 7) {
		/* (div1 * div2) <= 7, no need to use array search */
		*postdiv1 = tmp0;
		*postdiv2 = 1;
	} else {
		/* (div1 * div2) > 7, use array search */
		for (index = 0; index < ARRAY_SIZE(postdiv1_2); index++) {
			if (tmp0 > postdiv1_2[index][POSTDIV_RESULT_INDEX]) {
				continue;
			} else {
				/* found it */
				break;
			}
		}
		if (index < ARRAY_SIZE(postdiv1_2)) {
			*postdiv1 = postdiv1_2[index][1];
			*postdiv2 = postdiv1_2[index][0];
		} else {
			pr_debug("%s out of postdiv array range!\n", __func__);
			ret = -ESPIPE;
		}
	}

	return ret;
}

static int __mango_get_pll_ctl_setting(struct mango_pll_ctrl *best,
			unsigned long req_rate, unsigned long parent_rate)
{
	int ret;
	unsigned int fbdiv, refdiv, fref, postdiv1, postdiv2;
	unsigned long tmp = 0, foutvco;

	fref = parent_rate;

	for (refdiv = REFDIV_MIN; refdiv < REFDIV_MAX + 1; refdiv++) {
		for (fbdiv = FBDIV_MIN; fbdiv < FBDIV_MAX + 1; fbdiv++) {
			foutvco = fref * fbdiv / refdiv;
			/* check fpostdiv pfd */
			if (foutvco < PLL_FREQ_MIN || foutvco > PLL_FREQ_MAX
					|| (fref / refdiv) < 10)
				continue;

			ret = __mango_pll_get_postdiv_1_2(req_rate, fref, fbdiv,
					refdiv, &postdiv1, &postdiv2);
			if (ret)
				continue;

			tmp = foutvco / (postdiv1 * postdiv2);
			if (abs_diff(tmp, req_rate) < abs_diff(best->freq, req_rate)) {
				best->freq = tmp;
				best->refdiv = refdiv;
				best->fbdiv = fbdiv;
				best->postdiv1 = postdiv1;
				best->postdiv2 = postdiv2;
				if (tmp == req_rate)
					return 0;
			}
			continue;
		}
	}

	return 0;
}

/*
 * @hw: ccf use to hook get mango_pll_clock
 * @parent_rate: parent rate
 *
 * The is function will be called through clk_get_rate
 * and return current rate after decoding reg value
 */
unsigned long mango_clk_pll_recalc_rate(struct clk_hw *hw,
					    unsigned long parent_rate)
{
	unsigned int value;
	unsigned long rate;
	struct mango_pll_clock *pll = to_mango_pll_clk(hw);

	regmap_read(pll->map, pll->offset_ctrl, &value);
	rate = __mango_pll_recalc_rate(value, parent_rate);

	pr_debug("--> pll_recalc_rate: val = %ld\n", rate);
	return rate;
}

long mango_clk_pll_round_rate(struct clk_hw *hw,
				  unsigned long req_rate, unsigned long *prate)
{
	unsigned int value;
	struct mango_pll_ctrl pctrl_table;
	struct mango_pll_clock *pll = to_mango_pll_clk(hw);
	long proper_rate;

	memset(&pctrl_table, 0, sizeof(struct mango_pll_ctrl));

	/* use current setting to get fbdiv, refdiv
	 * then combine with prate, and req_rate to
	 * get postdiv1 and postdiv2
	 */
	regmap_read(pll->map, pll->offset_ctrl, &value);
	__mango_get_pll_ctl_setting(&pctrl_table, req_rate, *prate);
	if (!pctrl_table.freq) {
		proper_rate = 0;
		goto out;
	}

	value = ENCODE_PLL_CTRL(pctrl_table.fbdiv, pctrl_table.postdiv1,
			     pctrl_table.postdiv2, pctrl_table.refdiv);
	proper_rate = (long)__mango_pll_recalc_rate(value, *prate);

out:
	pr_debug("--> pll_round_rate: val = %ld\n", proper_rate);
	return proper_rate;
}

int mango_clk_pll_determine_rate(struct clk_hw *hw,
				     struct clk_rate_request *req)
{
	req->rate = mango_clk_pll_round_rate(hw, min(req->rate, req->max_rate),
					  &req->best_parent_rate);
	pr_debug("--> pll_determine_rate: val = %ld\n", req->rate);
	return 0;
}

int mango_clk_pll_set_rate(struct clk_hw *hw, unsigned long rate,
			       unsigned long parent_rate)
{
	unsigned long flags;
	unsigned int value;
	int ret = 0;
	struct mango_pll_ctrl pctrl_table;
	struct mango_pll_clock *pll = to_mango_pll_clk(hw);

	memset(&pctrl_table, 0, sizeof(struct mango_pll_ctrl));
	spin_lock_irqsave(pll->lock, flags);
	if (__mango_pll_enable(pll, 0)) {
		pr_warn("Can't disable pll(%s), status error\n", pll->name);
		goto out;
	}
	regmap_read(pll->map, pll->offset_ctrl, &value);
	__mango_get_pll_ctl_setting(&pctrl_table, rate, parent_rate);
	if (!pctrl_table.freq) {
		pr_warn("%s: Can't find a proper pll setting\n", pll->name);
		goto out;
	}

	value = ENCODE_PLL_CTRL(pctrl_table.fbdiv, pctrl_table.postdiv1,
			     pctrl_table.postdiv2, pctrl_table.refdiv);

	/* write the value to top register */
	regmap_write(pll->map, pll->offset_ctrl, value);
	__mango_pll_enable(pll, 1);
out:
	spin_unlock_irqrestore(pll->lock, flags);

	pr_debug("--> pll_set_rate: val = %d\n", value);
	return ret;
}

unsigned long mango_clk_divider_recalc_rate(struct clk_hw *hw,
						unsigned long parent_rate)
{
	struct mango_divider_clock *divider = to_mango_clk_divider(hw);
	unsigned int val;
	unsigned long ret_rate;

	val = readl(divider->reg) >> divider->shift;
	val &= div_mask(divider->width);

	ret_rate = divider_recalc_rate(hw, parent_rate, val, divider->table,
				   divider->div_flags, divider->width);
	pr_debug("--> divider_recalc_rate: val = %ld\n", ret_rate);
	return ret_rate;
}

long mango_clk_divider_round_rate(struct clk_hw *hw, unsigned long rate,
				      unsigned long *prate)
{
	int bestdiv;
	unsigned long ret_rate;
	struct mango_divider_clock *divider = to_mango_clk_divider(hw);

	/* if read only, just return current value */
	if (divider->div_flags & CLK_DIVIDER_READ_ONLY) {
		bestdiv = readl(divider->reg) >> divider->shift;
		bestdiv &= div_mask(divider->width);
		bestdiv = __mango_get_div(divider->table, bestdiv, divider->div_flags,
				   divider->width);
		ret_rate = DIV_ROUND_UP_ULL((u64)*prate, bestdiv);
	} else {
		ret_rate = divider_round_rate(hw, rate, prate, divider->table,
				  divider->width, divider->div_flags);
	}

	pr_debug("--> divider_round_rate: val = %ld\n", ret_rate);
	return ret_rate;
}

int mango_clk_divider_set_rate(struct clk_hw *hw, unsigned long rate,
				   unsigned long parent_rate)
{
	unsigned int value;
	unsigned int val, val2;
	unsigned long flags = 0;
	struct mango_divider_clock *divider = to_mango_clk_divider(hw);

	value = divider_get_val(rate, parent_rate, divider->table,
				divider->width, divider->div_flags);

	if (divider->lock)
		spin_lock_irqsave(divider->lock, flags);
	else
		__acquire(divider->lock);

	/* div assert */
	val = readl(divider->reg);
	val &= ~0x1;
	writel(val, divider->reg);

	if (divider->div_flags & CLK_DIVIDER_HIWORD_MASK) {
		val = div_mask(divider->width) << (divider->shift + 16);
	} else {
		val = readl(divider->reg);
		val &= ~(div_mask(divider->width) << divider->shift);
	}

	val |= value << divider->shift;
	writel(val, divider->reg);
	val2 = val;

	if (!(divider->div_flags & CLK_DIVIDER_READ_ONLY))
		val |= 1 << 3;

	/* de-assert */
	val |= 1;
	writel(val, divider->reg);
	if (divider->lock)
		spin_unlock_irqrestore(divider->lock, flags);
	else
		__release(divider->lock);

	pr_debug("--> divider_set_rate: val = %d\n", val2);
	return 0;
}