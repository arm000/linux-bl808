// SPDX-License-Identifier: GPL-2.0-only

#include <linux/clk.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/mmc/host.h>
#include <linux/module.h>
#include <linux/of.h>

#include "sdhci-pltfm.h"

static u16 sdhci_bflb_readw(struct sdhci_host *host, int reg)
{
	u16 ret;

	switch (reg) {
	case SDHCI_HOST_VERSION:
	case SDHCI_SLOT_INT_STATUS:
		/* those registers don't exist */
		return 0;
	default:
		ret = readw(host->ioaddr + reg);
	}
	return ret;
}

static u32 sdhci_bflb_readl(struct sdhci_host *host, int reg)
{
	u32 ret;

	ret = readl(host->ioaddr + reg);

	switch (reg) {
	case SDHCI_CAPABILITIES:
		/* Mask the support for 3.0V */
		ret &= ~SDHCI_CAN_VDD_300;
		break;
	}
	return ret;
}

static const struct sdhci_ops sdhci_bflb_ops = {
	.read_w	= sdhci_bflb_readw,
	.read_l	= sdhci_bflb_readl,
	.set_clock = sdhci_set_clock,
	.get_max_clock = sdhci_pltfm_clk_get_max_clock,
	.get_timeout_clock = sdhci_pltfm_clk_get_max_clock,
	.set_bus_width = sdhci_set_bus_width,
	.reset = sdhci_reset,
	.set_uhs_signaling = sdhci_set_uhs_signaling,
};

static const struct sdhci_pltfm_data sdhci_bflb_pdata = {
	.ops	= &sdhci_bflb_ops,
	.quirks	= SDHCI_QUIRK_NO_SIMULT_VDD_AND_POWER |
		  SDHCI_QUIRK_NO_BUSY_IRQ |
		  SDHCI_QUIRK_BROKEN_TIMEOUT_VAL |
		  SDHCI_QUIRK_INVERTED_WRITE_PROTECT,
};

static int sdhci_bflb_probe(struct platform_device *pdev)
{
	struct sdhci_host *host;
	struct sdhci_pltfm_host *pltfm_host;
	int ret;

	host = sdhci_pltfm_init(pdev, &sdhci_bflb_pdata, 0);
	if (IS_ERR(host))
		return PTR_ERR(host);

	pltfm_host = sdhci_priv(host);
	pltfm_host->clk = devm_clk_get(&pdev->dev, NULL);

	if (!IS_ERR(pltfm_host->clk))
		clk_prepare_enable(pltfm_host->clk);

	ret = mmc_of_parse(host->mmc);
	if (ret)
		goto err_sdhci_add;

	ret = sdhci_add_host(host);
	if (ret)
		goto err_sdhci_add;

	return 0;

err_sdhci_add:
	clk_disable_unprepare(pltfm_host->clk);
	sdhci_pltfm_free(pdev);
	return ret;
}

static const struct of_device_id sdhci_bflb_of_match_table[] = {
	{ .compatible = "bflb,bl808-sdhci", },
	{}
};
MODULE_DEVICE_TABLE(of, sdhci_bflb_of_match_table);

static struct platform_driver sdhci_bflb_driver = {
	.driver		= {
		.name	= "sdhci-bflb",
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
		.pm	= &sdhci_pltfm_pmops,
		.of_match_table = sdhci_bflb_of_match_table,
	},
	.probe		= sdhci_bflb_probe,
	.remove		= sdhci_pltfm_unregister,
};

module_platform_driver(sdhci_bflb_driver);

MODULE_DESCRIPTION("SDHCI driver for Bflb");
MODULE_LICENSE("GPL v2");
