/*
 * Qualcomm SDHCI driver - SD/eMMC controller
 *
 * (C) Copyright 2015 Mateusz Kulikowski <mateusz.kulikowski@gmail.com>
 *
 * Based on Linux driver
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */

#include <asm/io.h>
#include <common.h>
#include <dm.h>
#include <linux/bitops.h>
#include <sdhci.h>
#include <clk.h>

/* Non-standard registers needed for SDHCI startup */
#define SDCC_MCI_POWER   0x0
#define SDCC_MCI_POWER_SW_RST BIT(7)

/* This is undocumented register */
#define SDCC_MCI_VERSION             0x50
#define SDCC_MCI_VERSION_MAJOR_SHIFT 28
#define SDCC_MCI_VERSION_MAJOR_MASK  (0xf << SDCC_MCI_VERSION_MAJOR_SHIFT)
#define SDCC_MCI_VERSION_MINOR_MASK  0xff

#define SDCC_MCI_HC_MODE 0x78

/* Offset to SDHCI registers */
#define SDCC_SDHCI_OFFSET 0x900

/* Non standard (?) SDHCI register */
#define SDHCI_VENDOR_SPEC_CAPABILITIES0  0x11c

struct msm_sdhc {
	struct sdhci_host host;
	phys_addr_t base;
	unsigned width;
};

DECLARE_GLOBAL_DATA_PTR;

static int msm_sdc_clk_init(struct udevice *dev)
{
	uint clk_rate = fdtdec_get_uint(gd->fdt_blob, dev->of_offset,
					"clock-frequency", 400000);
	uint clkd[2]; /* clk_id and clk_no */
	fdtdec_get_int_array(gd->fdt_blob, dev->of_offset, "clock", clkd, 2);
	clkd[0] = fdt_node_offset_by_phandle(gd->fdt_blob, clkd[0]);

	struct udevice *clk = NULL;
	uclass_get_device_by_of_offset(UCLASS_CLK, clkd[0], &clk);
	if (clk)
		clk_set_periph_rate(clk, clkd[1], clk_rate);

	return 0;
}

static int msm_sdc_probe(struct udevice *dev)
{
	struct msm_sdhc *prv = dev_get_priv(dev);
	struct sdhci_host *host = &prv->host;
	u32 core_version, core_minor, core_major;

	host->quirks = SDHCI_QUIRK_WAIT_SEND_CMD | SDHCI_QUIRK_BROKEN_R1B;

	/* Init clocks */
	if (msm_sdc_clk_init(dev))
		return -EIO;

	/* Reset the core and Enable SDHC mode */
	writel(readl(prv->base + SDCC_MCI_POWER) | SDCC_MCI_POWER_SW_RST,
	       prv->base + SDCC_MCI_POWER);

	/* SW reset can take upto 10HCLK + 15MCLK cycles. (min 40us) */
	mdelay(2);

	if (readl(prv->base + SDCC_MCI_POWER) & SDCC_MCI_POWER_SW_RST) {
		printf("msm_sdhci: stuck in reset\n");
		return -1;
	}

	/* Enable host-controller mode */
	writel(1, prv->base + SDCC_MCI_HC_MODE);

	core_version = readl(prv->base + SDCC_MCI_VERSION);

	core_major = (core_version & SDCC_MCI_VERSION_MAJOR_MASK);
	core_major >>= SDCC_MCI_VERSION_MAJOR_SHIFT;

	core_minor = core_version & SDCC_MCI_VERSION_MINOR_MASK;

	/*
	 * Support for some capabilities is not advertised by newer
	 * controller versions and must be explicitly enabled.
	 */
	if (core_major >= 1 && core_minor != 0x11 && core_minor != 0x12) {
		u32 caps = readl(host->ioaddr + SDHCI_CAPABILITIES);
		caps |= SDHCI_CAN_VDD_300 | SDHCI_CAN_DO_8BIT;
		writel(caps, host->ioaddr + SDHCI_VENDOR_SPEC_CAPABILITIES0);
	}

	/* Set host controller version */
	host->version = sdhci_readw(host, SDHCI_HOST_VERSION);

	/* automatically detect max and min speed */
	return add_sdhci(host, 0, 0);
}

static int msm_sdc_remove(struct udevice *dev)
{
	struct msm_sdhc *priv = dev_get_priv(dev);
	 /* Disable host-controller mode */
	writel(0, priv->base + SDCC_MCI_HC_MODE);
	return 0;
}

static int msm_ofdata_to_platdata(struct udevice *dev)
{
	struct msm_sdhc *priv = dev_get_priv(dev);
	struct sdhci_host *host = &priv->host;

	host->name = strdup(dev->name);
	host->ioaddr = (void *)dev_get_addr(dev);
	host->bus_width = fdtdec_get_int(gd->fdt_blob, dev->of_offset,
					 "bus-width", 4);
	host->index = fdtdec_get_uint(gd->fdt_blob, dev->of_offset, "index", 0);
	priv->base = fdtdec_get_addr_size_auto_parent(gd->fdt_blob,
						      dev->parent->of_offset,
						      dev->of_offset, "reg",
						      1, NULL);
	return 0;
}

static const struct udevice_id msm_mmc_ids[] = {
	{ .compatible = "qcom,sdhci-msm-v4" },
	{ }
};

U_BOOT_DRIVER(msm_sdc_drv) = {
	.name		= "msm_sdc",
	.id		= UCLASS_MMC,
	.of_match	= msm_mmc_ids,
	.ofdata_to_platdata = msm_ofdata_to_platdata,
	.probe		= msm_sdc_probe,
	.remove		= msm_sdc_remove,
	.priv_auto_alloc_size = sizeof(struct msm_sdhc),
};