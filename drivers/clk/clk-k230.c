// SPDX-License-Identifier: GPL-2.0-only
/*
 * Kendryte Canaan K230 Clock Drivers
 *
 * Author: Xukai Wang <kingxukai@zohomail.com>
 * Author: Troy Mitchell <troymitchell988@gmail.com>
 */

#include <linux/clk.h>
#include <linux/clkdev.h>
#include <linux/clk-provider.h>
#include <linux/iopoll.h>
#include <linux/mod_devicetable.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>

#include <dt-bindings/clock/canaan,k230-clk.h>

/* PLL control register bits. */
#define K230_PLL_BYPASS_ENABLE			BIT(19)
#define K230_PLL_GATE_ENABLE			BIT(2)
#define K230_PLL_GATE_WRITE_ENABLE		BIT(18)
#define K230_PLL_OD_SHIFT			24
#define K230_PLL_OD_MASK			0xF
#define K230_PLL_R_SHIFT			16
#define K230_PLL_R_MASK				0x3F
#define K230_PLL_F_SHIFT			0
#define K230_PLL_F_MASK				0x1FFF
#define K230_PLL_DIV_REG_OFFSET			0x00
#define K230_PLL_BYPASS_REG_OFFSET		0x04
#define K230_PLL_GATE_REG_OFFSET		0x08
#define K230_PLL_LOCK_REG_OFFSET		0x0C

/* PLL lock register  */
#define K230_PLL_LOCK_STATUS_MASK		BIT(0)
#define K230_PLL_LOCK_TIME_DELAY		400
#define K230_PLL_LOCK_TIMEOUT			0

/* K230 CLK registers offset */
#define K230_CLK_AUDIO_CLKDIV_OFFSET		0x34
#define K230_CLK_PDM_CLKDIV_OFFSET		0x40
#define K230_CLK_CODEC_ADC_MCLKDIV_OFFSET	0x38
#define K230_CLK_CODEC_DAC_MCLKDIV_OFFSET	0x3c

#define K230_CLK_MAX_PARENT_NUM			3
#define K230_CLK_NUM				211

#define K230_FMT(_var)				(&k230_##_var)

#define K230_PLLX_OFFSET(idx)			((idx) * 0x10)
#define K230_PLLX_BASE(base, idx)		((base) + K230_PLLX_OFFSET(idx))

#define K230_PLLX_DIV_ADDR(base, idx)						\
	(K230_PLL_DIV_REG_OFFSET + K230_PLLX_BASE(base, idx))

#define K230_PLLX_BYPASS_ADDR(base, idx)					\
	(K230_PLL_BYPASS_REG_OFFSET + K230_PLLX_BASE(base, idx))

#define K230_PLLX_GATE_ADDR(base, idx)						\
	(K230_PLL_GATE_REG_OFFSET + K230_PLLX_BASE(base, idx))

#define K230_PLLX_LOCK_ADDR(base, idx)						\
	(K230_PLL_LOCK_REG_OFFSET + K230_PLLX_BASE(base, idx))

#define K230_CLK_FIXED_FACTOR_FORMAT(_var,					\
				     _mul, _div, _flags,			\
				     _parent)					\
	static struct k230_clk_fixed_factor k230_##_var = {			\
		.clk = {							\
			.mult = _mul,						\
			.div = _div,						\
			.hw.init = CLK_HW_INIT_HW(#_var,			\
				   &k230_##_parent.clk.hw, &clk_fixed_factor_ops,	\
				   _flags),					\
		},								\
	}

#define K230_CLK_PLL_DIV_FORMAT(_var, _div, _flags, _parent)			\
	static struct k230_clk_fixed_factor k230_##_var = {			\
		.clk = {							\
			.mult = 1,						\
			.div = _div,						\
			.hw.init = CLK_HW_INIT_HW(#_var,			\
				   &k230_##_parent.clk.hw, &clk_fixed_factor_ops,	\
				   _flags),					\
		},								\
	}

#define K230_CLK_PLL_FORMAT(_var, _id, _flags, _parent)				\
	static struct k230_pll k230_##_var = {					\
		.clk = {							\
			.hw.init = CLK_HW_INIT_FW_NAME(#_var,			\
				   _parent,					\
				   &k230_pll_ops, _flags),			\
			.id = _id,						\
		},								\
	}

#define K230_CLK_RATE_FORMAT(_var,						\
			     _mul_min, _mul_max, _mul_shift, _mul_mask,		\
			     _div_min, _div_max, _div_shift, _div_mask,		\
			     _reg, _bit, _method, _reg2,			\
			     _read_only, _flags,				\
			     _parent)						\
	static struct k230_clk_rate k230_##_var = {				\
		.reg_off = _reg,						\
		.reg_off2 = _reg2,						\
		.clk = {							\
			.write_enable_bit = _bit,				\
			.mul_min = _mul_min,					\
			.mul_max = _mul_max,					\
			.mul_shift = _mul_shift,				\
			.mul_mask = _mul_mask,					\
			.div_min = _div_min,					\
			.div_max = _div_max,					\
			.div_shift = _div_shift,				\
			.div_mask = _div_mask,					\
			.read_only = _read_only,				\
			.hw.init = CLK_HW_INIT_HW(#_var,			\
				   &k230_##_parent.clk.hw,			\
				   &k230_clk_ops_##_method,			\
				   _flags),					\
		},								\
	}

#define K230_CLK_RATE_FORMAT_PDATA(_var,					\
				   _mul_min, _mul_max, _mul_shift, _mul_mask,	\
				   _div_min, _div_max, _div_shift, _div_mask,	\
				   _reg, _bit, _method, _reg2,			\
				   _read_only, _flags,				\
				   _parent)					\
	static struct k230_clk_rate k230_##_var = {				\
		.reg_off = _reg,						\
		.reg_off2 = _reg2,						\
		.clk = {							\
			.write_enable_bit = _bit,				\
			.mul_min = _mul_min,					\
			.mul_max = _mul_max,					\
			.mul_shift = _mul_shift,				\
			.mul_mask = _mul_mask,					\
			.div_min = _div_min,					\
			.div_max = _div_max,					\
			.div_shift = _div_shift,				\
			.div_mask = _div_mask,					\
			.read_only = _read_only,				\
			.hw.init = CLK_HW_INIT_FW_NAME(#_var,			\
				   _parent,					\
				   &k230_clk_ops_##_method,			\
				   _flags),					\
		},								\
	}

#define K230_CLK_GATE_FORMAT(_var,						\
			     _reg, _bit, _flags, _gate_flags,			\
			     _parent)						\
	static struct k230_clk_gate k230_##_var = {				\
		.reg_off = _reg,						\
		.clk = {							\
			.bit_idx = _bit,					\
			.flags = _gate_flags,					\
			.hw.init = CLK_HW_INIT_HW(#_var,			\
				   &k230_##_parent.clk.hw, &clk_gate_ops, _flags),	\
		},								\
	}

#define K230_CLK_GATE_FORMAT_PDATA(_var,					\
				   _reg, _bit, _flags, _gate_flags,		\
				   _parent)					\
	static struct k230_clk_gate k230_##_var = {				\
		.reg_off = _reg,						\
		.clk = {							\
			.bit_idx = _bit,					\
			.flags = _gate_flags,					\
			.hw.init = CLK_HW_INIT_FW_NAME(#_var,			\
				   _parent, &clk_gate_ops, _flags),		\
		},								\
	}

#define K230_CLK_MUX_FORMAT(_var,						\
			    _reg, _shift, _mask, _flags, _mux_flags,		\
			    _parents)						\
	static struct k230_clk_mux k230_##_var = {				\
		.reg_off = _reg,						\
		.clk = {							\
			.flags = _mux_flags,					\
			.shift = _shift,					\
			.mask = _mask,						\
			.hw.init = CLK_HW_INIT_PARENTS_HW(#_var,		\
				   _parents, &clk_mux_ops, _flags),		\
		},								\
	}

#define K230_CLK_MUX_FORMAT_PDATA(_var,						\
				  _reg, _shift, _mask, _flags, _mux_flags,	\
				  _parents)					\
	static struct k230_clk_mux k230_##_var = {				\
		.reg_off = _reg,						\
		.clk = {							\
			.flags = _mux_flags,					\
			.shift = _shift,					\
			.mask = _mask,						\
			.hw.init = CLK_HW_INIT_PARENTS_DATA(#_var,		\
				   _parents, &clk_mux_ops, _flags),		\
		},								\
	}

#define K230_CLK_FIXED_RATE_FORMAT(_var,					\
				   _rate, _flags)				\
	static struct k230_clk_fixed_rate k230_##_var = {			\
		.clk = {							\
			.fixed_rate = _rate,					\
			.hw.init = CLK_HW_INIT_NO_PARENT(#_var,			\
				   &clk_fixed_rate_ops, _flags),		\
		},								\
	}

struct k230_pll_self {
	struct clk_hw	hw;
	void __iomem	*reg;
	/* ensures mutual exclusion for concurrent register access. */
	spinlock_t	*lock;
	int id;
};

struct k230_pll {
	struct k230_pll_self	clk;
};

#define hw_to_k230_pll_self(_hw) container_of(_hw, struct k230_pll_self, hw)

struct k230_clk_rate_self {
	struct clk_hw	hw;
	void __iomem	*reg;
	bool		read_only;
	u32		write_enable_bit;
	u32		mul_min;
	u32		mul_max;
	u32		mul_shift;
	u32		mul_mask;
	u32		div_min;
	u32		div_max;
	u32		div_shift;
	u32		div_mask;
	/* ensures mutual exclusion for concurrent register access. */
	spinlock_t	*lock;
};

#define hw_to_k230_clk_rate_self(_hw)	container_of(_hw,			\
					struct k230_clk_rate_self, hw)

struct k230_clk_rate {
	u32				reg_off;
	/* second register address restoring divider to calculate rate */
	u32				reg_off2;
	struct k230_clk_rate_self	clk;
};

static inline struct k230_clk_rate *hw_to_k230_clk_rate(struct clk_hw *hw)
{
	return container_of(hw_to_k230_clk_rate_self(hw), struct k230_clk_rate,
			    clk);
}

struct k230_clk_gate {
	u32			reg_off;
	struct clk_gate		clk;
};

struct k230_clk_mux {
	u32			reg_off;
	struct clk_mux		clk;
};

struct k230_clk_fixed_rate {
	struct clk_fixed_rate	clk;
};

struct k230_clk_fixed_factor {
	struct clk_fixed_factor clk;
};

static int k230_pll_prepare(struct clk_hw *hw);
static int k230_pll_enable(struct clk_hw *hw);
static void k230_pll_disable(struct clk_hw *hw);
static int k230_pll_is_enabled(struct clk_hw *hw);
static unsigned long k230_pll_get_rate(struct clk_hw *hw, unsigned long parent_rate);

static const struct clk_ops k230_pll_ops = {
	.prepare	= k230_pll_prepare,
	.enable	        = k230_pll_enable,
	.disable	= k230_pll_disable,
	.is_enabled	= k230_pll_is_enabled,
	.recalc_rate	= k230_pll_get_rate,
};

static int k230_clk_set_rate_mul(struct clk_hw *hw, unsigned long rate,
				 unsigned long parent_rate);
static long k230_clk_round_rate_mul(struct clk_hw *hw, unsigned long rate,
				    unsigned long *parent_rate);
static unsigned long k230_clk_get_rate_mul(struct clk_hw *hw,
					   unsigned long parent_rate);
static int k230_clk_set_rate_div(struct clk_hw *hw, unsigned long rate,
				 unsigned long parent_rate);
static long k230_clk_round_rate_div(struct clk_hw *hw, unsigned long rate,
				    unsigned long *parent_rate);
static unsigned long k230_clk_get_rate_div(struct clk_hw *hw,
					   unsigned long parent_rate);
static int k230_clk_set_rate_mul_div(struct clk_hw *hw, unsigned long rate,
				     unsigned long parent_rate);
