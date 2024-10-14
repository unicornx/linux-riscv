// SPDX-License-Identifier: GPL-2.0
/*
 * pcie-sg2042 - PCIe controller driver for Sophgo SG2042 SoC
 *
 * Copyright (C) 2024 Sophgo Technology Inc.
 * Copyright (C) 2024 Chen Wang <unicorn_wang@outlook.com>
 */

#include <linux/irq.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqdomain.h>
#include <linux/kernel.h>
#include <linux/mfd/syscon.h>
#include <linux/msi.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_pci.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>

#include "pcie-cadence.h"

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
	u32			pcie_id; // FIXME dts 设置了，但是代码中用不到
	u32			link_id;
	u32			top_intc_used;

	// 用于存放 pci_msi_create_irq_domain 的结果
	// 这个变量对于使用或者于不使用 top-intc 的情况都会用到
	struct irq_domain	*msi_domain;

	// 下面的成员都是和 pcie-intc 处理有关
	int			msi_irq;
	struct irq_domain	*irq_domain; // FIXME：这个成员名称和结构体名字相同了，不好的编程习惯，建议改掉。
	dma_addr_t		msi_data;
	void			*msi_page;
	struct irq_chip		*msi_irq_chip;
	u32			num_vectors; // FIXME: 这个成员感觉用处不大。初始化为 MSI_DEF_NUM_VECTORS 就不变的。
	u32			num_applied_vecs; // FIXME：增加这个成员的作用是为了代码优化，避免每次都要循环 512。可能改个名字会更好，仅仅是为了优化。
	raw_spinlock_t		lock;
	DECLARE_BITMAP(msi_irq_in_use, MAX_MSI_IRQS);
	struct regmap		*syscon;
};

static void sg2042_msi_ack_irq(struct irq_data *d)
{
	irq_chip_ack_parent(d);
}

static void sg2042_msi_mask_irq(struct irq_data *d)
{
	pci_msi_mask_irq(d);
	irq_chip_mask_parent(d);
}

static void sg2042_msi_unmask_irq(struct irq_data *d)
{
	pci_msi_unmask_irq(d);
	irq_chip_unmask_parent(d);
}

static struct irq_chip sg2042_pcie_msi_irq_chip = {
	.name = "PCI-MSI",
	.irq_ack = sg2042_msi_ack_irq,
	.irq_mask = sg2042_msi_mask_irq,
	.irq_unmask = sg2042_msi_unmask_irq,
};

static struct msi_domain_info sg2042_pcie_msi_domain_info = {
	.flags	= (MSI_FLAG_USE_DEF_DOM_OPS | MSI_FLAG_USE_DEF_CHIP_OPS),
	.chip	= &sg2042_pcie_msi_irq_chip,
};

// top-intc
static struct irq_domain *sg2042_pcie_get_parent_irq_domain(struct device *dev)
{
	struct device_node *np = dev->of_node;
	struct device_node *parent;
	struct irq_domain *domain;

	if (!of_find_property(np, "interrupt-parent", NULL)) {
		dev_err(dev, "Can't find interrupt-parent!\n");
		return NULL;
	}

	parent = of_irq_find_parent(np);
	if (!parent) {
		dev_err(dev, "Can't find parent node!\n");
		return ERR_PTR(-ENXIO);
	}

	domain = irq_find_host(parent);
	of_node_put(parent);
	if (!domain) {
		dev_err(dev, "Can't find domain of interrupt-parent!\n");
		return ERR_PTR(-ENXIO);
	}

	return domain;
}


static int sg2042_pcie_setup_top_intc(struct sg2042_pcie *pcie)
{
	struct device *dev = pcie->cdns_pcie->dev;
	struct fwnode_handle *fwnode = of_node_to_fwnode(dev->of_node);
	struct irq_domain *parent_domain = sg2042_pcie_get_parent_irq_domain(dev);

	pcie->msi_domain = pci_msi_create_irq_domain(fwnode,
						     &sg2042_pcie_msi_domain_info,
						     parent_domain);

	if (!pcie->msi_domain) {
		dev_err(dev, "create msi irq domain failed\n");
		return -ENODEV;
	}

	return 0;
}

