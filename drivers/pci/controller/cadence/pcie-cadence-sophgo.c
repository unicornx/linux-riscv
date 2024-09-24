// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2017 Cadence
// Cadence PCIe host controller driver.
// Author: Cyrille Pitchen <cyrille.pitchen@free-electrons.com>

#include <linux/kernel.h>
#include <linux/of_address.h>
#include <linux/of_pci.h>
#include <linux/msi.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>


#include "pcie-cadence.h"
#include "pcie-cadence-sophgo.h"

#define MAX_MSI_IRQS			512
#define MAX_MSI_IRQS_PER_CTRL		1
#define MAX_MSI_CTRLS			(MAX_MSI_IRQS / MAX_MSI_IRQS_PER_CTRL)
#define MSI_DEF_NUM_VECTORS		512
#define BYTE_NUM_PER_MSI_VEC		4

// mango sideband signals
#define CDNS_PCIE_IRS_REG0804       0x0804
#define CDNS_PCIE_IRS_REG080C       0x080C
#define CDNS_PCIE_IRS_REG0810       0x0810
#define CDNS_PCIE_IRS_REG085C       0x085C
#define CDNS_PCIE_IRS_REG0860       0x0860
#define CDNS_PCIE_IRS_REG0864       0x0864
#define CDNS_PCIE_IRS_REG0868       0x0868
#define CDNS_PCIE_IRS_REG086C       0x086C

#define CDNS_PCIE_IRS_REG0804_CLR_LINK0_MSI_IN_BIT		2
#define CDNS_PCIE_IRS_REG0804_CLR_LINK1_MSI_IN_BIT		3
#define CDNS_PCIE_IRS_REG0810_ST_LINK0_MSI_IN_BIT		2
#define CDNS_PCIE_IRS_REG0810_ST_LINK1_MSI_IN_BIT		3

#define CDNS_PLAT_CPU_TO_BUS_ADDR       0xCFFFFFFFFF

struct sg2042_pcie {
	struct cdns_pcie	*cdns_pcie;

	// Fully private members for sg2042
	u16			pcie_id;
	u16			link_id;
	u32			top_intc_used;
	u32			msix_supported;
	struct irq_domain	*msi_domain;
	int			msi_irq;
	struct irq_domain	*irq_domain_parent;
	dma_addr_t		msi_data;
	void			*msi_page;
	struct irq_chip		*msi_irq_chip;
	u32			num_vectors;
	u32			num_applied_vecs;
	raw_spinlock_t		lock;
	DECLARE_BITMAP(msi_irq_in_use, MAX_MSI_IRQS);
	struct regmap		*syscon;
};

static u64 cdns_mango_cpu_addr_fixup(struct cdns_pcie *pcie, u64 cpu_addr)
{
	return cpu_addr & CDNS_PLAT_CPU_TO_BUS_ADDR;
}

static const struct cdns_pcie_ops cdns_mango_ops = {
	.cpu_addr_fixup = cdns_mango_cpu_addr_fixup,
};

static int sg2042_cdns_pcie_config_read(struct pci_bus *bus, unsigned int devfn,
				    int where, int size, u32 *value)
{
	if (pci_is_root_bus(bus))
		return pci_generic_config_read32(bus, devfn, where, size,
						 value);

	return pci_generic_config_read(bus, devfn, where, size, value);
}

static int sg2042_cdns_pcie_config_write(struct pci_bus *bus, unsigned int devfn,
				     int where, int size, u32 value)
{
	if (pci_is_root_bus(bus))
		return pci_generic_config_write32(bus, devfn, where, size,
						  value);

	return pci_generic_config_write(bus, devfn, where, size, value);
}

static struct pci_ops cdns_pcie_host_ops = {
	.map_bus	= cdns_pci_map_bus,
	.read		= sg2042_cdns_pcie_config_read,
	.write		= sg2042_cdns_pcie_config_write,
};

static const struct of_device_id cdns_pcie_host_of_match[] = {
	{ .compatible = "sophgo,cdns-pcie-host" },

	{ },
};