static long k230_clk_round_rate_mul_div(struct clk_hw *hw, unsigned long rate,
					unsigned long *parent_rate);
static unsigned long k230_clk_get_rate_mul_div(struct clk_hw *hw,
					       unsigned long parent_rate);

/* clk_ops for clocks whose rate is determined by a configurable multiplier */
static const struct clk_ops k230_clk_ops_mul = {
	.set_rate	= k230_clk_set_rate_mul,
	.round_rate	= k230_clk_round_rate_mul,
	.recalc_rate	= k230_clk_get_rate_mul,
};

/* clk_ops for clocks whose rate is determined by a configurable divider */
static const struct clk_ops k230_clk_ops_div = {
	.set_rate	= k230_clk_set_rate_div,
	.round_rate	= k230_clk_round_rate_div,
	.recalc_rate	= k230_clk_get_rate_div,
};

/* clk_ops for clocks whose rate is determined by both a multiplier and a divider */
static const struct clk_ops k230_clk_ops_mul_div = {
	.set_rate	= k230_clk_set_rate_mul_div,
	.round_rate	= k230_clk_round_rate_mul_div,
	.recalc_rate	= k230_clk_get_rate_mul_div,
};

K230_CLK_PLL_FORMAT(pll0, 0, CLK_IS_CRITICAL, "osc24m");
K230_CLK_PLL_FORMAT(pll1, 1, CLK_IS_CRITICAL, "osc24m");
K230_CLK_PLL_FORMAT(pll2, 2, CLK_IS_CRITICAL, "osc24m");
K230_CLK_PLL_FORMAT(pll3, 3, CLK_IS_CRITICAL, "osc24m");

struct k230_pll *k230_plls[] = {
	K230_FMT(pll0),
	K230_FMT(pll1),
	K230_FMT(pll2),
	K230_FMT(pll3),
};

#define K230_PLL_NUM ARRAY_SIZE(k230_plls)

K230_CLK_PLL_DIV_FORMAT(pll0_div2, 2, 0, pll0);
K230_CLK_PLL_DIV_FORMAT(pll0_div3, 3, 0, pll0);
K230_CLK_PLL_DIV_FORMAT(pll0_div4, 4, 0, pll0);
K230_CLK_PLL_DIV_FORMAT(pll0_div16, 16, 0, pll0);
K230_CLK_PLL_DIV_FORMAT(pll1_div2, 2, 0, pll1);
K230_CLK_PLL_DIV_FORMAT(pll1_div3, 3, 0, pll1);
K230_CLK_PLL_DIV_FORMAT(pll1_div4, 4, 0, pll1);
K230_CLK_PLL_DIV_FORMAT(pll2_div2, 2, 0, pll2);
K230_CLK_PLL_DIV_FORMAT(pll2_div3, 3, 0, pll2);
K230_CLK_PLL_DIV_FORMAT(pll2_div4, 4, 0, pll2);
K230_CLK_PLL_DIV_FORMAT(pll3_div2, 2, 0, pll3);
K230_CLK_PLL_DIV_FORMAT(pll3_div3, 3, 0, pll3);
K230_CLK_PLL_DIV_FORMAT(pll3_div4, 4, 0, pll3);

struct k230_clk_fixed_factor *k230_pll_divs[] = {
	K230_FMT(pll0_div2),
	K230_FMT(pll0_div3),
	K230_FMT(pll0_div4),
	K230_FMT(pll0_div16),
	K230_FMT(pll1_div2),
	K230_FMT(pll1_div3),
	K230_FMT(pll1_div4),
	K230_FMT(pll2_div2),
	K230_FMT(pll2_div3),
	K230_FMT(pll2_div4),
	K230_FMT(pll3_div2),
	K230_FMT(pll3_div3),
	K230_FMT(pll3_div4),
};

#define K230_PLL_DIV_NUM ARRAY_SIZE(k230_pll_divs)

K230_CLK_GATE_FORMAT(cpu0_src_gate,
		     0, 0, 0, 0,
		     pll0_div2);

K230_CLK_RATE_FORMAT(cpu0_src_rate,
		     1, 16, 0, 0,
		     16, 16, 1, 0xf,
		     0x0, 31, mul, 0x0,
		     false, 0,
		     cpu0_src_gate);

K230_CLK_RATE_FORMAT(cpu0_axi_rate,
		     1, 1, 0, 0,
		     1, 8, 6, 0x7,
		     0x0, 31, div, 0x0,
		     0, 0,
		     cpu0_src_rate);

K230_CLK_GATE_FORMAT(cpu0_plic_gate,
		     0x0, 9, 0, 0,
		     cpu0_src_rate);

K230_CLK_RATE_FORMAT(cpu0_plic_rate,
		     1, 1, 0, 0,
		     1, 8, 10, 0x7,
		     0x0, 31, div, 0x0,
		     false, 0,
		     cpu0_plic_gate);

K230_CLK_GATE_FORMAT(cpu0_noc_ddrcp4_gate,
		     0x60, 7, 0, 0,
		     cpu0_src_rate);

K230_CLK_GATE_FORMAT(cpu0_apb_gate,
		     0x0, 13, 0, 0,
		     pll0_div4);

K230_CLK_RATE_FORMAT(cpu0_apb_rate,
		     1, 1, 0, 0,
		     1, 8, 15, 0x7,
		     0x0, 31, div, 0x0,
		     false, 0,
		     cpu0_apb_gate);

static const struct clk_hw *k230_parents_cpu1_src_mux[] = {
	&k230_pll0_div2.clk.hw,
	&k230_pll3.clk.hw,
	&k230_pll0.clk.hw,
};
K230_CLK_MUX_FORMAT(cpu1_src_mux,
		     0x4, 1, 0x3,
		     0, 0,
		    k230_parents_cpu1_src_mux);

K230_CLK_GATE_FORMAT(cpu1_src_gate,
		     0x4, 0, CLK_IGNORE_UNUSED, 0,
		     cpu1_src_mux);

K230_CLK_RATE_FORMAT(cpu1_src_rate,
		     1, 1, 0, 0,
		     1, 8, 3, 0x7,
		     0x4, 31, div, 0x0,
		     false, 0,
		     cpu1_src_gate);

K230_CLK_RATE_FORMAT(cpu1_axi_rate,
		     1, 1, 0, 0,
		     1, 8, 12, 0x7,
		     0x4, 31, div, 0x0,
		     false, 0,
		     cpu1_src_rate);

K230_CLK_GATE_FORMAT(cpu1_plic_gate,
		     0x4, 15, CLK_IGNORE_UNUSED, 0,
		     cpu1_src_rate);

K230_CLK_RATE_FORMAT(cpu1_plic_rate,
		     1, 1, 0, 0,
		     1, 8, 16, 0x7,
		     0x4, 31, div, 0x0,
		     false, 0,
		     cpu1_plic_gate);

K230_CLK_GATE_FORMAT(cpu1_apb_gate,
		     0x4, 19, 0, 0,
		     pll0_div4);

K230_CLK_RATE_FORMAT(cpu1_apb_rate,
		     1, 1, 0, 0,
		     1, 8, 15, 0x7,
		     0x0, 31, div, 0x0,
		     false, 0,
		     cpu1_apb_gate);

K230_CLK_GATE_FORMAT_PDATA(pmu_apb_gate,
			   0x10, 0, 0, 0,
			   "osc24m");

K230_CLK_RATE_FORMAT(hs_hclk_high_src_rate,
		     1, 1, 0, 0,
		     1, 8, 0, 0x7,
		     0x1C, 31, div, 0x0,
		     false, 0,
		     pll0_div4);

K230_CLK_GATE_FORMAT(hs_hclk_high_gate,
		     0x18, 1, 0, 0,
		     hs_hclk_high_src_rate);

K230_CLK_GATE_FORMAT(hs_hclk_src_gate,
		     0x18, 1, 0, 0,
		     hs_hclk_high_src_rate);

K230_CLK_RATE_FORMAT(hs_hclk_src_rate,
		     1, 1, 0, 0,
		     1, 8, 3, 0x7,
		     0x1C, 31, div, 0x0,
		     false, 0,
		     hs_hclk_src_gate);

K230_CLK_GATE_FORMAT(hs_sd0_ahb_gate,
		     0x18, 2, 0, 0,
		     hs_hclk_src_rate);

K230_CLK_GATE_FORMAT(hs_sd1_ahb_gate,
		     0x18, 3, 0, 0,
		     hs_hclk_src_rate);

K230_CLK_GATE_FORMAT(hs_ssi1_ahb_gate,
		     0x18, 7, 0, 0,
		     hs_hclk_src_rate);

K230_CLK_GATE_FORMAT(hs_ssi2_ahb_gate,
		     0x18, 8, 0, 0,
		     hs_hclk_src_rate);

K230_CLK_GATE_FORMAT(hs_usb0_ahb_gate,
		     0x18, 4, 0, 0,
		     hs_hclk_src_rate);

K230_CLK_GATE_FORMAT(hs_usb1_ahb_gate,
		     0x18, 5, 0, 0,
		     hs_hclk_src_rate);

K230_CLK_GATE_FORMAT(hs_ssi0_axi_gate,
		     0x18, 27, 0, 0,
		     pll0_div4);

K230_CLK_RATE_FORMAT(hs_ssi0_axi_rate,
		     1, 1, 0, 0,
		     1, 8, 9, 0x7,
		     0x20, 31, div, 0x0,
		     false, 0,
		     hs_ssi0_axi_gate);

K230_CLK_GATE_FORMAT(hs_ssi1_gate,
		     0x18, 25, 0, 0,
		     pll0_div4);

K230_CLK_RATE_FORMAT(hs_ssi1_rate,
		     1, 1, 0, 0,
		     1, 8, 3, 0x7,
		     0x20, 31, div, 0x0,
		     false, 0,
		     hs_ssi1_gate);

K230_CLK_GATE_FORMAT(hs_ssi2_gate,
		     0x18, 26, 0, 0,
		     pll0_div4);

K230_CLK_RATE_FORMAT(hs_ssi2_rate,
		     1, 1, 0, 0,
		     1, 8, 6, 0x7,
		     0x20, 31, div, 0x0,
		     false, 0,
		     hs_ssi2_gate);

K230_CLK_GATE_FORMAT(hs_qspi_axi_src_gate,
		     0x18, 28, 0, 0,
		     pll0_div4);

K230_CLK_RATE_FORMAT(hs_qspi_axi_src_rate,
		     1, 1, 0, 0,
		     1, 8, 12, 0x7,
		     0x20, 31, div, 0x0,
		     false, 0,
		     hs_qspi_axi_src_gate);

K230_CLK_GATE_FORMAT(hs_ssi1_axi_gate,
		     0x18, 29, 0, 0,
		     hs_qspi_axi_src_rate);

K230_CLK_GATE_FORMAT(hs_ssi2_axi_gate,
		     0x18, 30, 0, 0,
		     hs_qspi_axi_src_rate);

K230_CLK_GATE_FORMAT(hs_sd_card_src_gate,
		     0x18, 11, 0, 0,
		     pll0_div4);

K230_CLK_RATE_FORMAT(hs_sd_card_src_rate,
		     1, 1, 0, 0,
		     2, 8, 12, 0x7,
		     0x1C, 31, div, 0x0,
		     false, 0,
		     pll0_div4);

K230_CLK_GATE_FORMAT(hs_sd0_card_gate,
		     0x18, 15, 0, 0,
		     hs_sd_card_src_rate);

K230_CLK_GATE_FORMAT(hs_sd1_card_gate,
		     0x18, 19, 0, 0,
		     hs_sd_card_src_rate);

K230_CLK_GATE_FORMAT(hs_sd_axi_src_gate,
		     0x18, 9, 0, 0,
		     pll2_div4);

K230_CLK_RATE_FORMAT(hs_sd_axi_src_rate,
		     1, 1, 0, 0,
		     1, 8, 6, 0x7,
		     0x1C, 31, div, 0x0,
		     false, 0,
		     hs_sd_axi_src_gate);

