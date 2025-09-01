// SPDX-License-Identifier: GPL-2.0
/*
 * pcie-sg2042 - PCIe controller driver for Sophgo SG2042 SoC
 *
 * Copyright (C) 2025 Sophgo Technology Inc.
 * Copyright (C) 2025 Chen Wang <unicorn_wang@outlook.com>
 */

#include <linux/mod_devicetable.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

#include "pcie-cadence.h"

/*
 * SG2042 only supports 4-byte aligned access, so for the rootbus (i.e. to
 * read/write the Root Port itself, read32/write32 is required. For
 * non-rootbus (i.e. to read/write the PCIe peripheral registers, supports
 * 1/2/4 byte aligned access, so directly using read/write should be fine.
 */

static struct pci_ops sg2042_pcie_root_ops = {
	.map_bus	= cdns_pci_map_bus,
	.read		= pci_generic_config_read32,
	.write		= pci_generic_config_write32,
};

static struct pci_ops sg2042_pcie_child_ops = {
	.map_bus	= cdns_pci_map_bus,
	.read		= pci_generic_config_read,
	.write		= pci_generic_config_write,
};

static int sg2042_pcie_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pci_host_bridge *bridge;
	struct cdns_pcie *pcie;
	struct cdns_pcie_rc *rc;
	int ret;

	bridge = devm_pci_alloc_host_bridge(dev, sizeof(*rc));
	if (!bridge) {
		dev_err(dev, "Failed to alloc host bridge!\n");
		return -ENOMEM;
	}

	bridge->ops = &sg2042_pcie_root_ops;
	bridge->child_ops = &sg2042_pcie_child_ops;

	rc = pci_host_bridge_priv(bridge);
	pcie = &rc->pcie;
	pcie->dev = dev;

	platform_set_drvdata(pdev, pcie);

	pm_runtime_enable(dev);

	ret = pm_runtime_get_sync(dev);
	if (ret < 0) {
		dev_err(dev, "pm_runtime_get_sync failed\n");
		goto err_get_sync;
	}

	ret = cdns_pcie_init_phy(dev, pcie);
	if (ret) {
		dev_err(dev, "Failed to init phy!\n");
		goto err_get_sync;
	}

	ret = cdns_pcie_host_setup(rc);
	if (ret < 0) {
		dev_err(dev, "Failed to setup host!\n");
		goto err_host_setup;
	}

	return 0;

err_host_setup:
	cdns_pcie_disable_phy(pcie);

err_get_sync:
	pm_runtime_put(dev);
	pm_runtime_disable(dev);

	return ret;
}

static void sg2042_pcie_shutdown(struct platform_device *pdev)
{
	struct cdns_pcie *pcie = platform_get_drvdata(pdev);
	struct device *dev = &pdev->dev;

	cdns_pcie_disable_phy(pcie);

	pm_runtime_put(dev);
	pm_runtime_disable(dev);
}

static const struct of_device_id sg2042_pcie_of_match[] = {
	{ .compatible = "sophgo,sg2042-pcie-host" },
	{},
};

static struct platform_driver sg2042_pcie_driver = {
	.driver = {
		.name		= "sg2042-pcie",
		.of_match_table	= sg2042_pcie_of_match,
		.pm		= &cdns_pcie_pm_ops,
	},
	.probe		= sg2042_pcie_probe,
	.shutdown	= sg2042_pcie_shutdown,
};
builtin_platform_driver(sg2042_pcie_driver);