// 这是 mango 自己定义的函数，会保留
static int cdns_pcie_msi_init(struct sg2042_pcie *pcie)
{
	struct device *dev = pcie->cdns_pcie->dev;
	u64 msi_target = 0;
	u32 value = 0;

	// support 512 msi vectors
	pcie->msi_page = dma_alloc_coherent(dev, 2048, &pcie->msi_data,
					  (GFP_KERNEL|GFP_DMA32|__GFP_ZERO));
	if (pcie->msi_page == NULL)
		return -1;

	dev_info(dev, "msi_data is 0x%llx\n", pcie->msi_data);
	msi_target = (u64)pcie->msi_data;

	if (pcie->link_id == 1) {
		/* Program the msi_data */
		regmap_write(pcie->syscon, CDNS_PCIE_IRS_REG0868,
				 lower_32_bits(msi_target));
		regmap_write(pcie->syscon, CDNS_PCIE_IRS_REG086C,
				 upper_32_bits(msi_target));

		regmap_read(pcie->syscon, CDNS_PCIE_IRS_REG080C, &value);
		value = (value & 0xffff0000) | MAX_MSI_IRQS;
		regmap_write(pcie->syscon, CDNS_PCIE_IRS_REG080C, value);
	} else {
		/* Program the msi_data */
		regmap_write(pcie->syscon, CDNS_PCIE_IRS_REG0860,
				 lower_32_bits(msi_target));
		regmap_write(pcie->syscon, CDNS_PCIE_IRS_REG0864,
				 upper_32_bits(msi_target));

		regmap_read(pcie->syscon, CDNS_PCIE_IRS_REG085C, &value);
		value = (value & 0x0000ffff) | (MAX_MSI_IRQS << 16);
		regmap_write(pcie->syscon, CDNS_PCIE_IRS_REG085C, value);
	}

	return 0;
}

//////////////////////////////////////////////////////////////////
// mango 私有逻辑，需要保留
static void cdns_pcie_msi_ack_irq(struct irq_data *d)
{
	irq_chip_ack_parent(d);
}

static void cdns_pcie_msi_mask_irq(struct irq_data *d)
{
	pci_msi_mask_irq(d);
	irq_chip_mask_parent(d);
}

static void cdns_pcie_msi_unmask_irq(struct irq_data *d)
{
	pci_msi_unmask_irq(d);
	irq_chip_unmask_parent(d);
}

static struct irq_chip cdns_pcie_msi_irq_chip = {
	.name = "cdns-msi",
	.irq_ack = cdns_pcie_msi_ack_irq,
	.irq_mask = cdns_pcie_msi_mask_irq,
	.irq_unmask = cdns_pcie_msi_unmask_irq,
};

static struct msi_domain_info cdns_pcie_msi_domain_info = {
	.flags	= (MSI_FLAG_USE_DEF_DOM_OPS | MSI_FLAG_USE_DEF_CHIP_OPS),
	.chip	= &cdns_pcie_msi_irq_chip,
};

static struct msi_domain_info cdns_pcie_top_intr_msi_domain_info = {
	.flags	= (MSI_FLAG_USE_DEF_DOM_OPS | MSI_FLAG_USE_DEF_CHIP_OPS
		   | MSI_FLAG_PCI_MSIX),
	.chip	= &cdns_pcie_msi_irq_chip,
};

// FIXME：有关 check_vendor_id 我发现 a42bea5b2840bf1e33aafa4158d26c064227dfe8
// 这个 commit 里 sophgo 塞了一些东西在内核里（除了驱动和 dts 之外的部分），
// 这些东西好 upstream 吗？感觉需要拿出来讨论一下
struct vendor_id_list vendor_id_list[] = {
	{"Inter X520", 0x8086, 0x10fb},
	{"Inter I40E", 0x8086, 0x1572},
	//{"WangXun RP1000", 0x8088},
	{"Switchtec", 0x11f8,0x4052},
	{"Mellanox ConnectX-2", 0x15b3, 0x6750}
};

size_t vendor_id_list_num = ARRAY_SIZE(vendor_id_list);