K230_CLK_GATE_FORMAT(hs_sd0_axi_gate,
		     0x18, 13, 0, 0,
		     hs_sd_axi_src_rate);

K230_CLK_GATE_FORMAT(hs_sd1_axi_gate,
		     0x18, 17, 0, 0,
		     hs_sd_axi_src_rate);

K230_CLK_GATE_FORMAT(hs_sd0_base_gate,
		     0x18, 14, 0, 0,
		     hs_sd_axi_src_rate);

K230_CLK_GATE_FORMAT(hs_sd1_base_gate,
		     0x18, 18, 0, 0,
		     hs_sd_axi_src_rate);

static const struct clk_hw *k230_parents_hs_ospi_src_mux[] = {
	&k230_pll0_div2.clk.hw,
	&k230_pll2_div4.clk.hw,
};
K230_CLK_MUX_FORMAT(hs_ospi_src_mux,
		     0x20, 18, 0x1,
		     0, 0,
		     k230_parents_hs_ospi_src_mux);

K230_CLK_GATE_FORMAT(hs_ospi_src_gate,
		     0x18, 24, CLK_IGNORE_UNUSED, 0,
		     hs_ospi_src_mux);

K230_CLK_RATE_FORMAT(hs_usb_ref_50m_rate,
		     1, 1, 0, 0,
		     1, 8, 15, 0x7,
		     0x20, 31, div, 0x0,
		     false, 0,
		     pll0_div16);

K230_CLK_GATE_FORMAT_PDATA(hs_sd_timer_src_gate,
			   0x18, 12, 0, 0,
			   "osc24m");

K230_CLK_RATE_FORMAT(hs_sd_timer_src_rate,
		     1, 1, 0, 0,
		     24, 32, 15, 0x1F,
		     0x1C, 31, div, 0x0,
		     false, 0,
		     hs_sd_timer_src_gate);

K230_CLK_GATE_FORMAT(hs_sd0_timer_gate,
		     0x18, 16, 0, 0,
		     hs_sd_timer_src_rate);

K230_CLK_GATE_FORMAT(hs_sd1_timer_gate,
		     0x18, 20, 0, 0,
		     hs_sd_timer_src_rate);

static const struct clk_parent_data k230_parents_hs_usb0_ref_mux[] = {
	{ .fw_name = "osc24m", },
	{ .hw = &k230_hs_usb_ref_50m_rate.clk.hw },
};
K230_CLK_MUX_FORMAT_PDATA(hs_usb0_ref_mux,
			  0x18, 23, 0x1,
			  0, 0,
			  k230_parents_hs_usb0_ref_mux);

K230_CLK_GATE_FORMAT(hs_usb0_ref_gate,
		     0x18, 21, CLK_IGNORE_UNUSED, 0,
		     hs_usb0_ref_mux);

static const struct clk_parent_data k230_parents_hs_usb1_ref_mux[] = {
	{ .fw_name = "osc24m", },
	{ .hw = &k230_hs_usb_ref_50m_rate.clk.hw },
};
K230_CLK_MUX_FORMAT_PDATA(hs_usb1_ref_mux,
			  0x18, 23, 0x1,
			  0, 0,
			  k230_parents_hs_usb1_ref_mux);

K230_CLK_GATE_FORMAT(hs_usb1_ref_gate,
		     0x18, 22, CLK_IGNORE_UNUSED, 0,
		     hs_usb1_ref_mux);

K230_CLK_GATE_FORMAT(ls_apb_src_gate,
		     0x24, 0, CLK_IS_CRITICAL, 0,
		     pll0_div4);

K230_CLK_RATE_FORMAT(ls_apb_src_rate,
		     1, 1, 0, 0,
		     1, 8, 0, 0x7,
		     0x30, 31, div, 0x0,
		     false, 0,
		     ls_apb_src_gate);

K230_CLK_GATE_FORMAT(ls_uart0_apb_gate,
		     0x24, 1, CLK_IS_CRITICAL, 0,
		     ls_apb_src_rate);

K230_CLK_GATE_FORMAT(ls_uart1_apb_gate,
		     0x24, 2, CLK_IS_CRITICAL, 0,
		     ls_apb_src_rate);

K230_CLK_GATE_FORMAT(ls_uart2_apb_gate,
		     0x24, 3, CLK_IS_CRITICAL, 0,
		     ls_apb_src_rate);

K230_CLK_GATE_FORMAT(ls_uart3_apb_gate,
		     0x24, 4, CLK_IS_CRITICAL, 0,
		     ls_apb_src_rate);

K230_CLK_GATE_FORMAT(ls_uart4_apb_gate,
		     0x24, 5, CLK_IS_CRITICAL, 0,
		     ls_apb_src_rate);

K230_CLK_GATE_FORMAT(ls_i2c0_apb_gate,
		     0x24, 6, 0, 0,
		     ls_apb_src_rate);

K230_CLK_GATE_FORMAT(ls_i2c1_apb_gate,
		     0x24, 7, 0, 0,
		     ls_apb_src_rate);

K230_CLK_GATE_FORMAT(ls_i2c2_apb_gate,
		     0x24, 8, 0, 0,
		     ls_apb_src_rate);

K230_CLK_GATE_FORMAT(ls_i2c3_apb_gate,
		     0x24, 9, 0, 0,
		     ls_apb_src_rate);

K230_CLK_GATE_FORMAT(ls_i2c4_apb_gate,
		     0x24, 10, 0, 0,
		     ls_apb_src_rate);

K230_CLK_GATE_FORMAT(ls_gpio_apb_gate,
		     0x24, 11, 0, 0,
		     ls_apb_src_rate);

K230_CLK_GATE_FORMAT(ls_pwm_apb_gate,
		     0x24, 12, 0, 0,
		     ls_apb_src_rate);

K230_CLK_GATE_FORMAT(ls_jamlink0_apb_gate,
		     0x28, 4, 0, 0,
		     ls_apb_src_rate);

K230_CLK_GATE_FORMAT(ls_jamlink1_apb_gate,
		     0x28, 5, 0, 0,
		     ls_apb_src_rate);

K230_CLK_GATE_FORMAT(ls_jamlink2_apb_gate,
		     0x28, 6, 0, 0,
		     ls_apb_src_rate);

K230_CLK_GATE_FORMAT(ls_jamlink3_apb_gate,
		     0x28, 7, 0, 0,
		     ls_apb_src_rate);

K230_CLK_GATE_FORMAT(ls_audio_apb_gate,
		     0x24, 13, 0, 0,
		     ls_apb_src_rate);

K230_CLK_GATE_FORMAT(ls_adc_apb_gate,
		     0x24, 15, 0, 0,
		     ls_apb_src_rate);

K230_CLK_GATE_FORMAT(ls_codec_apb_gate,
		     0x24, 14, 0, 0,
		     pll0_div4);

K230_CLK_GATE_FORMAT(ls_i2c0_gate,
		     0x24, 21, 0, 0,
		     pll0_div4);

K230_CLK_RATE_FORMAT(ls_i2c0_rate,
		     1, 1, 0, 0,
		     1, 8, 15, 0x7,
		     0x2C, 31, div, 0x0,
		     false, 0,
		     ls_i2c0_gate);

K230_CLK_GATE_FORMAT(ls_i2c1_gate,
		     0x24, 22, 0, 0,
		     pll0_div4);

K230_CLK_RATE_FORMAT(ls_i2c1_rate,
		     1, 1, 0, 0,
		     1, 8, 18, 0x7,
		     0x2C, 31, div, 0x0,
		     false, 0,
		     ls_i2c1_gate);

K230_CLK_GATE_FORMAT(ls_i2c2_gate,
		     0x24, 23, 0, 0,
		     pll0_div4);

K230_CLK_RATE_FORMAT(ls_i2c2_rate,
		     1, 1, 0, 0,
		     1, 8, 21, 0x7,
		     0x2C, 31, div, 0x0,
		     false, 0,
		     ls_i2c2_gate);

K230_CLK_GATE_FORMAT(ls_i2c3_gate,
		     0x24, 24, 0, 0,
		     pll0_div4);

K230_CLK_RATE_FORMAT(ls_i2c3_rate,
		     1, 1, 0, 0,
		     1, 8, 24, 0x7,
		     0x2C, 31, div, 0x0,
		     false, 0,
		     ls_i2c3_gate);

K230_CLK_GATE_FORMAT(ls_i2c4_gate,
		     0x24, 25, 0, 0,
		     pll0_div4);

K230_CLK_RATE_FORMAT(ls_i2c4_rate,
		     1, 1, 0, 0,
		     1, 8, 27, 0x7,
		     0x2C, 31, div, 0x0,
		     false, 0,
		     ls_i2c4_gate);

K230_CLK_GATE_FORMAT(ls_codec_adc_gate,
		     0x24, 29, 0, 0,
		     pll0_div4);

K230_CLK_RATE_FORMAT(ls_codec_adc_rate,
		     0x10, 0x1B9, 14, 0x1FFF,
		     0xC35, 0x3D09, 0, 0x3FFF,
		     0x38, 31, mul_div, 0x0,
		     false, 0,
		     ls_codec_adc_gate);

K230_CLK_GATE_FORMAT(ls_codec_dac_gate,
		     0x24, 30, 0, 0,
		     pll0_div4);

K230_CLK_RATE_FORMAT(ls_codec_dac_rate,
		     0x10, 0x1B9, 14, 0x1FFF,
		     0xC35, 0x3D09, 0, 0x3FFF,
		     0x3C, 31, mul_div, 0x0,
		     false, 0,
		     ls_codec_dac_gate);

K230_CLK_GATE_FORMAT(ls_audio_dev_gate,
		     0x24, 28, 0, 0,
		     pll0_div4);

K230_CLK_RATE_FORMAT(ls_audio_dev_rate,
		     0x4, 0x1B9, 16, 0x7FFF,
		     0xC35, 0xF424, 0, 0xFFFF,
		     0x34, 31, mul_div, 0x0,
		     false, 0,
		     pll0_div4);

K230_CLK_GATE_FORMAT(ls_pdm_gate,
		     0x24, 31, 0, 0,
		     pll0_div4);

K230_CLK_RATE_FORMAT(ls_pdm_rate,
		     0x2, 0x1B9, 0, 0xFFFF,
		     0xC35, 0x1E848, 0, 0x1FFFF,
		     0x40, 0, mul_div, 0x44,
		     false, 0,
		     ls_pdm_gate);

K230_CLK_GATE_FORMAT(ls_adc_gate,
		     0x24, 26, 0, 0,
		     pll0_div4);

K230_CLK_RATE_FORMAT(ls_adc_rate,
		     1, 1, 0, 0,
		     1, 1024, 3, 0x3FF,
		     0x30, 31, div, 0x0,
		     false, 0,
		     ls_adc_gate);

K230_CLK_GATE_FORMAT(ls_uart0_gate,
		     0x24, 16, CLK_IS_CRITICAL, 0,
		     pll0_div16);

K230_CLK_RATE_FORMAT(ls_uart0_rate,
		     1, 1, 0, 0,
		     1, 8, 0, 0x7,
		     0x2C, 31, div, 0x0,
		     false, 0,
		     ls_uart0_gate);

K230_CLK_GATE_FORMAT(ls_uart1_gate,
		     0x24, 17, CLK_IS_CRITICAL, 0,
		     pll0_div16);

K230_CLK_RATE_FORMAT(ls_uart1_rate,
		     1, 1, 0, 0,
		     1, 8, 3, 0x7,
		     0x2C, 31, div, 0x0,
		     false, 0,
		     ls_uart1_gate);

K230_CLK_GATE_FORMAT(ls_uart2_gate,
		     0x24, 18, CLK_IS_CRITICAL, 0,
		     pll0_div16);

K230_CLK_RATE_FORMAT(ls_uart2_rate,
		     1, 1, 0, 0,
		     1, 8, 6, 0x7,
		     0x2C, 31, div, 0x0,
		     false, 0,
		     ls_uart2_gate);

