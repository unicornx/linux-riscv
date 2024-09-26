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


// dw 的设计
// 1）dw_pcie_msi_host_init 中通过 dmam_alloc_coherent 分配的只是一个 64 bit 的数据区
// 2）PCIe 设备插入后，触发 dw_pcie_irq_domain_alloc（domain.alloc） 中的实现：
//    根据传入的 virq 和 范围，在 bitmap 中找到一块连续的 area 并将其映射设置到 domain 中
//    映射通过 irq_domain_set_info 完成，最终完成：bitmap bit <-> virq（IRQ#), domain 中的 hwirq 值就是 bitmap bit
// 3）还是在 PCIe 设备上电出发的初始化过程中
//    RC 驱动调用 dw_pci_setup_msi_msg (chip.irq_compose_msi_msg）中将该 8 字节的首地址和 hwirq 组装 msg
//    RC 在设备的 msi capability 中的写入：
//    message address 记录的是这个 8 字节首地址
//    message data 记录的是该设备的 hwirq（即 bitmap 中的 bit）

// 中断这里：
// 1) 先尝试通过 DTS 获取 interrupt source 和 IRQ#
//    正常应该会通过 interrupt-names msi0/msi1/... 最多 8 个。
//    一个 IRQ# 对应一个 msi_irq, 即 msi_irq[MAX_MSI_CTRLS]， 这个数组也是 8 个
//    如果 DTS 中没有 msiX，则默认会退回到一个 IRQ#
//    dw 的 PCIe RC 支持 mulitple 方式的 MSI 中断源，这个可以在其 dw_pcie_msi_domain_info.flags
//    设置中可以看出来，其设置了 MSI_FLAG_MULTI_PCI_MSI。最多 8 路 upstream 中断源
// 2) 通过 irq_set_chained_handler_and_data 为每个 IRQ# 设置 irq handler 为 dw_chained_msi_isr
//    也就是说任一路进来的处理函数都是同一个。
// 3）设备中断发生时，会根据 RC 设置的 messaage addr 和 message data 触发 MSI。
// 4）进入 dw_chained_msi_isr 处理 -> dw_handle_msi_irq
//    因为支持多路中断源，每一路对应一个 ctrl，所以需要对每一个 ctrl 进行检查
//    ctrl 是 PCI 控制器上的一组寄存器 block，首地址为 PCIE_MSI_INTR0_STATUS，每个 block size 是 MSI_REG_CTRL_BLOCK_SIZE
//    针对每一个 reg block，如果读出来其值 status 不为 0，说明该 reg block 管理的多路 hw irq 中
//    有中断发生，所以继续检查该 status 的对应位。对每一个为 1 的 位调用
//    generic_handle_domain_irq(pp->irq_domain,  (i * MAX_MSI_IRQS_PER_CTRL) + pos);
//    注意这里第二个参数即 hwirq。因为我们在 domain.alloc 中的实现 已经 建立了 bit <-> virq 的映射
//    所以这里传入的第一个参数中的 irq_domain 会帮助我们翻译这个映射，将 hwirq（bitmap bit）转化为
//    virq(IRQ#)