int check_vendor_id(struct pci_dev *dev, struct vendor_id_list vendor_id_list[],
			size_t vendor_id_list_num)
{
	uint16_t device_vendor_id;
	uint16_t device_id;

	if (pci_read_config_word(dev, PCI_VENDOR_ID, &device_vendor_id) != 0) {
		pr_err("Failed to read device vendor ID\n");
		return 0;
	}

	if (pci_read_config_word(dev, PCI_DEVICE_ID, &device_id) != 0) {
		pr_err("Failed to read device vendor ID\n");
		return 0;
	}

	for (int i = 0; i < vendor_id_list_num; ++i) {
		if (device_vendor_id == vendor_id_list[i].vendor_id && device_id == vendor_id_list[i].device_id) {
			pr_info("dev: %s vendor ID: 0x%04x device ID: 0x%04x Enable MSI-X IRQ\n",
				vendor_id_list[i].name, device_vendor_id, device_id);
			return 1;
		}
	}
	return 0;
}


static int cdns_pcie_msi_setup_for_top_intc(struct sg2042_pcie *pcie, int intc_id)
{
	struct irq_domain *irq_parent = cdns_pcie_get_parent_irq_domain(intc_id);
	struct device *dev = pcie->cdns_pcie->dev;
	struct fwnode_handle *fwnode = of_node_to_fwnode(dev->of_node);

	if (pcie->msix_supported == 1) {
		pcie->msi_domain = pci_msi_create_irq_domain(fwnode,
							   &cdns_pcie_top_intr_msi_domain_info,
							   irq_parent);
	} else {
		pcie->msi_domain = pci_msi_create_irq_domain(fwnode,
							   &cdns_pcie_msi_domain_info,
							   irq_parent);
	}

	if (!pcie->msi_domain) {
		dev_err(dev, "create msi irq domain failed\n");
		return -ENODEV;
	}

	return 0;
}

/* MSI int handler */
static irqreturn_t cdns_handle_msi_irq(struct sg2042_pcie *pcie)
{
	u32 i, pos, irq;
	unsigned long val;
	u32 status, num_vectors;
	irqreturn_t ret = IRQ_NONE;

	num_vectors = pcie->num_applied_vecs;
	for (i = 0; i <= num_vectors; i++) {
		status = readl((void *)(pcie->msi_page + i * BYTE_NUM_PER_MSI_VEC));
		if (!status)
			continue;

		ret = IRQ_HANDLED;
		val = status;
		pos = 0;
		while ((pos = find_next_bit(&val, MAX_MSI_IRQS_PER_CTRL,
					    pos)) != MAX_MSI_IRQS_PER_CTRL) {
			irq = irq_find_mapping(pcie->irq_domain_parent,
					       (i * MAX_MSI_IRQS_PER_CTRL) +
					       pos);
			generic_handle_irq(irq);
			pos++;
		}
		writel(0, ((void *)(pcie->msi_page) + i * BYTE_NUM_PER_MSI_VEC));
	}
	if (ret == IRQ_NONE) {
		ret = IRQ_HANDLED;
		for (i = 0; i <= num_vectors; i++) {
			for (pos = 0; pos < MAX_MSI_IRQS_PER_CTRL; pos++) {
				irq = irq_find_mapping(pcie->irq_domain_parent,
						       (i * MAX_MSI_IRQS_PER_CTRL) +
						       pos);
				if (!irq)
					continue;
				generic_handle_irq(irq);
			}
		}
	}

	return ret;
}