K230_CLK_GATE_FORMAT(ls_uart3_gate,
		     0x24, 19, CLK_IS_CRITICAL, 0,
		     pll0_div16);

K230_CLK_RATE_FORMAT(ls_uart3_rate,
		     1, 1, 0, 0,
		     1, 8, 9, 0x7,
		     0x2C, 31, div, 0x0,
		     false, 0,
		     ls_uart3_gate);

K230_CLK_GATE_FORMAT(ls_uart4_gate,
		     0x24, 20, CLK_IS_CRITICAL, 0,
		     pll0_div16);

K230_CLK_RATE_FORMAT(ls_uart4_rate,
		     1, 1, 0, 0,
		     1, 8, 12, 0x7,
		     0x2C, 31, div, 0x0,
		     false, 0,
		     ls_uart4_gate);

K230_CLK_RATE_FORMAT(ls_jamlinkco_src_rate,
		     1, 1, 0, 0,
		     2, 512, 23, 0xFF,
		     0x30, 31, div, 0x0,
		     false, 0,
		     pll0_div16);

K230_CLK_GATE_FORMAT(ls_jamlink0co_gate,
		     0x28, 0, 0, 0,
		     ls_jamlinkco_src_rate);

K230_CLK_GATE_FORMAT(ls_jamlink1co_gate,
		     0x28, 1, 0, 0,
		     ls_jamlinkco_src_rate);

K230_CLK_GATE_FORMAT(ls_jamlink2co_gate,
		     0x28, 2, 0, 0,
		     ls_jamlinkco_src_rate);

K230_CLK_GATE_FORMAT(ls_jamlink3co_gate,
		     0x28, 3, 0, 0,
		     ls_jamlinkco_src_rate);

K230_CLK_GATE_FORMAT_PDATA(ls_gpio_debounce_gate,
			   0x24, 27, 0, 0,
			   "osc24m");

K230_CLK_RATE_FORMAT(ls_gpio_debounce_rate,
		     1, 1, 0, 0,
		     1, 1024, 13, 0x3FF,
		     0x30, 31, div, 0x0,
		     false, 0,
		     ls_gpio_debounce_gate);

K230_CLK_FIXED_RATE_FORMAT(sysctl_apb_src, 100000000, 0);

K230_CLK_GATE_FORMAT(sysctl_wdt0_apb_gate,
		     0x50, 1, 0, 0,
		     sysctl_apb_src);

K230_CLK_GATE_FORMAT(sysctl_wdt1_apb_gate,
		     0x50, 2, 0, 0,
		     sysctl_apb_src);

K230_CLK_GATE_FORMAT(sysctl_timer_apb_gate,
		     0x50, 3, 0, 0,
		     sysctl_apb_src);

K230_CLK_GATE_FORMAT(sysctl_iomux_apb_gate,
		     0x50, 20, 0, 0,
		     sysctl_apb_src);

K230_CLK_GATE_FORMAT(sysctl_mailbox_apb_gate,
		     0x50, 4, 0, 0,
		     sysctl_apb_src);

K230_CLK_GATE_FORMAT(sysctl_hdi_gate,
		     0x50, 21, 0, 0,
		     pll0_div4);

K230_CLK_RATE_FORMAT(sysctl_hdi_rate,
		     1, 1, 0, 0,
		     1, 8, 28, 0x7,
		     0x58, 31, div, 0x0,
		     false, 0,
		     sysctl_hdi_gate);

K230_CLK_GATE_FORMAT(sysctl_time_stamp_gate,
		     0x50, 19, CLK_IS_CRITICAL, 0,
		     pll1_div4);

K230_CLK_RATE_FORMAT(sysctl_time_stamp_rate,
		     1, 1, 0, 0,
		     1, 32, 15, 0x1F,
		     0x58, 31, div, 0x0,
		     false, 0,
		     sysctl_time_stamp_gate);

K230_CLK_RATE_FORMAT_PDATA(sysctl_temp_sensor_rate,
			   1, 1, 0, 0,
			   1, 256, 20, 0xFF,
			   0x58, 31, div, 0x0,
			   false, 0,
			   "osc24m");

K230_CLK_GATE_FORMAT_PDATA(sysctl_wdt0_gate,
			   0x50, 4, 0, 0,
			   "osc24m");

K230_CLK_RATE_FORMAT(sysctl_wdt0_rate,
		     1, 1, 0, 0,
		     1, 64, 3, 0x3F,
		     0x58, 31, div, 0x0,
		     false, 0,
		     sysctl_wdt0_gate);

K230_CLK_GATE_FORMAT_PDATA(sysctl_wdt1_gate,
			   0x50, 4, 0, 0,
			   "osc24m");

K230_CLK_RATE_FORMAT(sysctl_wdt1_rate,
		     1, 1, 0, 0,
		     1, 64, 3, 0x3F,
		     0x58, 31, div, 0x0,
		     false, 0,
		     sysctl_wdt1_gate);

K230_CLK_RATE_FORMAT(timer0_src_rate,
		     1, 1, 0, 0,
		     1, 8, 0, 0x7,
		     0x54, 31, div, 0x0,
		     false, 0,
		     pll0_div16);

K230_CLK_RATE_FORMAT(timer1_src_rate,
		     1, 1, 0, 0,
		     1, 8, 3, 0x7,
		     0x54, 31, div, 0x0,
		     false, 0,
		     pll0_div16);

K230_CLK_RATE_FORMAT(timer2_src_rate,
		     1, 1, 0, 0,
		     1, 8, 6, 0x7,
		     0x54, 31, div, 0x0,
		     false, 0,
		     pll0_div16);

K230_CLK_RATE_FORMAT(timer3_src_rate,
		     1, 1, 0, 0,
		     1, 8, 9, 0x7,
		     0x54, 31, div, 0x0,
		     false, 0,
		     pll0_div16);

K230_CLK_RATE_FORMAT(timer4_src_rate,
		     1, 1, 0, 0,
		     1, 8, 12, 0x7,
		     0x54, 31, div, 0x0,
		     false, 0,
		     pll0_div16);

K230_CLK_RATE_FORMAT(timer5_src_rate,
		     1, 1, 0, 0,
		     1, 8, 15, 0x7,
		     0x54, 31, div, 0x0,
		     false, 0,
		     pll0_div16);

static const struct clk_parent_data k230_parents_timer0_mux[] = {
	{ .fw_name = "timer-pulse-in", },
	{ .hw = &k230_timer0_src_rate.clk.hw },
};
K230_CLK_MUX_FORMAT_PDATA(timer0_mux,
			  0x50, 7, 0x1,
			  0, 0,
			  k230_parents_timer0_mux);

K230_CLK_GATE_FORMAT(timer0_gate,
		     0x50, 13, CLK_IGNORE_UNUSED, 0,
		     timer0_mux);

static const struct clk_parent_data k230_parents_timer1_mux[] = {
	{ .fw_name = "timer-pulse-in", },
	{ .hw = &k230_timer1_src_rate.clk.hw },
};
K230_CLK_MUX_FORMAT_PDATA(timer1_mux,
			  0x50, 8, 0x1,
			  0, 0,
			  k230_parents_timer1_mux);

K230_CLK_GATE_FORMAT(timer1_gate,
		     0x50, 14, CLK_IGNORE_UNUSED, 0,
		     timer1_mux);

static const struct clk_parent_data k230_parents_timer2_mux[] = {
	{ .fw_name = "timer-pulse-in", },
	{ .hw = &k230_timer2_src_rate.clk.hw },
};
K230_CLK_MUX_FORMAT_PDATA(timer2_mux,
			  0x50, 9, 0x1,
			  0, 0,
			  k230_parents_timer2_mux);

K230_CLK_GATE_FORMAT(timer2_gate,
		     0x50, 15, CLK_IGNORE_UNUSED, 0,
		     timer2_mux);

static const struct clk_parent_data k230_parents_timer3_mux[] = {
	{ .fw_name = "timer-pulse-in", },
	{ .hw = &k230_timer3_src_rate.clk.hw },
};
K230_CLK_MUX_FORMAT_PDATA(timer3_mux,
			  0x50, 10, 0x1,
			  0, 0,
			  k230_parents_timer3_mux);

K230_CLK_GATE_FORMAT(timer3_gate,
		     0x50, 16, CLK_IGNORE_UNUSED, 0,
		     timer3_mux);

static const struct clk_parent_data k230_parents_timer4_mux[] = {
	{ .fw_name = "timer-pulse-in", },
	{ .hw = &k230_timer4_src_rate.clk.hw },
};
K230_CLK_MUX_FORMAT_PDATA(timer4_mux,
			  0x50, 11, 0x1,
			  0, 0,
			  k230_parents_timer4_mux);

K230_CLK_GATE_FORMAT(timer4_gate,
		     0x50, 17, CLK_IGNORE_UNUSED, 0,
		     timer4_mux);

static const struct clk_parent_data k230_parents_timer5_mux[] = {
	{ .fw_name = "timer-pulse-in", },
	{ .hw = &k230_timer5_src_rate.clk.hw },
};
K230_CLK_MUX_FORMAT_PDATA(timer5_mux,
			  0x50, 12, 0x1,
			  0, 0,
			  k230_parents_timer5_mux);

K230_CLK_GATE_FORMAT(timer5_gate,
		     0x50, 18, CLK_IGNORE_UNUSED, 0,
		     timer5_mux);

K230_CLK_GATE_FORMAT(shrm_apb_gate,
		     0x5C, 0, 0, 0,
		     pll0_div4);

K230_CLK_RATE_FORMAT(shrm_apb_rate,
		     1, 1, 0, 0,
		     1, 8, 18, 0x7,
		     0x5C, 31, div, 0x0,
		     false, 0,
		     shrm_apb_gate);

static const struct clk_hw *k230_parents_shrm_sram_mux[] = {
	&k230_pll3_div2.clk.hw,
	&k230_pll0_div2.clk.hw,
};
K230_CLK_MUX_FORMAT(shrm_sram_mux,
		    0x50, 14, 0x1,
		    0, 0,
		    k230_parents_shrm_sram_mux);

K230_CLK_GATE_FORMAT(shrm_sram_gate,
		     0x5c, 10, CLK_IGNORE_UNUSED, 0,
		     shrm_sram_mux);

K230_CLK_FIXED_FACTOR_FORMAT(shrm_sram_div2,
			     1, 2, 0,
			     shrm_sram_gate);

K230_CLK_GATE_FORMAT(shrm_axi_slave_gate,
		     0x5C, 11, CLK_IGNORE_UNUSED, 0,
		     shrm_sram_div2);

K230_CLK_GATE_FORMAT(shrm_axi_gate,
		     0x5C, 12, 0, 0,
		     pll0_div4);

K230_CLK_GATE_FORMAT(shrm_nonai2d_axi_gate,
		     0x5C, 9, 0, 0,
		     shrm_axi_gate);

K230_CLK_GATE_FORMAT(shrm_decompress_axi_gate,
		     0x5C, 7, CLK_IGNORE_UNUSED, 0,
		     shrm_sram_gate);

K230_CLK_GATE_FORMAT(shrm_sdma_axi_gate,
		     0x5C, 5, 0, 0,
		     shrm_axi_gate);

K230_CLK_GATE_FORMAT(shrm_pdma_axi_gate,
		     0x5C, 3, 0, 0,
		     shrm_axi_gate);

static const struct clk_hw *k230_parents_ddrc_src_mux[] = {
	&k230_pll0_div2.clk.hw,
	&k230_pll0_div3.clk.hw,
	&k230_pll2_div4.clk.hw,
};
K230_CLK_MUX_FORMAT(ddrc_src_mux,
		    0x60, 0, 0x3,
		    0, 0,
		    k230_parents_ddrc_src_mux);

K230_CLK_GATE_FORMAT(ddrc_src_gate,
		     0x60, 2, CLK_IGNORE_UNUSED, 0,
		     ddrc_src_mux);