// 我们的流程，以 linuxboot 启动过程中显卡作为 PCIe 设备的例子
// 1）PCI 控制器 RC 初始化
//    cdns_pcie_msi_init 中分配一个 512 * 8 字节的连续内存
// 2）radeon 显卡插入后的 probe 触发 
//    radeon_pci_probe -> ... radeon_irq_kms_init -> pci_enable_msi -> ... 
//    -> __msi_domain_alloc_irqs -> ... -> irq_domain_alloc_irqs_parent 
//    -> (.alloc)cdns_pcie_irq_domain_alloc
//    根据传入的 virq 和 范围，在 bitmap 中找到一块连续的 area 并将其映射设置到 domain 中
//    映射通过 irq_domain_set_info 完成，最终完成：bitmap bit <-> virq（IRQ#), domain 中的 hwirq 值就是 bitmap bit
// 3) radeon_pci_probe -> ... radeon_irq_kms_init -> pci_enable_msi -> ... 
//    -> __msi_domain_alloc_irqs -> irq_domain_activate_irq -> ... 
//    -> irq_chip_compose_msi_msg -> (.alloc)cdns_pci_setup_msi_msg
//    cdns_pci_setup_msi_msg 组装将被发送给设备的 msg
//    message address 是 基地址 + hwirq * 4
//    message data 是 1
//    同时记录把此时的 hwirq 记录下来到 pcie->num_applied_vecs
// 4) 在处理完 cdns_pci_setup_msi_msg 后，会将组装好的 msg 再通过 pci write 写入显卡设备的 msi capability 中
//    msg 中包含 addr 信息和 data 信息
//
// 中断处理流程:
// 1) sg2042_pcie_host_probe 首先根据 DTS （根据 interrupt-names = "msi";）获取了 PLIC 的中断源对应的 IRQ#，
//    我们只有一个中断源，所以对应的 cdns_pcie_msi_domain_info.flags 中不能设置 MSI_FLAG_MULTI_PCI_MSI
// 2）cdns_pcie_msi_setup 中通过 irq_set_chained_handler_and_data 对该 IRQ# 设置了 high level hander 为 cdns_chained_msi_isr
// 3) 设备下次上报 MSI 中断时，会根据 msi capability 中的 addr 和 data 信息，往该 addr 地址写入 data 信息，这会
//    触发 PCI 控制器的 RC 通过中断源向 PLIC 上报中断。所有的 MSI 都走一条中断源上报给 CPU， 触发 cdns_chained_msi_isr
// 4）cdns_chained_msi_isr 的处理逻辑：
//    首先读取 PCIe reg810 获取状态，不为0 说明有中断发生
//    先清中断： FIXME：为啥先清中断？
//    -> cdns_handle_msi_irq
// 5) cdns_handle_msi_irq 逻辑：
//    按照 pcie->num_applied_vecs 进行循环，FIXME：这个很奇怪，这个值是设备插入时记录的 hwirq，在中断发生时如果中间还有新的
//    设备插入 slot，这个值是会发生变化的。
//    读取连续内存的内容，针对不为 0 的 bit 进行处理，
//    逻辑比较怪异，FIXME, 为啥要多一步 irq_find_mapping，如果按照 dw 的做法，得到 bitmap bit 后直接 generic_handle_domain_irq 不可以吗？
//    处理后会再来一遍，又是为何？FIXME

struct sg2042_pcie {
	struct cdns_pcie	*cdns_pcie;

	// Fully private members for sg2042
	u16			pcie_id; // dts 设置了，但是代码中用不到 FIXME
	u16			link_id;
	u32			top_intc_used;

	// msix_supported 用于决定 当 top_intc_used = 1 时是否支持 msix，
	// dts 配置如下：
	// 第一个 slot 使用 top-intc，也支持 msix
	// 第二个 slot 不使用 top-intc, 所以也没有配置 msix
	// 第三个 slot 使用 top-intc，不支持 msix
	// 在 cdns_pcie_msi_setup_for_top_intc 中区分 pci_msi_create_irq_domain 传入的 MSI domain info
	// 也就是说这个标志会影响 msi_domain 的创建
	u32			msix_supported;
	// 用于存放 pci_msi_create_irq_domain 的结果
	// 这个变量对于使用或者于不使用 top-intc 的情况都会用到
	struct irq_domain	*msi_domain;

