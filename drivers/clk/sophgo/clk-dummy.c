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

unsigned long mango_clk_divider_recalc_rate(struct clk_hw *hw,
						unsigned long parent_rate)
{
	struct device_node *node;
	struct of_phandle_args clkspec;
	int rc, index = 0;
	u32 rate;
	struct property *prop;
	const __be32 *cur;
	struct clk *clk;

	/*
	 * NOTE: default value of socket0 and socket1 are same,
	 * so we just use socket0_default_rates.
	 */
	node = of_find_node_by_name(NULL, "socket0_default_rates");

	of_property_for_each_u32 (node, "clock-rates", prop, cur, rate) {
		if (rate) {
			rc = of_parse_phandle_with_args(node, "clocks",
							"#clock-cells", index, &clkspec);
			if (rc < 0) {
				/* skip empty (null) phandles */
				if (rc == -ENOENT)
					continue;
				else
					return rc;
			}

			clk = of_clk_get_from_provider(&clkspec);
			if (IS_ERR(clk))
				return PTR_ERR(clk);
			if (!strcmp(clk_hw_get_name(hw), __clk_get_name(clk)))
				return rate;
		}
		index++;
	}
	return 0;
}

long mango_clk_divider_round_rate(struct clk_hw *hw, unsigned long rate,
				      unsigned long *prate)
{
	return rate;
}

int mango_clk_divider_set_rate(struct clk_hw *hw, unsigned long rate,
				   unsigned long parent_rate)
{
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
	struct device_node *node;
	struct of_phandle_args clkspec;
	int rc, index = 0;
	u32 rate;
	struct property *prop;
	const __be32 *cur;

	/*
	 * NOTE: default value of socket0 and socket1 are same,
	 * so we just use socket0_default_rates.
	 */
	node = of_find_node_by_name(NULL, "socket0_default_rates");

	of_property_for_each_u32 (node, "clock-rates", prop, cur, rate) {
		if (rate) {
			rc = of_parse_phandle_with_args(node, "clocks",
							"#clock-cells", index, &clkspec);
			if (rc < 0) {
				/* skip empty (null) phandles */
				if (rc == -ENOENT)
					continue;
				else
					return rc;
			}

			if (!strncmp(clk_hw_get_name(hw), clkspec.np->name, 4))
				return rate;
		}
		index++;
	}
	return 0;
}

long mango_clk_pll_round_rate(struct clk_hw *hw,
				  unsigned long req_rate, unsigned long *prate)
{
	return req_rate;
}

int mango_clk_pll_determine_rate(struct clk_hw *hw,
				     struct clk_rate_request *req)
{
	req->rate = mango_clk_pll_round_rate(hw, min(req->rate, req->max_rate),
					  &req->best_parent_rate);
	return 0;
}

int mango_clk_pll_set_rate(struct clk_hw *hw, unsigned long rate,
			       unsigned long parent_rate)
{
	return 0;
}