K230_CLK_RATE_FORMAT(ddrc_src_rate,
		     1, 1, 0, 0,
		     1, 16, 10, 0xF,
		     0x60, 31, div, 0x0,
		     false, 0,
		     ddrc_src_gate);

K230_CLK_GATE_FORMAT(ddrc_bypass_gate,
		     0x60, 8, 0, 0,
		     pll2_div4);

K230_CLK_GATE_FORMAT(ddrc_apb_gate,
		     0x60, 9, 0, 0,
		     pll0_div4);

K230_CLK_RATE_FORMAT(ddrc_apb_rate,
		     1, 1, 0, 0,
		     1, 16, 14, 0xF,
		     0x60, 31, div, 0x0,
		     false, 0,
		     ddrc_apb_gate);

K230_CLK_GATE_FORMAT(display_ahb_gate,
		     0x74, 0, 0, 0,
		     pll0_div4);

K230_CLK_RATE_FORMAT(display_ahb_rate,
		     1, 1, 0, 0,
		     1, 8, 0, 0x7,
		     0x78, 31, div, 0x0,
		     false, 0,
		     display_ahb_gate);

K230_CLK_GATE_FORMAT(display_axi_gate,
		     0x74, 1, 0, 0,
		     pll0_div4);

K230_CLK_RATE_FORMAT(display_clkext_rate,
		     1, 1, 0, 0,
		     1, 16, 16, 0xF,
		     0x78, 31, div, 0x0,
		     false, 0,
		     display_axi_gate);

K230_CLK_GATE_FORMAT(display_gpu_gate,
		     0x74, 6, 0, 0,
		     pll0_div3);

K230_CLK_RATE_FORMAT(display_gpu_rate,
		     1, 1, 0, 0,
		     1, 16, 20, 0xF,
		     0x78, 31, div, 0x0,
		     false, 0,
		     display_gpu_gate);

K230_CLK_GATE_FORMAT(display_dpip_gate,
		     0x74, 2, 0, 0,
		     pll1_div4);

K230_CLK_RATE_FORMAT(display_dpip_rate,
		     1, 1, 0, 0,
		     1, 256, 3, 0xFF,
		     0x78, 31, div, 0x0,
		     false, 0,
		     display_dpip_gate);

K230_CLK_GATE_FORMAT(display_cfg_gate,
		     0x74, 4, 0, 0,
		     pll1_div4);

K230_CLK_RATE_FORMAT(display_cfg_rate,
		     1, 1, 0, 0,
		     1, 32, 11, 0x1F,
		     0x78, 31, div, 0x0,
		     false, 0,
		     display_cfg_gate);

K230_CLK_GATE_FORMAT_PDATA(display_ref_gate,
			   0x74, 3, 0, 0,
			   "osc24m");

K230_CLK_GATE_FORMAT(vpu_src_gate,
		     0xC, 0, 0, 0,
		     pll0_div2);

K230_CLK_RATE_FORMAT(vpu_src_rate,
		     1, 16, 0, 0,
		     16, 16, 1, 0xF,
		     0xC, 31, mul, 0x0,
		     false, 0,
		     vpu_src_gate);

K230_CLK_RATE_FORMAT(vpu_axi_src_rate,
		     1, 1, 0, 0,
		     1, 16, 6, 0xF,
		     0xC, 31, div, 0x0,
		     false, 0,
		     vpu_src_rate);

K230_CLK_GATE_FORMAT(vpu_axi_gate,
		     0xC, 5, 0, 0,
		     vpu_axi_src_rate);

K230_CLK_GATE_FORMAT(vpu_ddrcp2_gate,
		     0x60, 5, 0, 0,
		     vpu_axi_src_rate);

K230_CLK_GATE_FORMAT(vpu_cfg_gate,
		     0xC, 10, 0, 0,
		     pll0_div4);

K230_CLK_RATE_FORMAT(vpu_cfg_rate,
		     1, 1, 0, 0,
		     1, 16, 11, 0xF,
		     0xC, 31, div, 0x0,
		     false, 0,
		     vpu_cfg_gate);

K230_CLK_GATE_FORMAT(sec_apb_gate,
		     0x80, 0, 0, 0,
		     pll0_div4);

K230_CLK_RATE_FORMAT(sec_apb_rate,
		     1, 1, 0, 0,
		     1, 8, 1, 0x7,
		     0x80, 31, div, 0x0,
		     false, 0,
		     sec_apb_gate);

K230_CLK_GATE_FORMAT(sec_fix_gate,
		     0x80, 5, 0, 0,
		     pll1_div4);

K230_CLK_RATE_FORMAT(sec_fix_rate,
		     1, 1, 0, 0,
		     1, 32, 6, 0x1F,
		     0x80, 31, div, 0x0,
		     false, 0,
		     sec_fix_gate);

K230_CLK_GATE_FORMAT(sec_axi_gate,
		     0x80, 4, 0, 0,
		     pll1_div4);

K230_CLK_RATE_FORMAT(sec_axi_rate,
		     1, 1, 0, 0,
		     1, 8, 11, 0x3,
		     0x80, 31, div, 0,
		     false, 0,
		     sec_axi_gate);

K230_CLK_GATE_FORMAT(usb_480m_gate,
		     0x100, 0, 0, 0,
		     pll1);

K230_CLK_RATE_FORMAT(usb_480m_rate,
		     1, 1, 0, 0,
		     1, 8, 1, 0x7,
		     0x100, 31, div, 0,
		     false, 0,
		     usb_480m_gate);

K230_CLK_GATE_FORMAT(usb_100m_gate,
		     0x100, 0, 0, 0,
		     pll0_div4);

K230_CLK_RATE_FORMAT(usb_100m_rate,
		     1, 1, 0, 0,
		     1, 8, 4, 0x7,
		     0x100, 31, div, 0,
		     false, 0,
		     usb_100m_gate);

K230_CLK_GATE_FORMAT(dphy_dft_gate,
		     0x100, 0, 0, 0,
		     pll0);

K230_CLK_RATE_FORMAT(dphy_dft_rate,
		     1, 1, 0, 0,
		     1, 16, 1, 0xF,
		     0x104, 31, div, 0,
		     false, 0,
		     dphy_dft_gate);

K230_CLK_GATE_FORMAT(spi2axi_gate,
		     0x108, 0, 0, 0,
		     pll0_div4);

K230_CLK_RATE_FORMAT(spi2axi_rate,
		     1, 1, 0, 0,
		     1, 8, 1, 0x7,
		     0x108, 31, div, 0x0,
		     false, 0,
		     spi2axi_gate);

static const struct clk_hw *k230_parents_ai_src_mux[] = {
	&k230_pll0_div2.clk.hw,
	&k230_pll3_div2.clk.hw,
};
K230_CLK_MUX_FORMAT(ai_src_mux,
		    0x8, 2, 0x1,
		    0, 0,
		    k230_parents_ai_src_mux);

K230_CLK_GATE_FORMAT(ai_src_gate,
		     0x8, 0, CLK_IGNORE_UNUSED, 0,
		     ai_src_mux);

K230_CLK_RATE_FORMAT(ai_src_rate,
		     1, 1, 0, 0,
		     1, 8, 3, 0x7,
		     0x8, 31, div, 0x0,
		     false, 0,
		     ai_src_gate);

K230_CLK_GATE_FORMAT(ai_axi_gate,
		     0x8, 10, 0, 0,
		     ai_src_rate);

static const struct clk_hw *k230_parents_camera0_mux[] = {
	&k230_pll1_div3.clk.hw,
	&k230_pll1_div4.clk.hw,
	&k230_pll0_div4.clk.hw,
};
K230_CLK_MUX_FORMAT(camera0_mux,
		    0x6C, 3, 0x3,
		    0, 0,
		    k230_parents_camera0_mux);

K230_CLK_GATE_FORMAT(camera0_gate,
		     0x6C, 0, CLK_IGNORE_UNUSED, 0,
		     camera0_mux);

K230_CLK_RATE_FORMAT(camera0_rate,
		     1, 1, 0, 0,
		     1, 32, 5, 0x1f,
		     0x6C, 31, div, 0x0,
		     false, 0,
		     camera0_gate);

static const struct clk_hw *k230_parents_camera1_mux[] = {
	&k230_pll1_div3.clk.hw,
	&k230_pll1_div4.clk.hw,
	&k230_pll0_div4.clk.hw,
};
K230_CLK_MUX_FORMAT(camera1_mux,
		    0x6C, 10, 0x3,
		    0, 0,
		    k230_parents_camera1_mux);

K230_CLK_GATE_FORMAT(camera1_gate,
		     0x6C, 1, CLK_IGNORE_UNUSED, 0,
		     camera1_mux);

K230_CLK_RATE_FORMAT(camera1_rate,
		     1, 1, 0, 0,
		     1, 32, 12, 0x1f,
		     0x6C, 31, div, 0x0,
		     false, 0,
		     camera1_gate);

static const struct clk_hw *k230_parents_camera2_mux[] = {
	&k230_pll1_div3.clk.hw,
	&k230_pll1_div4.clk.hw,
	&k230_pll0_div4.clk.hw,
};
K230_CLK_MUX_FORMAT(camera2_mux,
		    0x6C, 17, 0x3,
		    0, 0,
		    k230_parents_camera2_mux);

K230_CLK_GATE_FORMAT(camera2_gate,
		     0x6C, 2, CLK_IGNORE_UNUSED, 0,
		     camera2_mux);

K230_CLK_RATE_FORMAT(camera2_rate,
		     1, 1, 0, 0,
		     1, 32, 19, 0x1f,
		     0x6C, 31, div, 0x0,
		     false, 0,
		     camera2_gate);

static int k230_pll_prepare(struct clk_hw *hw)
{
	struct k230_pll_self *pll = hw_to_k230_pll_self(hw);
	u32 reg;

	/* wait for PLL lock until it reaches lock status */
	return readl_poll_timeout(K230_PLLX_LOCK_ADDR(pll->reg, pll->id), reg,
				  reg & K230_PLL_LOCK_STATUS_MASK,
				  K230_PLL_LOCK_TIME_DELAY, K230_PLL_LOCK_TIMEOUT);
}

static inline bool k230_pll_hw_is_enabled(struct k230_pll_self *pll)
{
	return readl(K230_PLLX_GATE_ADDR(pll->reg, pll->id)) & K230_PLL_GATE_ENABLE;
}

static void k230_pll_enable_hw(struct k230_pll_self *pll)
{
	u32 reg;

	if (k230_pll_hw_is_enabled(pll))
		return;

	/* Set PLL factors */
	reg = readl(K230_PLLX_GATE_ADDR(pll->reg, pll->id));
	reg |= K230_PLL_GATE_ENABLE | K230_PLL_GATE_WRITE_ENABLE;
	writel(reg, K230_PLLX_GATE_ADDR(pll->reg, pll->id));
}

static int k230_pll_enable(struct clk_hw *hw)
{
	struct k230_pll_self *pll = hw_to_k230_pll_self(hw);

	guard(spinlock)(pll->lock);

	k230_pll_enable_hw(pll);

	return 0;
}

static void k230_pll_disable(struct clk_hw *hw)
{
	struct k230_pll_self *pll = hw_to_k230_pll_self(hw);
	u32 reg;

	guard(spinlock)(pll->lock);

	reg = readl(K230_PLLX_GATE_ADDR(pll->reg, pll->id));
	reg &= ~(K230_PLL_GATE_ENABLE);
	reg |= (K230_PLL_GATE_WRITE_ENABLE);
	writel(reg, K230_PLLX_GATE_ADDR(pll->reg, pll->id));
}

static int k230_pll_is_enabled(struct clk_hw *hw)
{
	return k230_pll_hw_is_enabled(hw_to_k230_pll_self(hw));
}

