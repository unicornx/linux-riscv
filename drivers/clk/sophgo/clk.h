#ifndef __SOPHGO_CLOCK__
#define __SOPHGO_CLOCK__

#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/device.h>
#include <linux/clkdev.h>

struct mango_pll_ctrl {
	unsigned long freq;
	unsigned int fbdiv;
	unsigned int postdiv1;
	unsigned int postdiv2;
	unsigned int refdiv;
};

/*
 * clock common data
 * @iobase & @syscon: point to the same address (top of syscon), the reason
 *  we use two different type of pointer just due to pll uses regmap while
 *  others use iomem.
 * @lock: clock register access lock
 * @onecell_data: used for adding provider
 */
struct mango_clk_data {
       void __iomem *iobase;
       struct regmap *syscon;
       spinlock_t lock;
       struct clk_hw_onecell_data onecell_data;
};

/*
 * PLL clock
 * @id:				used to map clk_onecell_data
 * @name:			used for print even when clk registeration failed
 * @map:			used for regmap read/write, regmap is more useful
 * 				then iomem address when we have multiple offsets
 * 				for different registers
 * @lock:			spinlock to protect register access
 * @offset_status:		offset of pll status registers
 * @offset_enable:		offset of pll enable registers
 * @offset_ctrl:		offset of pll control registers
 * @shift_status_lock:		shift of XXX_LOCK in pll status regsiter
 * @shift_status_updating:	shift of UPDATING_XXX in pll status register
 * @shift_enable:		shift of XXX_CLK_EN in pll enable register
 */
struct mango_pll_clock {
	struct clk_hw	hw;

	/* private data */
	unsigned int id;
	const char *name;

	struct regmap *map;
	spinlock_t *lock;

	u32 offset_status;
	u32 offset_enable;
	u32 offset_ctrl;
	u8 shift_status_lock;
	u8 shift_status_updating;
	u8 shift_enable;
};

#define to_mango_pll_clk(_hw) container_of(_hw, struct mango_pll_clock, hw)

/*
 * Divider clock
 * @id:			used to map clk_onecell_data
 * @name:		used for print even when clk registeration failed
 * @reg:		used for readl/writel.
 * @lock:		spinlock to protect register access
 * @offset_ctrl:	offset of divider control registers
 * @shift:		shift of "Clock Divider Factor" in divider control register
 * @width:		width of "Clock Divider Factor" in divider control register
 * @div_flags:		private flags for this clock, not for framework-specific
 * @initial_val:	initial value of the divider, a value < 0 means ignoring
 * 			setting of initial value.
 * @table:		the div table that the divider supports
 */
struct mango_divider_clock {
	struct clk_hw	hw;

	/* private data */
	unsigned int id;
	const char *name;

	void __iomem *reg;
	spinlock_t *lock;

	unsigned long offset_ctrl;
	u8 shift;
	u8 width;
	u8 div_flags;
	s32 initial_val;
	struct clk_div_table *table;
};

#define to_mango_clk_divider(_hw)	\
	container_of(_hw, struct mango_divider_clock, hw)

/*
 * Gate clock
 * @id:			used to map clk_onecell_data
 * @name:		string of this clock name
 * @parent_name:	string of parent clock name
 * @flags:		framework-specific flags for this clock
 * @offset_enable:	offset of gate enable registers
 * @bit_idx:		which bit in the register controls gating of this clock
 */
struct mango_gate_clock {
	unsigned int id;
	const char *name;
	const char *parent_name;
	unsigned long flags;
	unsigned long offset_enable;
	u8 bit_idx;
};

/*
 * Gate clock
 * @id:			used to map clk_onecell_data
 * @name:		string of this clock name
 * @parent_name:	string array of parents' clock name
 * @flags:		framework-specific flags for this clock
 * @offset_select:	offset of mux selection registers
 * @shift:		shift of "Clock Select" in mux selection register
 * @width:		width of "Clock Select" in mux selection register
 * @clk_nb:		used for notification
 */
struct mango_mux_clock {
	unsigned int id;
	const char *name;
	const char * const *parent_names;
	u8 num_parents;
	unsigned long flags;
	unsigned long offset_select;
	u8 shift;
	u8 width;
	struct notifier_block clk_nb;
};


#define DEBUG
#ifdef DEBUG
	#define dbg_info(format, arg...) do {\
		pr_info("--> %s: "format"" , __func__ , ## arg);\
	} while (0)
#else
	#define dbg_info(format, arg...)
#endif

#endif
