/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Sophgo SG2042 Clock Generator Driver
 *
 * Copyright (C) 2024 Sophgo Technology Inc. All rights reserved.
 */

#ifndef _CLK_SOPHGO_SG2042_H_
#define _CLK_SOPHGO_SG2042_H_

#include <linux/io.h>
#include <linux/clk-provider.h>

/**
 * struct sg2042_clk_data - Common data of clock-controller
 * @iobase: base address of clock-controller
 * @onecell_data: used for adding providers.
 */
struct sg2042_clk_data {
	void __iomem *iobase;
	struct clk_hw_onecell_data onecell_data;
};

#endif /* _CLK_SOPHGO_SG2042_H_ */
