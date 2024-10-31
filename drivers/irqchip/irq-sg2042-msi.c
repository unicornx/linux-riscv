#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/msi.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/of_pci.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

struct pch_msi_data {
	void __iomem *reg_sta; /* status reg, see TRM, 10.1.31, GP_INTR_REGISTER_0 */
	void __iomem *reg_set; /* set reg, see TRM, 10.1.32, GP_INTR0_SET */
	void __iomem *reg_clr; /* clear reg, see TRM, 10.1.33, GP_INTR0_CLR */

	int reg_bitwidth;

	struct mutex	msi_map_lock;
	phys_addr_t	doorbell;
	u32		irq_first;	/* The vector number that MSIs starts */
	u32		num_irqs;	/* The number of vectors for MSIs */
	unsigned long	*msi_map;
};

static int pch_msi_allocate_hwirq(struct pch_msi_data *priv, int num_req)
{
	int first;

	mutex_lock(&priv->msi_map_lock);

	first = bitmap_find_free_region(priv->msi_map, priv->num_irqs,
					get_count_order(num_req));
	if (first < 0) {
		mutex_unlock(&priv->msi_map_lock);
		return -ENOSPC;
	}

	mutex_unlock(&priv->msi_map_lock);

	return priv->irq_first + first;
}

static void pch_msi_free_hwirq(struct pch_msi_data *priv,
				int hwirq, int num_req)
{
	int first = hwirq - priv->irq_first;

	mutex_lock(&priv->msi_map_lock);
	bitmap_release_region(priv->msi_map, first, get_count_order(num_req));
	mutex_unlock(&priv->msi_map_lock);
}

static void pch_msi_ack(struct irq_data *d)
{
	struct pch_msi_data *data  = irq_data_get_irq_chip_data(d);
	int bit_off = d->hwirq - data->irq_first;

	writel(1 << bit_off, (unsigned int *)data->reg_clr);

	irq_chip_ack_parent(d);
}

static void pch_msi_compose_msi_msg(struct irq_data *data,
				    struct msi_msg *msg)
{
	struct pch_msi_data *priv = irq_data_get_irq_chip_data(data);

	msg->address_hi = upper_32_bits(priv->doorbell);
	msg->address_lo = lower_32_bits(priv->doorbell);
	msg->data = 1 << (data->hwirq - priv->irq_first);

	pr_info("----> %s hwirq[%d]: address_hi[%#x], address_lo[%#x], data[%#x]\n",
		__func__,
		(int)data->hwirq, msg->address_hi, msg->address_lo, msg->data);
}

static struct irq_chip middle_irq_chip = {
	.name			= "PCH MSI",
	.irq_ack		= pch_msi_ack,
	.irq_mask		= irq_chip_mask_parent,
	.irq_unmask		= irq_chip_unmask_parent,
	.irq_set_affinity	= irq_chip_set_affinity_parent,
	.irq_compose_msi_msg	= pch_msi_compose_msi_msg,
};

static int pch_msi_parent_domain_alloc(struct irq_domain *domain,
					unsigned int virq, int hwirq)
{
	struct irq_fwspec fwspec;
	struct irq_data *d;
	int ret;

	fwspec.fwnode = domain->parent->fwnode;
	fwspec.param_count = 2;
	fwspec.param[0] = hwirq;
	fwspec.param[1] = IRQ_TYPE_EDGE_RISING;

	ret = irq_domain_alloc_irqs_parent(domain, virq, 1, &fwspec);
	if (ret)
		return ret;

	d = irq_domain_get_irq_data(domain->parent, virq);
	return d->chip->irq_set_type(d, IRQ_TYPE_EDGE_RISING);
}