// pcie-intc
static irqreturn_t sg2042_pcie_handle_msi_irq(struct sg2042_pcie *pcie)
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
		// FIXME: 按照 dw 的做法，得到 bitmap bit 后直接 generic_handle_domain_irq 不可以吗？
		while ((pos = find_next_bit(&val, MAX_MSI_IRQS_PER_CTRL,
					    pos)) != MAX_MSI_IRQS_PER_CTRL) {
			irq = irq_find_mapping(pcie->irq_domain,
					       (i * MAX_MSI_IRQS_PER_CTRL) +
					       pos);
			generic_handle_irq(irq);
			pos++;
		}
		writel(0, ((void *)(pcie->msi_page) + i * BYTE_NUM_PER_MSI_VEC));
	}
	// FIXME: 会再来一遍，据说是为了防止漏中断。可以看看可能可以参考 dw 的处理方式优化。或者作为 vendor 补丁方式
	if (ret == IRQ_NONE) {
		ret = IRQ_HANDLED;
		for (i = 0; i <= num_vectors; i++) {
			for (pos = 0; pos < MAX_MSI_IRQS_PER_CTRL; pos++) {
				irq = irq_find_mapping(pcie->irq_domain,
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

// pcie-intc
// msi 逻辑会走到这里
// 核心是调用的 sg2042_pcie_handle_msi_irq
/* Chained MSI interrupt service routine */
static void sg2042_pcie_chained_msi_isr(struct irq_desc *desc)
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

		// FIXME: 为啥对 804 写 1 然后还要写 0？一般不是会自动翻转吗？
		// A: 和硬件 IC 确认了一下，这步不能省，硬件不会自动翻转为 0，需要软件手动翻转
		status &= ~((u32)0x1 << clr_msi_in_bit);
		regmap_write(pcie->syscon, CDNS_PCIE_IRS_REG0804, status);

		// FIXME: 为啥是在 cdns_handle_msi_irq之前清，而不是之后？
		// A: 先清中断，是想让其他中断能够继续上报, 方便后面处理
		// 但和硬件确认后，清中断只是将 status 标志恢复为 0，但 MSI data 
		// 的写入操作和中断上报完全是异步的，RC 一旦收到 EP 的 MSI 通知
		// 就会对 msi data 内存区进行写，和 cpu 这边是否去 clear 并无关系。
		sg2042_pcie_handle_msi_irq(pcie);
	}

	chained_irq_exit(chip, desc);
}

static int sg2042_pcie_msi_set_affinity(struct irq_data *d,
					const struct cpumask *mask, bool force)
{
	return -EINVAL;
}

static void sg2042_pcie_bottom_mask(struct irq_data *d)
{
}

static void sg2042_pcie_bottom_unmask(struct irq_data *d)
{
}

static void sg2042_pcie_setup_msi_msg(struct irq_data *d, struct msi_msg *msg)
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

static void sg2042_pcie_bottom_ack(struct irq_data *d)
{
}

// 这个也是参考的 drivers/pci/controller/dwc/pcie-designware-host.c
// 中的 dw_pci_msi_bottom_irq_chip
// FIXME: 大部分函数都没有实现，是否可以删掉？需要学习一下 irq_chip 的回调函数
// sg2042_pcie_msi_bottom_irq_chip 会在 sg2042_pcie_irq_domain_alloc 中
// 也就是 irq_domain 的 .alloc 回调中传入 irq_domain_set_info
static struct irq_chip sg2042_pcie_msi_bottom_irq_chip = {
	.name = "SG2042-PCI-MSI",
	.irq_ack = sg2042_pcie_bottom_ack,
	.irq_compose_msi_msg = sg2042_pcie_setup_msi_msg,
	.irq_set_affinity = sg2042_pcie_msi_set_affinity,
	.irq_mask = sg2042_pcie_bottom_mask,
	.irq_unmask = sg2042_pcie_bottom_unmask,
};

// 以下的 sg2042_pcie_msi_domain_ops 和 sg2042_pcie_allocate_domains
// 参考了 drivers/pci/controller/dwc/pcie-designware-host.c
// 中的 dw_pcie_msi_domain_ops 和 dw_pcie_allocate_domains
static int sg2042_pcie_irq_domain_alloc(struct irq_domain *domain,
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

static void sg2042_pcie_irq_domain_free(struct irq_domain *domain,
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

static const struct irq_domain_ops sg2042_pcie_msi_domain_ops = {
	.alloc	= sg2042_pcie_irq_domain_alloc,
	.free	= sg2042_pcie_irq_domain_free,
};

// 这个函数参考了 drivers/pci/controller/dwc/pcie-designware-host.c
// 中的 dw_pcie_allocate_domains
static int sg2042_pcie_allocate_domains(struct sg2042_pcie *pcie)
{
	struct device *dev = pcie->cdns_pcie->dev;
	struct fwnode_handle *fwnode = of_node_to_fwnode(dev->of_node);

	// 创建 irq domain
	// 这个 irq_domain 不仅在下面创建 msi irq domain 中作为 parent domain
	// 而且在 sg2042_pcie_handle_msi_irq/sg2042_pcie_free_msi 中会被使用，free 中
	// 主要负责释放
	pcie->irq_domain = irq_domain_create_linear(fwnode, pcie->num_vectors,
						    &sg2042_pcie_msi_domain_ops,
						    pcie);
	if (!pcie->irq_domain) {
		dev_err(dev, "Failed to create IRQ domain\n");
		return -ENOMEM;
	}

	irq_domain_update_bus_token(pcie->irq_domain, DOMAIN_BUS_NEXUS);

	pcie->msi_domain = pci_msi_create_irq_domain(fwnode,
						     &sg2042_pcie_msi_domain_info,
						     pcie->irq_domain);
	if (!pcie->msi_domain) {
		dev_err(dev, "Failed to create MSI domain\n");
		irq_domain_remove(pcie->irq_domain);
		return -ENOMEM;
	}

	return 0;
}

static void sg2042_pcie_free_msi(struct sg2042_pcie *pcie)
{
	struct device *dev = pcie->cdns_pcie->dev;

	if (pcie->msi_irq) {
		irq_set_chained_handler(pcie->msi_irq, NULL);
		irq_set_handler_data(pcie->msi_irq, NULL);
	}

	irq_domain_remove(pcie->msi_domain);
	irq_domain_remove(pcie->irq_domain);

	if (pcie->msi_page)
		dma_free_coherent(dev, 1024, pcie->msi_page, pcie->msi_data);

}

// 分配一块连续的内存用于存放 msi data, 然后将该内存的物理地址告诉 pcie-intc
static int sg2042_pcie_msi_init(struct sg2042_pcie *pcie)
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

static int sg2042_pcie_setup_msi(struct sg2042_pcie *pcie, struct platform_device *pdev)
{
	struct device *dev = pcie->cdns_pcie->dev;
	int ret = 0;

	pcie->num_vectors = MSI_DEF_NUM_VECTORS;
	pcie->num_applied_vecs = 0; // FIXME. 可以省略，默认为 zero

	ret = sg2042_pcie_msi_init(pcie);
	if (ret) {
		dev_err(dev, "Failed to initialize msi!\n");
		return ret;
	}

	ret = platform_get_irq_byname(pdev, "msi");
	if (ret <= 0) {
		dev_err(dev, "failed to get MSI irq\n");
		return ret;
	}
	pcie->msi_irq = ret;

	// 初始化一把 lock，这把锁会用于 bitmap_find_free_region/bitmap_release_region
	// FIMXE? why 需要这把锁？
	raw_spin_lock_init(&pcie->lock);

	// FIXME，这个逻辑感觉是多余的，从原代码的逻辑看，这里保存了 sg2042_pcie_msi_bottom_irq_chip
	// 的地址，然后在 sg2042_pcie_irq_domain_alloc 中调用 irq_domain_set_info
	// 的时候使用，但是 sg2042_pcie_msi_bottom_irq_chip 本身是全局变量
	// 在 irq_domain_set_info 使用的时候直接引用就好了，不需要转一下。
	// 不过参考 drivers/pci/controller/dwc/pcie-designware-host.c 里还是存放了这个，看了一下也是感觉多余
	pcie->msi_irq_chip = &sg2042_pcie_msi_bottom_irq_chip;

	ret = sg2042_pcie_allocate_domains(pcie);
	if (ret)
		return ret;

	if (pcie->msi_irq)
		irq_set_chained_handler_and_data(pcie->msi_irq,
						 sg2042_pcie_chained_msi_isr, pcie);

	return 0;
}

static u64 sg2042_pcie_cpu_addr_fixup(struct cdns_pcie *pcie, u64 cpu_addr)
{
	return cpu_addr & CDNS_PLAT_CPU_TO_BUS_ADDR;
}

static const struct cdns_pcie_ops sg2042_pcie_ops = {
	.cpu_addr_fixup = sg2042_pcie_cpu_addr_fixup,
};

/*
 * SG2042 only support 4-byte aligned access, so for the rootbus (i.e. to read
 * the PCIe controller itself, read32 is required. For non-rootbus (i.e. to read
 * the PCIe peripheral registers, supports 1/2/4 byte aligned access, so
 * directly use read should be fine.
 * The same is true for write.
 */
static int sg2042_pcie_config_read(struct pci_bus *bus, unsigned int devfn,
				   int where, int size, u32 *value)
{
	if (pci_is_root_bus(bus))
		return pci_generic_config_read32(bus, devfn, where, size,
						 value);

	return pci_generic_config_read(bus, devfn, where, size, value);
}

static int sg2042_pcie_config_write(struct pci_bus *bus, unsigned int devfn,
				    int where, int size, u32 value)
{
	if (pci_is_root_bus(bus))
		return pci_generic_config_write32(bus, devfn, where, size,
						  value);

	return pci_generic_config_write(bus, devfn, where, size, value);
}

static struct pci_ops sg2042_pcie_host_ops = {
	.map_bus	= cdns_pci_map_bus,
	.read		= sg2042_pcie_config_read,
	.write		= sg2042_pcie_config_write,
};

static const struct of_device_id sg2042_pcie_of_match[] = {
	{ .compatible = "sophgo,cdns-pcie-host" },
	{},
};

static int sg2042_pcie_host_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct pci_host_bridge *bridge;
	struct device_node *np_syscon;
	struct cdns_pcie *cdns_pcie;
	struct sg2042_pcie *pcie;
	struct cdns_pcie_rc *rc;
	struct regmap *syscon;
	int ret;

	pcie = devm_kzalloc(dev, sizeof(*pcie), GFP_KERNEL);
	if (!pcie)
		return -ENOMEM;

	if (!IS_ENABLED(CONFIG_PCIE_CADENCE_HOST))
		return -ENODEV;

	bridge = devm_pci_alloc_host_bridge(dev, sizeof(*rc));
	if (!bridge)
		return -ENOMEM;
	bridge->ops = &sg2042_pcie_host_ops;
	rc = pci_host_bridge_priv(bridge);

	cdns_pcie = &rc->pcie;
	cdns_pcie->dev = dev;
	cdns_pcie->ops = &sg2042_pcie_ops;
	pcie->cdns_pcie = cdns_pcie;

	np_syscon = of_parse_phandle(np, "pcie-syscon", 0);
	if (!np_syscon) {
		dev_err(dev, "Failed to get pcie-syscon node\n");
		return -ENOMEM;
	}
	syscon = syscon_node_to_regmap(np_syscon);
	if (IS_ERR(syscon)) {
		dev_err(dev, "Failed to get regmap for pcie-syscon\n");
		return -ENOMEM;
	}
	pcie->syscon = syscon;

	of_property_read_u32(np, "pcie-id", &pcie->pcie_id);
	of_property_read_u32(np, "link-id", &pcie->link_id);
	of_property_read_u32(np, "top-intc-used", &pcie->top_intc_used);

	platform_set_drvdata(pdev, pcie);

	pm_runtime_enable(dev);
	ret = pm_runtime_get_sync(dev);
	if (ret < 0) {
		dev_err(dev, "pm_runtime_get_sync failed\n");
		goto err_get_sync;
	}

	if (pcie->top_intc_used == 1) {
		ret = sg2042_pcie_setup_top_intc(pcie);
		if (ret < 0)
			goto err_pcie_setup;
	} else {
		ret = sg2042_pcie_setup_msi(pcie, pdev);
		if (ret < 0)
			goto err_sg2042_pcie_setup_msi;
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

 err_sg2042_pcie_setup_msi:
	sg2042_pcie_free_msi(pcie);

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
		.of_match_table = sg2042_pcie_of_match,
		.pm = &cdns_pcie_pm_ops,
	},
	.probe = sg2042_pcie_host_probe,
	.shutdown = cdns_pcie_shutdown,
};
builtin_platform_driver(cdns_pcie_host_driver);