/* Chained MSI interrupt service routine */
static void cdns_chained_msi_isr(struct irq_desc *desc)
{
	struct irq_chip *chip = irq_desc_get_chip(desc);
	struct sg2042_pcie *pcie;
	u32 status = 0;
	u32 st_msi_in_bit = 0;
	u32 clr_msi_in_bit = 0;

	chained_irq_enter(chip, desc);

	pcie = irq_desc_get_handler_data(desc);
	if (pcie->link_id == 1) {
		st_msi_in_bit = CDNS_PCIE_IRS_REG0810_ST_LINK1_MSI_IN_BIT;
		clr_msi_in_bit = CDNS_PCIE_IRS_REG0804_CLR_LINK1_MSI_IN_BIT;
	} else {
		st_msi_in_bit = CDNS_PCIE_IRS_REG0810_ST_LINK0_MSI_IN_BIT;
		clr_msi_in_bit = CDNS_PCIE_IRS_REG0804_CLR_LINK0_MSI_IN_BIT;
	}

	regmap_read(pcie->syscon, CDNS_PCIE_IRS_REG0810, &status);
	if ((status >> st_msi_in_bit) & 0x1) {
		WARN_ON(!IS_ENABLED(CONFIG_PCI_MSI));

		//clear msi interrupt bit reg0810[2]
		regmap_read(pcie->syscon, CDNS_PCIE_IRS_REG0804, &status);
		status |= ((u32)0x1 << clr_msi_in_bit);
		regmap_write(pcie->syscon, CDNS_PCIE_IRS_REG0804, status);

		status &= ~((u32)0x1 << clr_msi_in_bit);
		regmap_write(pcie->syscon, CDNS_PCIE_IRS_REG0804, status);

		cdns_handle_msi_irq(pcie);
	}

	chained_irq_exit(chip, desc);
}

static int cdns_pci_msi_set_affinity(struct irq_data *d,
				   const struct cpumask *mask, bool force)
{
	return -EINVAL;
}

static void cdns_pci_bottom_mask(struct irq_data *d)
{
}

static void cdns_pci_bottom_unmask(struct irq_data *d)
{
}

static void cdns_pci_setup_msi_msg(struct irq_data *d, struct msi_msg *msg)
{
	struct sg2042_pcie *pcie = irq_data_get_irq_chip_data(d);
	struct device *dev = pcie->cdns_pcie->dev;
	u64 msi_target;

	msi_target = (u64)pcie->msi_data;

	msg->address_lo = lower_32_bits(msi_target) + BYTE_NUM_PER_MSI_VEC * d->hwirq;
	msg->address_hi = upper_32_bits(msi_target);
	msg->data = 1;

	pcie->num_applied_vecs = d->hwirq;

	dev_err(dev, "msi#%d address_hi %#x address_lo %#x\n",
		(int)d->hwirq, msg->address_hi, msg->address_lo);
}

static void cdns_pci_bottom_ack(struct irq_data *d)
{
}

static struct irq_chip cdns_pci_msi_bottom_irq_chip = {
	.name = "CDNS-PCI-MSI",
	.irq_ack = cdns_pci_bottom_ack,
	.irq_compose_msi_msg = cdns_pci_setup_msi_msg,
	.irq_set_affinity = cdns_pci_msi_set_affinity,
	.irq_mask = cdns_pci_bottom_mask,
	.irq_unmask = cdns_pci_bottom_unmask,
};

static int cdns_pcie_irq_domain_alloc(struct irq_domain *domain,
				    unsigned int virq, unsigned int nr_irqs,
				    void *args)
{
	struct sg2042_pcie *pcie = domain->host_data;
	unsigned long flags;
	u32 i;
	int bit;

	raw_spin_lock_irqsave(&pcie->lock, flags);

	bit = bitmap_find_free_region(pcie->msi_irq_in_use, pcie->num_vectors,
				      order_base_2(nr_irqs));

	raw_spin_unlock_irqrestore(&pcie->lock, flags);

	if (bit < 0)
		return -ENOSPC;

	for (i = 0; i < nr_irqs; i++)
		irq_domain_set_info(domain, virq + i, bit + i,
				    pcie->msi_irq_chip,
				    pcie, handle_edge_irq,
				    NULL, NULL);

	return 0;
}

static void cdns_pcie_irq_domain_free(struct irq_domain *domain,
				    unsigned int virq, unsigned int nr_irqs)
{
	struct irq_data *d = irq_domain_get_irq_data(domain, virq);
	struct sg2042_pcie *pcie = irq_data_get_irq_chip_data(d);
	unsigned long flags;

	raw_spin_lock_irqsave(&pcie->lock, flags);

	bitmap_release_region(pcie->msi_irq_in_use, d->hwirq,
			      order_base_2(nr_irqs));

	raw_spin_unlock_irqrestore(&pcie->lock, flags);
}