	// 下面的成员都是和 !top-intc 处理有关
	int			msi_irq;
	struct irq_domain	*irq_domain; // FIXME：这个成员名称和结构体名字相同了，不好的编程习惯，建议改掉。
	dma_addr_t		msi_data;
	void			*msi_page;
	struct irq_chip		*msi_irq_chip;
	u32			num_vectors; // FIXME: 这个成员感觉用处不大。初始化为 MSI_DEF_NUM_VECTORS 就不变的。
	u32			num_applied_vecs;
	u32			irq_mask[MAX_MSI_CTRLS]; // FIXME：这个没用上，可以删掉
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

// 用于 !top-intc
// 分配一块 DMA 用于存放 msi data, 然后将该内存的物理地址告诉 pcie 内部算能扩展的中断控制器
// 本质上是通知给 RC。
// FIXME：具体运行时最好和 IC 人员确认一下是不是这个设计，以及数据区中是否有格式要求？

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

// top-intc 的处理代码
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

// 这个不是只针对 top-intc, !top-intc 也会用到。
static struct msi_domain_info cdns_pcie_msi_domain_info = {
	.flags	= (MSI_FLAG_USE_DEF_DOM_OPS | MSI_FLAG_USE_DEF_CHIP_OPS),
	.chip	= &cdns_pcie_msi_irq_chip,
};

static struct msi_domain_info cdns_pcie_top_intr_msi_domain_info = {
	.flags	= (MSI_FLAG_USE_DEF_DOM_OPS | MSI_FLAG_USE_DEF_CHIP_OPS
		   | MSI_FLAG_PCI_MSIX),
	.chip	= &cdns_pcie_msi_irq_chip,
};

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

// !top-intc
// 
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
			irq = irq_find_mapping(pcie->irq_domain,
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

// FIXME: 这个函数实际运行中并没有看到会被触发，但根据算能开发人员说硬件上设计了这个中断源，用于 rc 上报，所以需要保留
// 具体怎么处理还要再看看。
// 该函数的处理逻辑和 cdns_chained_msi_isr 类似，如果保留也是可以优化的。
static irqreturn_t cdns_pcie_irq_handler(int irq, void *arg)
{
	struct sg2042_pcie *pcie = arg;
	u32 status = 0;
	u32 st_msi_in_bit = 0;
	u32 clr_msi_in_bit = 0;

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

	return IRQ_HANDLED;
}

// !top-intc
// msi 逻辑会走到这里
// 核心是调用的 cdns_handle_msi_irq
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

// 这个也是参考的 drivers/pci/controller/dwc/pcie-designware-host.c
// 中的 dw_pci_msi_bottom_irq_chip
// FIXME: 大部分函数都没有实现，是否可以删掉？需要学习一下 irq_chip 的回调函数
// cdns_pci_msi_bottom_irq_chip 会在 cdns_pcie_irq_domain_alloc 中
// 也就是 irq_domain 的 .alloc 回调中传入 irq_domain_set_info
static struct irq_chip cdns_pci_msi_bottom_irq_chip = {
	.name = "CDNS-PCI-MSI",
	.irq_ack = cdns_pci_bottom_ack,
	.irq_compose_msi_msg = cdns_pci_setup_msi_msg,
	.irq_set_affinity = cdns_pci_msi_set_affinity,
	.irq_mask = cdns_pci_bottom_mask,
	.irq_unmask = cdns_pci_bottom_unmask,
};

// 以下的 cdns_pcie_msi_domain_ops 和 cdns_pcie_allocate_domains
// 参考了 drivers/pci/controller/dwc/pcie-designware-host.c
// 中的 dw_pcie_msi_domain_ops 和 dw_pcie_allocate_domains
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

// 这个函数参考了 drivers/pci/controller/dwc/pcie-designware-host.c
// 中的 dw_pcie_allocate_domains
static int cdns_pcie_allocate_domains(struct sg2042_pcie *pcie)
{
	struct device *dev = pcie->cdns_pcie->dev;
	struct fwnode_handle *fwnode = of_node_to_fwnode(dev->of_node);

	// 创建 irq domain
	// 这个 irq_domain 不仅在下面创建 msi irq domain 中作为 parent domain
	// 而且在 cdns_handle_msi_irq/cdns_pcie_free_msi 中会被使用，free 中
	// 主要负责释放
	pcie->irq_domain = irq_domain_create_linear(fwnode, pcie->num_vectors,
					       &cdns_pcie_msi_domain_ops, pcie);
	if (!pcie->irq_domain) {
		dev_err(dev, "Failed to create IRQ domain\n");
		return -ENOMEM;
	}

	irq_domain_update_bus_token(pcie->irq_domain, DOMAIN_BUS_NEXUS);

	pcie->msi_domain = pci_msi_create_irq_domain(fwnode,
						   &cdns_pcie_msi_domain_info,
						   pcie->irq_domain);
	if (!pcie->msi_domain) {
		dev_err(dev, "Failed to create MSI domain\n");
		irq_domain_remove(pcie->irq_domain);
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
	irq_domain_remove(pcie->irq_domain);

	if (pcie->msi_page)
		dma_free_coherent(dev, 1024, pcie->msi_page, pcie->msi_data);

}

// 用于 !top-intc
// 在 cdns_pcie_msi_init 后对中断做进一步初始化，FIXME，是否可以和 cdns_pcie_msi_init 合并？
static int cdns_pcie_msi_setup(struct sg2042_pcie *pcie)
{
	int ret = 0;

	// 初始化一把 lock，这把锁会用于 bitmap_find_free_region/bitmap_release_region
	// FIMXE? why 需要这把锁？
	raw_spin_lock_init(&pcie->lock);

	if (IS_ENABLED(CONFIG_PCI_MSI)) { // FIXME，这些判断分散在各处，最好统一处理，结合相关函数的优化整合
		// FIXME，这个逻辑感觉是多余的，从原代码的逻辑看，这里保存了 cdns_pci_msi_bottom_irq_chip
		// 的地址，然后在 cdns_pcie_irq_domain_alloc 中调用 irq_domain_set_info
		// 的时候使用，但是 cdns_pci_msi_bottom_irq_chip 本身是全局变量
		// 在 irq_domain_set_info 使用的时候直接引用就好了，不需要转一下。
		// 不过参考 drivers/pci/controller/dwc/pcie-designware-host.c 里还是存放了这个，看了一下也是感觉多余
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

		ret = devm_request_irq(dev, pcie->msi_irq, cdns_pcie_irq_handler,
				       IRQF_SHARED | IRQF_NO_THREAD,
				       "cdns-pcie-irq", pcie);

		if (ret) {
			dev_err(dev, "failed to request MSI irq\n");
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