static unsigned long k230_pll_get_rate(struct clk_hw *hw, unsigned long parent_rate)
{
	struct k230_pll_self *pll = hw_to_k230_pll_self(hw);
	u32 reg;
	u32 r, f, od;

	guard(spinlock)(pll->lock);

	reg = readl(K230_PLLX_BYPASS_ADDR(pll->reg, pll->id));
	if (reg & K230_PLL_BYPASS_ENABLE)
		return parent_rate;

	reg = readl(K230_PLLX_LOCK_ADDR(pll->reg, pll->id));
	if (!(reg & (K230_PLL_LOCK_STATUS_MASK)))
		return 0;

	reg = readl(K230_PLLX_DIV_ADDR(pll->reg, pll->id));
	r = ((reg >> K230_PLL_R_SHIFT) & K230_PLL_R_MASK) + 1;
	f = ((reg >> K230_PLL_F_SHIFT) & K230_PLL_F_MASK) + 1;
	od = ((reg >> K230_PLL_OD_SHIFT) & K230_PLL_OD_MASK) + 1;

	return mul_u64_u32_div(parent_rate, f, r * od);
}

static int k230_register_plls(struct platform_device *pdev, spinlock_t *lock,
			      void __iomem *reg)
{
	int i, ret;
	struct k230_pll_self *pll;

	for (i = 0; i < K230_PLL_NUM; i++) {
		const char *name;

		pll = &k230_plls[i]->clk;

		name = pll->hw.init->name;
		pll->lock = lock;
		pll->reg = reg;

		ret = devm_clk_hw_register(&pdev->dev, &pll->hw);
		if (ret)
			return ret;

		ret = devm_clk_hw_register_clkdev(&pdev->dev, &pll->hw, name, NULL);
		if (ret)
			return ret;
	}

	return 0;
}

static int k230_register_pll_divs(struct platform_device *pdev)
{
	struct clk_fixed_factor *pll_div;
	int ret;

	for (int i = 0; i < K230_PLL_DIV_NUM; i++) {
		const char *name;

		pll_div = &k230_pll_divs[i]->clk;

		name = pll_div->hw.init->name;

		ret = devm_clk_hw_register(&pdev->dev, &pll_div->hw);
		if (ret)
			return ret;

		ret = devm_clk_hw_register_clkdev(&pdev->dev, &pll_div->hw,
						  name, NULL);
		if (ret)
			return ret;
	}

	return 0;
}

static unsigned long k230_clk_get_rate_mul(struct clk_hw *hw,
					   unsigned long parent_rate)
{
	struct k230_clk_rate *clk = hw_to_k230_clk_rate(hw);
	struct k230_clk_rate_self *rate_self = &clk->clk;
	u32 mul = 1, div;

	guard(spinlock)(rate_self->lock);

	div = rate_self->div_max;
	mul += (readl(rate_self->reg + clk->reg_off) >> rate_self->div_shift)
		& rate_self->div_mask;

	return mul_u64_u32_div(parent_rate, mul, div);
}

static unsigned long k230_clk_get_rate_div(struct clk_hw *hw,
					   unsigned long parent_rate)
{
	struct k230_clk_rate *clk = hw_to_k230_clk_rate(hw);
	struct k230_clk_rate_self *rate_self = &clk->clk;
	u32 mul, div = 1;

	guard(spinlock)(rate_self->lock);

	mul = rate_self->mul_max;
	div += (readl(rate_self->reg + clk->reg_off) >> rate_self->div_shift)
		& rate_self->div_mask;

	return mul_u64_u32_div(parent_rate, mul, div);
}

static unsigned long k230_clk_get_rate_mul_div(struct clk_hw *hw,
					       unsigned long parent_rate)
{
	struct k230_clk_rate *clk = hw_to_k230_clk_rate(hw);
	struct k230_clk_rate_self *rate_self = &clk->clk;
	u32 mul, div, reg_off, reg_off2;

	guard(spinlock)(rate_self->lock);

	reg_off = clk->reg_off;
	reg_off2 = clk->reg_off2 ? clk->reg_off2 : reg_off;

	mul = (readl(rate_self->reg + reg_off2) >> rate_self->mul_shift)
		& rate_self->mul_mask;

	div = (readl(rate_self->reg + reg_off) >> rate_self->div_shift)
		& rate_self->div_mask;

	return mul_u64_u32_div(parent_rate, mul, div);
}

static int k230_clk_find_approximate_mul(u32 mul_min, u32 mul_max,
					 u32 div_min, u32 div_max,
					 unsigned long rate, unsigned long parent_rate,
					 u32 *div, u32 *mul)
{
	long abs_min;
	long abs_current;
	long perfect_divide;

	if (!rate || !parent_rate || !mul_min || !mul_max)
		return -EINVAL;

	perfect_divide = (long)((parent_rate * 1000) / rate);
	abs_min = abs(perfect_divide -
		     (long)(((long)div_max * 1000) / (long)mul_min));
	*mul = mul_min;

	for (u32 i = mul_min + 1; i <= mul_max; i++) {
		abs_current = abs(perfect_divide -
				(long)((long)((long)div_max * 1000) / (long)i));
		if (abs_min > abs_current) {
			abs_min = abs_current;
			*mul = i;
		}
	}

	*div = div_max;

	return 0;
}

static int k230_clk_find_approximate_div(u32 mul_min, u32 mul_max,
					 u32 div_min, u32 div_max,
					 unsigned long rate, unsigned long parent_rate,
					 u32 *div, u32 *mul)
{
	long abs_min;
	long abs_current;
	long perfect_divide;

	if (!rate || !parent_rate || !mul_min || !mul_max)
		return -EINVAL;

	perfect_divide = (long)((parent_rate * 1000) / rate);
	abs_min = abs(perfect_divide -
		     (long)(((long)div_min * 1000) / (long)mul_max));
	*div = div_min;

	for (u32 i = div_min + 1; i <= div_max; i++) {
		abs_current = abs(perfect_divide -
				 (long)((long)((long)i * 1000) / (long)mul_max));
		if (abs_min > abs_current) {
			abs_min = abs_current;
			*div = i;
		}
	}

	*mul = mul_max;

	return 0;
}

static int k230_clk_find_approximate_mul_div(struct k230_clk_rate *clk,
					     u32 mul_min, u32 mul_max,
					     u32 div_min, u32 div_max,
					     unsigned long rate,
					     unsigned long parent_rate,
					     u32 *div, u32 *mul)
{
	const u32 codec_clk[9] = {
		2048000,
		3072000,
		4096000,
		6144000,
		8192000,
		11289600,
		12288000,
		24576000,
		49152000
	};

	const u32 codec_div[9][2] = {
		{3125, 16},
		{3125, 24},
		{3125, 32},
		{3125, 48},
		{3125, 64},
		{15625, 441},
		{3125, 96},
		{3125, 192},
		{3125, 384}
	};

	const u32 pdm_clk[20] = {
		128000,
		192000,
		256000,
		384000,
		512000,
		768000,
		1024000,
		1411200,
		1536000,
		2048000,
		2822400,
		3072000,
		4096000,
		5644800,
		6144000,
		8192000,
		11289600,
		12288000,
		24576000,
		49152000
	};

	const u32 pdm_div[20][2] = {
		{3125, 1},
		{6250, 3},
		{3125, 2},
		{3125, 3},
		{3125, 4},
		{3125, 6},
		{3125, 8},
		{125000, 441},
		{3125, 12},
		{3125, 16},
		{62500, 441},
		{3125, 24},
		{3125, 32},
		{31250, 441},
		{3125, 48},
		{3125, 64},
		{15625, 441},
		{3125, 96},
		{3125, 192},
		{3125, 384}
	};

	if (!rate || !parent_rate || !mul_min || !mul_max)
		return -EINVAL;

	if (clk->reg_off == K230_CLK_CODEC_ADC_MCLKDIV_OFFSET ||
	    clk->reg_off == K230_CLK_CODEC_DAC_MCLKDIV_OFFSET) {
		for (int i = 0; i < 9; i++) {
			if (rate == codec_clk[i]) {
				*div = codec_div[i][0];
				*mul = codec_div[i][1];
			}
		}
	} else if (clk->reg_off == K230_CLK_AUDIO_CLKDIV_OFFSET ||
		   clk->reg_off == K230_CLK_PDM_CLKDIV_OFFSET) {
		for (int i = 0; i < 20; i++) {
			if (rate == pdm_clk[i]) {
				*div = pdm_div[i][0];
				*mul = pdm_div[i][1];
			}
		}
	} else {
		return -EINVAL;
	}

	return 0;
}

static long k230_clk_round_rate_mul(struct clk_hw *hw, unsigned long rate,
				    unsigned long *parent_rate)
{
	struct k230_clk_rate_self *rate_self = hw_to_k230_clk_rate_self(hw);
	u32 div, mul;

	if (k230_clk_find_approximate_mul(rate_self->mul_min, rate_self->mul_max,
					  rate_self->div_min, rate_self->div_max,
					  rate, *parent_rate, &div, &mul))
		return 0;

	return mul_u64_u32_div(*parent_rate, mul, div);
}

static long k230_clk_round_rate_div(struct clk_hw *hw, unsigned long rate,
				    unsigned long *parent_rate)
{
	struct k230_clk_rate_self *rate_self = hw_to_k230_clk_rate_self(hw);
	u32 div, mul;

	if (k230_clk_find_approximate_div(rate_self->mul_min, rate_self->mul_max,
					  rate_self->div_min, rate_self->div_max,
					  rate, *parent_rate, &div, &mul))
		return 0;

	return mul_u64_u32_div(*parent_rate, mul, div);
}

static long k230_clk_round_rate_mul_div(struct clk_hw *hw, unsigned long rate,
					unsigned long *parent_rate)
{
	struct k230_clk_rate *clk = hw_to_k230_clk_rate(hw);
	struct k230_clk_rate_self *rate_self = &clk->clk;
	u32 div, mul;

	if (k230_clk_find_approximate_mul_div(clk,
					      rate_self->mul_min, rate_self->mul_max,
					      rate_self->div_min, rate_self->div_max,
					      rate, *parent_rate, &div, &mul))
		return 0;

	return mul_u64_u32_div(*parent_rate, mul, div);
}

static int k230_clk_set_rate_mul(struct clk_hw *hw, unsigned long rate,
				 unsigned long parent_rate)
{
	struct k230_clk_rate *clk = hw_to_k230_clk_rate(hw);
	struct k230_clk_rate_self *rate_self = &clk->clk;
	u32 div, mul, reg;

	if (rate > parent_rate)
		return -EINVAL;

	if (rate_self->read_only)
		return 0;

	if (k230_clk_find_approximate_mul(rate_self->mul_min, rate_self->mul_max,
					  rate_self->div_min, rate_self->div_max,
					  rate, parent_rate, &div, &mul))
		return -EINVAL;

	guard(spinlock)(rate_self->lock);

	reg = readl(rate_self->reg + clk->reg_off);
	reg &= ~((rate_self->div_mask) << (rate_self->div_shift));
	reg |= ((mul - 1) & rate_self->div_mask) << (rate_self->div_shift);
	reg |= BIT(rate_self->write_enable_bit);
	writel(reg, rate_self->reg + clk->reg_off);

	return 0;
}

static int k230_clk_set_rate_div(struct clk_hw *hw, unsigned long rate,
				 unsigned long parent_rate)
{
	struct k230_clk_rate *clk = hw_to_k230_clk_rate(hw);
	struct k230_clk_rate_self *rate_self = &clk->clk;
	u32 div, mul, reg;

	if (rate > parent_rate)
		return -EINVAL;

	if (rate_self->read_only)
		return 0;

	if (k230_clk_find_approximate_div(rate_self->mul_min, rate_self->mul_max,
					  rate_self->div_min, rate_self->div_max,
					  rate, parent_rate, &div, &mul))
		return -EINVAL;

	guard(spinlock)(rate_self->lock);

	reg = readl(rate_self->reg + clk->reg_off);
	reg &= ~((rate_self->div_mask) << (rate_self->div_shift));
	reg &= ~((rate_self->mul_mask) << (rate_self->mul_shift));
	reg |= ((div - 1) & rate_self->div_mask) << (rate_self->div_shift);
	reg |= BIT(rate_self->write_enable_bit);
	writel(reg, rate_self->reg + clk->reg_off);

	return 0;
}