static const struct irq_domain_ops cdns_pcie_msi_domain_ops = {
	.alloc	= cdns_pcie_irq_domain_alloc,
	.free	= cdns_pcie_irq_domain_free,
};

static int cdns_pcie_allocate_domains(struct sg2042_pcie *pcie)
{
	struct device *dev = pcie->cdns_pcie->dev;
	struct fwnode_handle *fwnode = of_node_to_fwnode(dev->of_node);

	pcie->irq_domain_parent = irq_domain_create_linear(fwnode, pcie->num_vectors,
					       &cdns_pcie_msi_domain_ops, pcie);
	if (!pcie->irq_domain_parent) {
		dev_err(dev, "Failed to create IRQ domain\n");
		return -ENOMEM;
	}

	irq_domain_update_bus_token(pcie->irq_domain_parent, DOMAIN_BUS_NEXUS);

	pcie->msi_domain = pci_msi_create_irq_domain(fwnode,
						   &cdns_pcie_msi_domain_info,
						   pcie->irq_domain_parent);
	if (!pcie->msi_domain) {
		dev_err(dev, "Failed to create MSI domain\n");
		irq_domain_remove(pcie->irq_domain_parent);
		return -ENOMEM;
	}

	return 0;
}

static void cdns_pcie_free_msi(struct sg2042_pcie *pcie)
{
	struct device *dev = pcie->cdns_pcie->dev;

	if (pcie->msi_irq) {
		irq_set_chained_handler(pcie->msi_irq, NULL);
		irq_set_handler_data(pcie->msi_irq, NULL);
	}

	irq_domain_remove(pcie->msi_domain);
	irq_domain_remove(pcie->irq_domain_parent);

	if (pcie->msi_page)
		dma_free_coherent(dev, 1024, pcie->msi_page, pcie->msi_data);

}

static int cdns_pcie_msi_setup(struct sg2042_pcie *pcie)
{
	int ret = 0;

	raw_spin_lock_init(&pcie->lock);

	if (IS_ENABLED(CONFIG_PCI_MSI)) {
		pcie->msi_irq_chip = &cdns_pci_msi_bottom_irq_chip;

		ret = cdns_pcie_allocate_domains(pcie);
		if (ret)
			return ret;

		if (pcie->msi_irq)
			irq_set_chained_handler_and_data(pcie->msi_irq, cdns_chained_msi_isr, pcie);
	}

	return ret;
}

// mango 私有逻辑，需要保留
//////////////////////////////////////////////////////////////////