static int pch_msi_middle_domain_alloc(struct irq_domain *domain,
					   unsigned int virq,
					   unsigned int nr_irqs, void *args)
{
	struct pch_msi_data *priv = domain->host_data;
	int hwirq, err, i;

	hwirq = pch_msi_allocate_hwirq(priv, nr_irqs);
	if (hwirq < 0)
		return hwirq;

	for (i = 0; i < nr_irqs; i++) {
		err = pch_msi_parent_domain_alloc(domain, virq + i, hwirq + i);
		if (err)
			goto err_hwirq;
		
		pr_info("----> pch_msi_middle_domain_alloc: virq[%d], hwirq[%d]\n",
			virq + i, (int)hwirq + i);

		irq_domain_set_hwirq_and_chip(domain, virq + i, hwirq + i,
					      &middle_irq_chip, priv);
	}

	return 0;

err_hwirq:
	pch_msi_free_hwirq(priv, hwirq, nr_irqs);
	irq_domain_free_irqs_parent(domain, virq, i);

	return err;
}

static void pch_msi_middle_domain_free(struct irq_domain *domain,
					   unsigned int virq,
					   unsigned int nr_irqs)
{
	struct irq_data *d = irq_domain_get_irq_data(domain, virq);
	struct pch_msi_data *priv = irq_data_get_irq_chip_data(d);

	irq_domain_free_irqs_parent(domain, virq, nr_irqs);
	pch_msi_free_hwirq(priv, d->hwirq, nr_irqs);
}

static const struct irq_domain_ops pch_msi_middle_domain_ops = {
	.alloc	= pch_msi_middle_domain_alloc,
	.free	= pch_msi_middle_domain_free,
};

static int pch_msi_init_domains(struct pch_msi_data *priv,
				struct device_node *node)
{
	struct irq_domain *plic_domain, *middle_domain;
	struct device_node *plic_node;
	struct fwnode_handle *fwnode = of_node_to_fwnode(node);

	if (!of_find_property(node, "interrupt-parent", NULL)) {
		pr_err("Can't find interrupt-parent!\n");
		return -EINVAL;
	}

	plic_node = of_irq_find_parent(node);
	if (!plic_node) {
		pr_err("Failed to find the PLIC node!\n");
		return -ENXIO;
	}

	plic_domain = irq_find_host(plic_node);
	of_node_put(plic_node);
	if (!plic_domain) {
		pr_err("Failed to find the PLIC domain\n");
		return -ENXIO;
	}

	middle_domain = irq_domain_create_hierarchy(plic_domain, 0, priv->num_irqs,
						    fwnode,
						    &pch_msi_middle_domain_ops,
						    priv);
	if (!middle_domain) {
		pr_err("Failed to create the MSI middle domain\n");
		return -ENOMEM;
	}

	return 0;
}

static int top_intc_probe(struct platform_device *pdev)
{
	struct pch_msi_data *data;
	struct resource *res;

	data = devm_kzalloc(&pdev->dev, sizeof(struct pch_msi_data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	if (device_property_read_u32(&pdev->dev, "reg-bitwidth", &data->reg_bitwidth))
		data->reg_bitwidth = 32;

	data->reg_sta = devm_platform_ioremap_resource_byname(pdev, "sta");
	if (IS_ERR(data->reg_sta)) {
		dev_err(&pdev->dev, "Failed to map status register\n");
		return PTR_ERR(data->reg_sta);
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "set");
	data->reg_set = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(data->reg_set)) {
		dev_err(&pdev->dev, "Failed map set register\n");
		return PTR_ERR(data->reg_set);
	}
	data->doorbell = res->start;

	data->reg_clr = devm_platform_ioremap_resource_byname(pdev, "clr");
	if (IS_ERR(data->reg_clr)) {
		dev_err(&pdev->dev, "Failed to map clear register\n");
		return PTR_ERR(data->reg_clr);
	}

	data->irq_first = 64;
	data->num_irqs = 32;

	mutex_init(&data->msi_map_lock);

	data->msi_map = bitmap_zalloc(data->num_irqs, GFP_KERNEL);
	if (!data->msi_map)
		return -ENOMEM;

	return pch_msi_init_domains(data, pdev->dev.of_node);
}

static const struct of_device_id top_intc_of_match[] = {
	{ .compatible = "sophgo,top-intc" },
	{}
};

static struct platform_driver top_intc_driver = {
	.driver = {
		.name = "sophgo,top-intc",
		.of_match_table = of_match_ptr(top_intc_of_match),
	},
	.probe = top_intc_probe,
};
builtin_platform_driver(top_intc_driver);