static int k230_clk_set_rate_mul_div(struct clk_hw *hw, unsigned long rate,
				     unsigned long parent_rate)
{
	struct k230_clk_rate *clk = hw_to_k230_clk_rate(hw);
	struct k230_clk_rate_self *rate_self = &clk->clk;
	u32 div, mul, reg, reg_c;

	if (rate > parent_rate)
		return -EINVAL;

	if (rate_self->read_only)
		return 0;

	if (k230_clk_find_approximate_mul_div(clk,
					      rate_self->mul_min, rate_self->mul_max,
					      rate_self->div_min, rate_self->div_max,
					      rate, parent_rate, &div, &mul))
		return -EINVAL;

	guard(spinlock)(rate_self->lock);

	reg = readl(rate_self->reg + clk->reg_off);
	reg &= ~((rate_self->div_mask) << (rate_self->div_shift));

	if (!clk->reg_off2) {
		reg |= (mul & rate_self->mul_mask) << (rate_self->mul_shift);
		reg |= (div & rate_self->div_mask) << (rate_self->div_shift);
		reg |= BIT(rate_self->write_enable_bit);
	} else {
		reg_c = readl(rate_self->reg + clk->reg_off2);
		reg_c &= ~((rate_self->mul_mask) << (rate_self->mul_shift));
		reg_c |= (mul & rate_self->mul_mask) << (rate_self->mul_shift);
		reg_c |= BIT(rate_self->write_enable_bit);
		writel(reg_c, rate_self->reg + clk->reg_off2);
	}

	reg |= (div & rate_self->div_mask) << (rate_self->div_shift);
	writel(reg, rate_self->reg + clk->reg_off);

	return 0;
}

static inline int k230_register_clk_mux(int id, struct k230_clk_mux *clk,
					struct device *dev,
					struct clk_hw_onecell_data *hw_data,
					spinlock_t *lock, void __iomem *reg)
{
	int ret;
	struct clk_hw *hw = &clk->clk.hw;

	clk->clk.lock = lock;
	clk->clk.reg = reg;

	ret = devm_clk_hw_register(dev, hw);
	if (ret)
		return ret;

	hw_data->hws[id] = hw;

	return 0;
}

static inline int k230_register_clk_gate(int id, struct k230_clk_gate *clk,
					 struct device *dev,
					 struct clk_hw_onecell_data *hw_data,
					 spinlock_t *lock, void __iomem *reg)
{
	int ret;
	struct clk_hw *hw = &clk->clk.hw;

	clk->clk.lock = lock;
	clk->clk.reg = reg;

	ret = devm_clk_hw_register(dev, hw);
	if (ret)
		return ret;

	hw_data->hws[id] = hw;

	return 0;
}

static inline int k230_register_clk_rate(int id, struct k230_clk_rate *clk,
					 struct device *dev,
					 struct clk_hw_onecell_data *hw_data,
					 spinlock_t *lock, void __iomem *reg)
{
	int ret;
	struct clk_hw *hw = &clk->clk.hw;

	clk->clk.lock = lock;
	clk->clk.reg = reg;

	ret = devm_clk_hw_register(dev, hw);
	if (ret)
		return ret;

	hw_data->hws[id] = hw;

	return 0;
}

static inline int k230_register_clk_fixed_factor(int id,
						 struct k230_clk_fixed_factor *clk,
						 struct device *dev,
						 struct clk_hw_onecell_data *hw_data)
{
	int ret;
	struct clk_hw *hw = &clk->clk.hw;

	ret = devm_clk_hw_register(dev, hw);
	if (ret)
		return ret;

	hw_data->hws[id] = hw;

	return 0;
}

static inline int k230_register_clk_fixed_rate(int id, struct k230_clk_fixed_rate *clk,
					       struct device *dev,
					       struct clk_hw_onecell_data *hw_data)
{
	int ret;
	struct clk_hw *hw = &clk->clk.hw;

	ret = devm_clk_hw_register(dev, hw);
	if (ret)
		return ret;

	hw_data->hws[id] = hw;

	return 0;
}