static int sg2042_pcie_host_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct device_node *np_top;
	struct pci_host_bridge *bridge;

	struct cdns_pcie *cdns_pcie;
	struct sg2042_pcie *pcie;

	struct cdns_pcie_rc *rc = NULL;

	int ret;
	int top_intc_id = -1;

	struct regmap *syscon;

	pcie = devm_kzalloc(dev, sizeof(*pcie), GFP_KERNEL);
	if (!pcie)
		return -ENOMEM;

	if (!IS_ENABLED(CONFIG_PCIE_CADENCE_HOST))
		return -ENODEV;

	bridge = devm_pci_alloc_host_bridge(dev, sizeof(*rc));
	if (!bridge)
		return -ENOMEM;
	bridge->ops = &cdns_pcie_host_ops;
	rc = pci_host_bridge_priv(bridge);

	cdns_pcie = &rc->pcie;
	cdns_pcie->dev = dev;
	cdns_pcie->ops = &cdns_mango_ops;
	pcie->cdns_pcie = cdns_pcie;

	////////////////////////////////////////////////////////////
	// parse dts for sg2042
	np_top = of_parse_phandle(np, "pcie-syscon", 0);
	if (!np_top) {
		dev_err(dev, "%s can't get pcie-syscon node\n", __func__);
		return -ENOMEM;
	}
	syscon = syscon_node_to_regmap(np_top);
	if (IS_ERR(syscon)) {
		dev_err(dev, "cannot get regmap\n");
		return -ENOMEM;
	}
	pcie->syscon = syscon;

	pcie->pcie_id = 0xffff;
	of_property_read_u16(np, "pcie-id", &pcie->pcie_id);

	pcie->link_id = 0xffff;
	of_property_read_u16(np, "link-id", &pcie->link_id);

	pcie->msix_supported = 0;
	of_property_read_u32(np, "msix-supported", &pcie->msix_supported);

	pcie->top_intc_used = 0;
	of_property_read_u32(np, "top-intc-used", &pcie->top_intc_used);
	if (pcie->top_intc_used == 1)
		of_property_read_u32(np, "top-intc-id", &top_intc_id);

	platform_set_drvdata(pdev, pcie);

	pm_runtime_enable(dev);
	ret = pm_runtime_get_sync(dev);
	if (ret < 0) {
		dev_err(dev, "pm_runtime_get_sync() failed\n");
		goto err_get_sync;
	}

	////////////////////////////////////////////////////////////
	// do sg2042 related initialization work
	// FIXME: 下面这段逻辑是从原来的 cdns_pcie_host_init 里挪出来的
	// 但是感觉也可以和下面一段逻辑合并，都是处理 not use top-intc 的情况
	if (pcie->top_intc_used == 0) {
		pcie->num_vectors = MSI_DEF_NUM_VECTORS;
		pcie->num_applied_vecs = 0;
		if (IS_ENABLED(CONFIG_PCI_MSI)) {
			ret = cdns_pcie_msi_init(pcie);
			if (ret) {
				dev_err(dev, "cdns_pcie_msi_init() failed\n");
				goto err_get_sync;
			}
		}
	}

	if ((pcie->top_intc_used == 0) && (IS_ENABLED(CONFIG_PCI_MSI))) {
		pcie->msi_irq = platform_get_irq_byname(pdev, "msi");
		if (pcie->msi_irq <= 0) {
			dev_err(dev, "failed to get MSI irq\n");
			goto err_init_irq;
		}
	}

	if (pcie->top_intc_used == 0) {
		ret = cdns_pcie_msi_setup(pcie);
		if (ret < 0)
			goto err_host_probe;
	} else if (pcie->top_intc_used == 1) {
		ret = cdns_pcie_msi_setup_for_top_intc(pcie, top_intc_id);
		if (ret < 0)
			goto err_host_probe;
	}

	ret = cdns_pcie_init_phy(dev, cdns_pcie);
	if (ret) {
		dev_err(dev, "Failed to init phy\n");
		goto err_get_sync;
	}
	
	ret = cdns_pcie_host_setup(rc);
	if (ret < 0) {
		goto err_pcie_setup;
	}

	return 0;

 err_host_probe:
 err_init_irq:
	if ((pcie->top_intc_used == 0) && pci_msi_enabled())
		cdns_pcie_free_msi(pcie);

 err_pcie_setup:
	cdns_pcie_disable_phy(cdns_pcie);

err_get_sync:
	pm_runtime_put(dev);
	pm_runtime_disable(dev);

	return ret;
}

static void cdns_pcie_shutdown(struct platform_device *pdev)
{
	struct sg2042_pcie *pcie = platform_get_drvdata(pdev);
	struct cdns_pcie *cdns_pcie = pcie->cdns_pcie;
	struct device *dev = &pdev->dev;

	cdns_pcie_disable_phy(cdns_pcie);
	pm_runtime_put(dev);
	pm_runtime_disable(dev);
}

static struct platform_driver cdns_pcie_host_driver = {
	.driver = {
		.name = "cdns-pcie-host",
		.of_match_table = cdns_pcie_host_of_match,
		.pm	= &cdns_pcie_pm_ops,
	},
	.probe = sg2042_pcie_host_probe,
	.shutdown = cdns_pcie_shutdown,
};
builtin_platform_driver(cdns_pcie_host_driver);
