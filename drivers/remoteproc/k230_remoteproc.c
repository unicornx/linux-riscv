
// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2025 Junhui Liu <junhui.liu@pigmoral.tech>
 */

#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_reserved_mem.h>
#include <linux/regmap.h>
#include <linux/remoteproc.h>
#include <linux/reset.h>
#include <linux/platform_device.h>

#include "remoteproc_internal.h"


#if 1
// small core
#define RSTVEC_ADDR	((__u32)0x91102100)
#define RSTCTL_ADDR	((__u32)0x91101004)
#else
// big core
#define RSTVEC_ADDR	((__u32)0x91102104)
#define RSTCTL_ADDR	((__u32)0x9110100c)
#endif

struct k230_rproc {
	struct device		*dev;
	struct reset_control	*rst;
	struct rproc		*rproc;
};

static int k230_rproc_mem_alloc(struct rproc *rproc,
				 struct rproc_mem_entry *mem)
{
	struct device *dev = rproc->dev.parent;
	void *va;

	dev_dbg(dev, "map memory: %pad+%zx\n", &mem->dma, mem->len);
	va = (__force void *)ioremap_wc(mem->dma, mem->len);
	if (IS_ERR_OR_NULL(va)) {
		dev_err(dev, "Unable to map memory region: %pad+0x%zx\n",
			&mem->dma, mem->len);
		return -ENOMEM;
	}

	/* Update memory entry va */
	mem->va = va;

	return 0;
}

static int k230_rproc_mem_release(struct rproc *rproc,
				   struct rproc_mem_entry *mem)
{
	dev_dbg(rproc->dev.parent, "unmap memory: %pa\n", &mem->dma);
	iounmap((__force __iomem void *)mem->va);

	return 0;
}

static int k230_rproc_prepare(struct rproc *rproc)
{
	struct device *dev = rproc->dev.parent;
	struct device_node *np = dev->of_node;
	struct of_phandle_iterator it;
	struct rproc_mem_entry *mem;
	struct reserved_mem *rmem;

	/* Register associated reserved memory regions */
	of_phandle_iterator_init(&it, np, "memory-region", NULL, 0);
	while (of_phandle_iterator_next(&it) == 0) {

		rmem = of_reserved_mem_lookup(it.node);
		if (!rmem) {
			of_node_put(it.node);
			dev_err(&rproc->dev,
				"unable to acquire memory-region\n");
			return -EINVAL;
		}

		if (rmem->base > U32_MAX) {
			of_node_put(it.node);
			return -EINVAL;
		}

		mem = rproc_mem_entry_init(dev, NULL,
					   (dma_addr_t)rmem->base,
					   rmem->size, rmem->base,
					   k230_rproc_mem_alloc,
					   k230_rproc_mem_release,
					   it.node->name);

		if (!mem) {
			of_node_put(it.node);
			return -ENOMEM;
		}

		rproc_add_carveout(rproc, mem);
	}

	return 0;
}

static int k230_rproc_start(struct rproc *rproc)
{
	struct device *dev = rproc->dev.parent;
	struct k230_rproc *priv = rproc->priv;
	void __iomem *rstvec;
	int ret;

	/* set boot addr */
	rstvec = ioremap(RSTVEC_ADDR, 4);
	iowrite32(rproc->bootaddr, rstvec);
	iounmap(rstvec);

	/*
	 * CPU0 not support assert and deassert, clk operation should be added
	 * later
	 */
	ret = reset_control_reset(priv->rst);
	if (ret) {
		dev_err(dev, "reset_control_deassert() failed: %d\n", ret);
		return ret;
	}

	dev_dbg(dev, "starting small core on addr = 0x%llx\n", rproc->bootaddr);

	return 0;
}

static int k230_rproc_stop(struct rproc *rproc)
{
	struct device *dev = rproc->dev.parent;
	struct k230_rproc *priv = rproc->priv;
	int ret;

	dev_dbg(dev, "stoping small core");

	return 0;
}

static int k230_rproc_parse_fw(struct rproc *rproc, const struct firmware *fw)
{
	if (rproc_elf_load_rsc_table(rproc, fw))
		dev_warn(&rproc->dev, "No resource table in elf\n");

	return 0;
}

static const struct rproc_ops k230_rproc_ops = {
	.prepare	= k230_rproc_prepare,
	.start		= k230_rproc_start,
	.stop		= k230_rproc_stop,
	.load		= rproc_elf_load_segments,
	.parse_fw	= k230_rproc_parse_fw,
	.find_loaded_rsc_table = rproc_elf_find_loaded_rsc_table,
	.sanity_check	= rproc_elf_sanity_check,
	.get_boot_addr	= rproc_elf_get_boot_addr,
};

static int k230_rproc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct k230_rproc *priv;
	struct reset_control *rst;
	struct rproc *rproc = NULL;
	const char *fw_name;
	int ret;

	ret = rproc_of_parse_firmware(dev, 0, &fw_name);
	if (ret) {
		dev_err(dev, "No firmware filename given\n");
		return ret;
	}

	rst = devm_reset_control_get_exclusive(dev, NULL);
	if (IS_ERR(rst))
		return dev_err_probe(dev, PTR_ERR(rst), "unable to get reset control\n");


	rproc = devm_rproc_alloc(dev, dev_name(dev), &k230_rproc_ops, fw_name,
				 sizeof(*priv));
	if (!rproc) {
		dev_err(dev, "unable to allocate remoteproc\n");
		return -ENOMEM;
	}

	rproc->has_iommu = false;

	priv = rproc->priv;
	priv->dev = dev;
	priv->rst = rst;
	priv->rproc = rproc;


	platform_set_drvdata(pdev, rproc);

	ret = devm_rproc_add(dev, rproc);
	if (ret) {
		dev_err(dev, "rproc_add failed: %d\n", ret);
		return ret;
	}

	return 0;
}

static void k230_rproc_remove(struct platform_device *pdev)
{
	struct rproc *rproc = platform_get_drvdata(pdev);

	if (atomic_read(&rproc->power) > 0)
		rproc_shutdown(rproc);

	rproc_del(rproc);
}

static const struct of_device_id k230_rproc_of_match[] = {
	{ .compatible = "canaan,k230-rproc" },
	{},
};

MODULE_DEVICE_TABLE(of, k230_rproc_of_match);

static struct platform_driver k230_rproc_driver = {
	.probe = k230_rproc_probe,
	.remove = k230_rproc_remove,
	.driver = {
		.name = "k230-rproc",
		.of_match_table = k230_rproc_of_match,
	},
};

module_platform_driver(k230_rproc_driver);

MODULE_AUTHOR("Junhui Liu <junhui.liu@pigmoral.tech>");
MODULE_DESCRIPTION("Canaan k230 remote processor control driver");
MODULE_LICENSE("GPL v2");