static int k230_register_clks(struct platform_device *pdev,
			      struct clk_hw_onecell_data *hw_data,
			      spinlock_t *lock, void __iomem *reg)
{
	int ret;
	struct device *dev = &pdev->dev;

	ret = k230_register_clk_gate(K230_CPU0_SRC_GATE, K230_FMT(cpu0_src_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_CPU0_SRC_RATE, K230_FMT(cpu0_src_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_CPU0_AXI_RATE, K230_FMT(cpu0_axi_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_CPU0_PLIC_GATE, K230_FMT(cpu0_plic_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_CPU0_PLIC_RATE, K230_FMT(cpu0_plic_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_CPU0_NOC_DDRCP4_GATE, K230_FMT(cpu0_noc_ddrcp4_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_CPU0_APB_GATE, K230_FMT(cpu0_apb_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_CPU0_APB_RATE, K230_FMT(cpu0_apb_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_mux(K230_CPU1_SRC_MUX, K230_FMT(cpu1_src_mux),
				    dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_CPU1_SRC_GATE, K230_FMT(cpu1_src_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_CPU1_SRC_RATE, K230_FMT(cpu1_src_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_CPU1_AXI_RATE, K230_FMT(cpu1_axi_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_CPU1_PLIC_GATE, K230_FMT(cpu1_plic_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_CPU1_PLIC_RATE, K230_FMT(cpu1_plic_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_CPU1_APB_GATE, K230_FMT(cpu1_apb_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_CPU1_APB_RATE, K230_FMT(cpu1_apb_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_PMU_APB_GATE, K230_FMT(pmu_apb_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_HS_HCLK_HIGH_SRC_RATE, K230_FMT(hs_hclk_high_src_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_HS_HCLK_HIGH_GATE, K230_FMT(hs_hclk_high_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_HS_HCLK_SRC_GATE, K230_FMT(hs_hclk_src_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_HS_HCLK_SRC_RATE, K230_FMT(hs_hclk_src_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_HS_SD0_AHB_GATE, K230_FMT(hs_sd0_ahb_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_HS_SD1_AHB_GATE, K230_FMT(hs_sd1_ahb_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_HS_SSI1_AHB_GATE, K230_FMT(hs_ssi1_ahb_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_HS_SSI2_AHB_GATE, K230_FMT(hs_ssi2_ahb_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_HS_USB0_AHB_GATE, K230_FMT(hs_usb0_ahb_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_HS_USB1_AHB_GATE, K230_FMT(hs_usb1_ahb_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_HS_SSI0_AXI_GATE, K230_FMT(hs_ssi0_axi_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_HS_SSI0_AXI_RATE, K230_FMT(hs_ssi0_axi_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_HS_SSI1_GATE, K230_FMT(hs_ssi1_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_HS_SSI1_RATE, K230_FMT(hs_ssi1_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_HS_SSI2_GATE, K230_FMT(hs_ssi2_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_HS_SSI2_RATE, K230_FMT(hs_ssi2_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_HS_QSPI_AXI_SRC_GATE, K230_FMT(hs_qspi_axi_src_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_HS_QSPI_AXI_SRC_RATE, K230_FMT(hs_qspi_axi_src_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_HS_SSI1_AXI_GATE, K230_FMT(hs_ssi1_axi_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_HS_SSI2_AXI_GATE, K230_FMT(hs_ssi2_axi_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_HS_SD_CARD_SRC_GATE, K230_FMT(hs_sd_card_src_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_HS_SD_CARD_SRC_RATE, K230_FMT(hs_sd_card_src_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_HS_SD0_CARD_GATE, K230_FMT(hs_sd0_card_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_HS_SD1_CARD_GATE, K230_FMT(hs_sd1_card_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_HS_SD_AXI_SRC_GATE, K230_FMT(hs_sd_axi_src_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_HS_SD_AXI_SRC_RATE, K230_FMT(hs_sd_axi_src_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_HS_SD0_AXI_GATE, K230_FMT(hs_sd0_axi_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_HS_SD1_AXI_GATE, K230_FMT(hs_sd1_axi_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_HS_SD0_BASE_GATE, K230_FMT(hs_sd0_base_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_HS_SD1_BASE_GATE, K230_FMT(hs_sd1_base_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_mux(K230_HS_OSPI_SRC_MUX, K230_FMT(hs_ospi_src_mux),
				    dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_HS_OSPI_SRC_GATE, K230_FMT(hs_ospi_src_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_HS_USB_REF_50M_RATE, K230_FMT(hs_usb_ref_50m_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_HS_SD_TIMER_SRC_GATE, K230_FMT(hs_sd_timer_src_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_HS_SD_TIMER_SRC_RATE, K230_FMT(hs_sd_timer_src_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_HS_SD0_TIMER_GATE, K230_FMT(hs_sd0_timer_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_HS_SD1_TIMER_GATE, K230_FMT(hs_sd1_timer_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_mux(K230_HS_USB0_REF_MUX, K230_FMT(hs_usb0_ref_mux),
				    dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_HS_USB0_REF_GATE, K230_FMT(hs_usb0_ref_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_mux(K230_HS_USB1_REF_MUX, K230_FMT(hs_usb1_ref_mux),
				    dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_HS_USB1_REF_GATE, K230_FMT(hs_usb1_ref_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_LS_APB_SRC_GATE, K230_FMT(ls_apb_src_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_LS_APB_SRC_RATE, K230_FMT(ls_apb_src_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_LS_UART0_APB_GATE, K230_FMT(ls_uart0_apb_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_LS_UART1_APB_GATE, K230_FMT(ls_uart1_apb_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_LS_UART2_APB_GATE, K230_FMT(ls_uart2_apb_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_LS_UART3_APB_GATE, K230_FMT(ls_uart3_apb_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_LS_UART4_APB_GATE, K230_FMT(ls_uart4_apb_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_LS_I2C0_APB_GATE, K230_FMT(ls_i2c0_apb_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_LS_I2C1_APB_GATE, K230_FMT(ls_i2c1_apb_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_LS_I2C2_APB_GATE, K230_FMT(ls_i2c2_apb_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_LS_I2C3_APB_GATE, K230_FMT(ls_i2c3_apb_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_LS_I2C4_APB_GATE, K230_FMT(ls_i2c4_apb_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_LS_GPIO_APB_GATE, K230_FMT(ls_gpio_apb_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_LS_PWM_APB_GATE, K230_FMT(ls_pwm_apb_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_LS_JAMLINK0_APB_GATE, K230_FMT(ls_jamlink0_apb_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_LS_JAMLINK1_APB_GATE, K230_FMT(ls_jamlink1_apb_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_LS_JAMLINK2_APB_GATE, K230_FMT(ls_jamlink2_apb_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_LS_JAMLINK3_APB_GATE, K230_FMT(ls_jamlink3_apb_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_LS_AUDIO_APB_GATE, K230_FMT(ls_audio_apb_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_LS_ADC_APB_GATE, K230_FMT(ls_adc_apb_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_LS_CODEC_APB_GATE, K230_FMT(ls_codec_apb_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_LS_I2C0_GATE, K230_FMT(ls_i2c0_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_LS_I2C0_RATE, K230_FMT(ls_i2c0_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_LS_I2C1_GATE, K230_FMT(ls_i2c1_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_LS_I2C1_RATE, K230_FMT(ls_i2c1_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_LS_I2C2_GATE, K230_FMT(ls_i2c2_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_LS_I2C2_RATE, K230_FMT(ls_i2c2_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_LS_I2C3_GATE, K230_FMT(ls_i2c3_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_LS_I2C3_RATE, K230_FMT(ls_i2c3_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_LS_I2C4_GATE, K230_FMT(ls_i2c4_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_LS_I2C4_RATE, K230_FMT(ls_i2c4_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_LS_CODEC_ADC_GATE, K230_FMT(ls_codec_adc_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_LS_CODEC_ADC_RATE, K230_FMT(ls_codec_adc_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_LS_CODEC_DAC_GATE, K230_FMT(ls_codec_dac_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_LS_CODEC_DAC_RATE, K230_FMT(ls_codec_dac_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_LS_AUDIO_DEV_GATE, K230_FMT(ls_audio_dev_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_LS_AUDIO_DEV_RATE, K230_FMT(ls_audio_dev_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_LS_PDM_GATE, K230_FMT(ls_pdm_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_LS_PDM_RATE, K230_FMT(ls_pdm_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_LS_ADC_GATE, K230_FMT(ls_adc_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_LS_ADC_RATE, K230_FMT(ls_adc_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_LS_UART0_GATE, K230_FMT(ls_uart0_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_LS_UART0_RATE, K230_FMT(ls_uart0_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_LS_UART1_GATE, K230_FMT(ls_uart1_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_LS_UART1_RATE, K230_FMT(ls_uart1_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_LS_UART2_GATE, K230_FMT(ls_uart2_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_LS_UART2_RATE, K230_FMT(ls_uart2_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_LS_UART3_GATE, K230_FMT(ls_uart3_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_LS_UART3_RATE, K230_FMT(ls_uart3_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_LS_UART4_GATE, K230_FMT(ls_uart4_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_LS_UART4_RATE, K230_FMT(ls_uart4_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_LS_JAMLINKCO_SRC_RATE, K230_FMT(ls_jamlinkco_src_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_LS_JAMLINK0CO_GATE, K230_FMT(ls_jamlink0co_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_LS_JAMLINK1CO_GATE, K230_FMT(ls_jamlink1co_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_LS_JAMLINK2CO_GATE, K230_FMT(ls_jamlink2co_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_LS_JAMLINK3CO_GATE, K230_FMT(ls_jamlink3co_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_LS_GPIO_DEBOUNCE_GATE, K230_FMT(ls_gpio_debounce_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_LS_GPIO_DEBOUNCE_RATE, K230_FMT(ls_gpio_debounce_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_fixed_rate(K230_SYSCTL_APB_SRC, K230_FMT(sysctl_apb_src),
					   dev, hw_data);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_SYSCTL_WDT0_APB_GATE, K230_FMT(sysctl_wdt0_apb_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_SYSCTL_WDT1_APB_GATE, K230_FMT(sysctl_wdt1_apb_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_SYSCTL_TIMER_APB_GATE, K230_FMT(sysctl_timer_apb_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_SYSCTL_IOMUX_APB_GATE,
				     K230_FMT(sysctl_iomux_apb_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_SYSCTL_MAILBOX_APB_GATE,
				     K230_FMT(sysctl_mailbox_apb_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_SYSCTL_HDI_GATE, K230_FMT(sysctl_hdi_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_SYSCTL_HDI_RATE, K230_FMT(sysctl_hdi_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_SYSCTL_TIME_STAMP_GATE,
				     K230_FMT(sysctl_time_stamp_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_SYSCTL_TIME_STAMP_RATE,
				     K230_FMT(sysctl_time_stamp_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_SYSCTL_TEMP_SENSOR_RATE,
				     K230_FMT(sysctl_temp_sensor_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_SYSCTL_WDT0_GATE, K230_FMT(sysctl_wdt0_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_SYSCTL_WDT0_RATE, K230_FMT(sysctl_wdt0_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_SYSCTL_WDT1_GATE, K230_FMT(sysctl_wdt1_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_SYSCTL_WDT1_RATE, K230_FMT(sysctl_wdt1_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_TIMER0_SRC_RATE, K230_FMT(timer0_src_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_TIMER1_SRC_RATE, K230_FMT(timer1_src_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_TIMER2_SRC_RATE, K230_FMT(timer2_src_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_TIMER3_SRC_RATE, K230_FMT(timer3_src_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_TIMER4_SRC_RATE, K230_FMT(timer4_src_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_TIMER5_SRC_RATE, K230_FMT(timer5_src_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_mux(K230_TIMER0_MUX, K230_FMT(timer0_mux),
				    dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_TIMER0_GATE, K230_FMT(timer0_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_mux(K230_TIMER1_MUX, K230_FMT(timer1_mux),
				    dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_TIMER1_GATE, K230_FMT(timer1_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_mux(K230_TIMER2_MUX, K230_FMT(timer2_mux),
				    dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_TIMER2_GATE, K230_FMT(timer2_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_mux(K230_TIMER3_MUX, K230_FMT(timer3_mux),
				    dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_TIMER3_GATE, K230_FMT(timer3_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_mux(K230_TIMER4_MUX, K230_FMT(timer4_mux),
				    dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_TIMER4_GATE, K230_FMT(timer4_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_mux(K230_TIMER5_MUX, K230_FMT(timer5_mux),
				    dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_TIMER5_GATE, K230_FMT(timer5_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_SHRM_APB_GATE, K230_FMT(shrm_apb_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_SHRM_APB_RATE, K230_FMT(shrm_apb_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_mux(K230_SHRM_SRAM_MUX, K230_FMT(shrm_sram_mux),
				    dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_SHRM_SRAM_GATE, K230_FMT(shrm_sram_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_fixed_factor(K230_SHRM_SRAM_DIV2, K230_FMT(shrm_sram_div2),
					     dev, hw_data);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_SHRM_AXI_GATE, K230_FMT(shrm_axi_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_SHRM_AXI_SLAVE_GATE, K230_FMT(shrm_axi_slave_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_SHRM_NONAI2D_AXI_GATE,
				     K230_FMT(shrm_nonai2d_axi_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_SHRM_DECOMPRESS_AXI_GATE,
				     K230_FMT(shrm_decompress_axi_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_SHRM_SDMA_AXI_GATE,
				     K230_FMT(shrm_sdma_axi_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_SHRM_PDMA_AXI_GATE, K230_FMT(shrm_pdma_axi_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_mux(K230_DDRC_SRC_MUX, K230_FMT(ddrc_src_mux),
				    dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_DDRC_SRC_GATE, K230_FMT(ddrc_src_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_DDRC_SRC_RATE, K230_FMT(ddrc_src_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_DDRC_BYPASS_GATE, K230_FMT(ddrc_bypass_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_DDRC_APB_GATE, K230_FMT(ddrc_apb_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_DDRC_APB_RATE, K230_FMT(ddrc_apb_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_DISPLAY_AHB_GATE, K230_FMT(display_ahb_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_DISPLAY_AHB_RATE, K230_FMT(display_ahb_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_DISPLAY_AXI_GATE, K230_FMT(display_axi_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_DISPLAY_CLKEXT_RATE, K230_FMT(display_clkext_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_DISPLAY_GPU_GATE, K230_FMT(display_gpu_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_DISPLAY_GPU_RATE, K230_FMT(display_gpu_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_DISPLAY_DPIP_GATE, K230_FMT(display_dpip_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_DISPLAY_DPIP_RATE, K230_FMT(display_dpip_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_DISPLAY_CFG_GATE, K230_FMT(display_cfg_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_DISPLAY_CFG_RATE, K230_FMT(display_cfg_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_DISPLAY_REF_GATE, K230_FMT(display_ref_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_VPU_SRC_GATE, K230_FMT(vpu_src_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_VPU_SRC_RATE, K230_FMT(vpu_src_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_VPU_AXI_SRC_RATE, K230_FMT(vpu_axi_src_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_VPU_AXI_GATE, K230_FMT(vpu_axi_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_VPU_DDRCP2_GATE, K230_FMT(vpu_ddrcp2_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_VPU_CFG_GATE, K230_FMT(vpu_cfg_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_VPU_CFG_RATE, K230_FMT(vpu_cfg_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_SEC_APB_GATE, K230_FMT(sec_apb_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_SEC_APB_RATE, K230_FMT(sec_apb_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_SEC_FIX_GATE, K230_FMT(sec_fix_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_SEC_FIX_RATE, K230_FMT(sec_fix_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_SEC_AXI_GATE, K230_FMT(sec_axi_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_SEC_AXI_RATE, K230_FMT(sec_axi_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_USB_480M_GATE, K230_FMT(usb_480m_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_USB_480M_RATE, K230_FMT(usb_480m_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_USB_100M_GATE, K230_FMT(usb_100m_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_USB_100M_RATE, K230_FMT(usb_100m_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_DPHY_DFT_GATE, K230_FMT(dphy_dft_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_DPHY_DFT_RATE, K230_FMT(dphy_dft_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_SPI2AXI_GATE, K230_FMT(spi2axi_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_SPI2AXI_RATE, K230_FMT(spi2axi_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_mux(K230_AI_SRC_MUX, K230_FMT(ai_src_mux),
				    dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_AI_SRC_GATE, K230_FMT(ai_src_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_AI_SRC_RATE, K230_FMT(ai_src_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_AI_AXI_GATE, K230_FMT(ai_axi_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_mux(K230_CAMERA0_MUX, K230_FMT(camera0_mux),
				    dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_CAMERA0_GATE, K230_FMT(camera0_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_CAMERA0_RATE, K230_FMT(camera0_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_mux(K230_CAMERA1_MUX, K230_FMT(camera1_mux),
				    dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_CAMERA1_GATE, K230_FMT(camera1_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_CAMERA1_RATE, K230_FMT(camera1_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_mux(K230_CAMERA2_MUX, K230_FMT(camera2_mux),
				    dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_gate(K230_CAMERA2_GATE, K230_FMT(camera2_gate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_clk_rate(K230_CAMERA2_RATE, K230_FMT(camera2_rate),
				     dev, hw_data, lock, reg);
	if (ret)
		return ret;

	return devm_of_clk_add_hw_provider(&pdev->dev, of_clk_hw_onecell_get, hw_data);
}

static int k230_clk_init_plls(struct platform_device *pdev)
{
	int ret;
	void __iomem *reg;
	/* used for all the plls */
	spinlock_t *lock;

	lock = devm_kzalloc(&pdev->dev, sizeof(*lock), GFP_KERNEL);
	if (!lock)
		return -ENOMEM;

	spin_lock_init(lock);

	reg = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(reg))
		return PTR_ERR(reg);

	ret = k230_register_plls(pdev, lock, reg);
	if (ret)
		return ret;

	ret = k230_register_pll_divs(pdev);
	if (ret)
		return ret;

	return 0;
}

static int k230_clk_init_clks(struct platform_device *pdev,
			      struct clk_hw_onecell_data *hw_data)
{
	int ret;
	void __iomem *reg;
	/* used for all the clocks */
	spinlock_t *lock;

	lock = devm_kzalloc(&pdev->dev, sizeof(*lock), GFP_KERNEL);
	if (!lock)
		return -ENOMEM;

	spin_lock_init(lock);

	hw_data->num = K230_CLK_NUM;

	reg = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(reg))
		return PTR_ERR(reg);

	ret = k230_register_clks(pdev, hw_data, lock, reg);
	if (ret)
		return ret;

	return 0;
}

static int k230_clk_probe(struct platform_device *pdev)
{
	int ret;
	struct clk_hw_onecell_data *hw_data;

	hw_data = devm_kzalloc(&pdev->dev, struct_size(hw_data, hws, K230_CLK_NUM),
			       GFP_KERNEL);
	if (!hw_data)
		return -ENOMEM;

	ret = k230_clk_init_plls(pdev);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "init plls failed\n");

	ret = k230_clk_init_clks(pdev, hw_data);
	if (ret)
		return dev_err_probe(&pdev->dev, ret, "init clks failed\n");

	return 0;
}

static const struct of_device_id k230_clk_ids[] = {
	{ .compatible = "canaan,k230-clk" },
	{ /* Sentinel */ }
};
MODULE_DEVICE_TABLE(of, k230_clk_ids);

static struct platform_driver k230_clk_driver = {
	.driver = {
		.name = "k230_clock_controller",
		.of_match_table = k230_clk_ids,
	},
	.probe = k230_clk_probe,
};
builtin_platform_driver(k230_clk_driver);

