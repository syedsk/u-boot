/*
 * Clock drivers for Qualcomm APQ8016
 *
 * (C) Copyright 2015 Mateusz Kulikowski <mateusz.kulikowski@gmail.com>
 *
 * Based on Little Kernel driver, simplified
 *
 * SPDX-License-Identifier:	BSD-3-Clause
 */

#include <asm/io.h>
#include <dm.h>
#include <clk.h>
#include <common.h>
#include <errno.h>
#include <linux/bitops.h>

/* GPLL0 clock control registers */
#define GPLL0_STATUS        0x2101C
#define GPLL0_STATUS_ACTIVE BIT(17)

#define APCS_GPLL_ENA_VOTE  0x45000
#define APCS_GPLL_ENA_VOTE_GPLL0 BIT(0)

/* vote reg for blsp1 clock */
#define APCS_CLOCK_BRANCH_ENA_VOTE  0x45004
#define APCS_CLOCK_BRANCH_ENA_VOTE_BLSP1 BIT(10)

/* SDC(n) clock control registers; n=1,2 */

/* block reset*/
#define SDCC_BCR(n)                 (((n*0x1000)) + 0x41000)
/* cmd */
#define SDCC_CMD_RCGR(n)            (((n*0x1000)) + 0x41004)
/* cfg */
#define SDCC_CFG_RCGR(n)            (((n*0x1000)) + 0x41008)
/* m */
#define SDCC_M(n)                   (((n*0x1000)) + 0x4100C)
/* n */
#define SDCC_N(n)                   (((n*0x1000)) + 0x41010)
/* d */
#define SDCC_D(n)                   (((n*0x1000)) + 0x41014)
/* branch control */
#define SDCC_APPS_CBCR(n)           (((n*0x1000)) + 0x41018)
#define SDCC_AHB_CBCR(n)            (((n*0x1000)) + 0x4101C)

/* BLSP1 AHB clock (root clock for BLSP) */
#define BLSP1_AHB_CBCR              0x1008

/* Uart clock control registers */
#define BLSP1_UART2_BCR             0x3028
#define BLSP1_UART2_APPS_CBCR       0x302C
#define BLSP1_UART2_APPS_CMD_RCGR   0x3034
#define BLSP1_UART2_APPS_CFG_RCGR   0x3038
#define BLSP1_UART2_APPS_M          0x303C
#define BLSP1_UART2_APPS_N          0x3040
#define BLSP1_UART2_APPS_D          0x3044

/* CBCR register fields */
#define CBCR_BRANCH_ENABLE_BIT  BIT(0)
#define CBCR_BRANCH_OFF_BIT     BIT(31)

struct msm_clk_priv {
	void *base;
};

/* Enable clock controlled by CBC soft macro */
static void clk_enable_cbc(void *cbcr)
{
	uint32_t val = readl(cbcr);
	val |= CBCR_BRANCH_ENABLE_BIT;
	writel(val, cbcr);

	while (readl(cbcr) & CBCR_BRANCH_OFF_BIT)
		;
}

/* clock has 800MHz */
static void clk_enable_gpll0(void *base)
{
	uint32_t ena;

	if (readl(base + GPLL0_STATUS) & GPLL0_STATUS_ACTIVE)
		return; /* clock already enabled */

	ena = readl(base + APCS_GPLL_ENA_VOTE);
	ena |= APCS_GPLL_ENA_VOTE_GPLL0;
	writel(ena, base + APCS_GPLL_ENA_VOTE);

	while ((readl(base + GPLL0_STATUS) & GPLL0_STATUS_ACTIVE) == 0)
		;
}

#define APPS_CMD_RGCR_UPDATE BIT(0)

/* Update clock command via CMD_RGCR */
static void clk_bcr_update(void *apps_cmd_rgcr)
{
	uint32_t cmd;

	cmd  = readl(apps_cmd_rgcr);
	cmd |= APPS_CMD_RGCR_UPDATE;
	writel(cmd, apps_cmd_rgcr);

	/* Wait for frequency to be updated. */
	while (readl(apps_cmd_rgcr) & APPS_CMD_RGCR_UPDATE)
		;
}

struct bcr_regs {
	uintptr_t cfg_rcgr;
	uintptr_t cmd_rcgr;
	uintptr_t M;
	uintptr_t N;
	uintptr_t D;
};

/* RCGR_CFG register fields */
#define CFG_MODE_DUAL_EDGE (0x2 << 12) /* Counter mode */

/* sources */
#define CFG_CLK_SRC_CXO   (0 << 8)
#define CFG_CLK_SRC_GPLL0 (1 << 8)
#define CFG_CLK_SRC_MASK  (7 << 8)

/* Mask for supported fields */
#define CFG_MASK 0x3FFF

#define BM(msb, lsb)     (((((uint32_t)-1) << (31-msb)) >> (31-msb+lsb)) << lsb)
#define BVAL(msb, lsb, val) (((val) << lsb) & BM(msb, lsb))

/* root set rate for clocks with half integer and MND divider */
static void clk_rcg_set_rate_mnd(void *base, const struct bcr_regs *regs,
				 int div, int m, int n, int source)
{
	uint32_t cfg;
	/* This register houses the M value for MND divider. */
	uint32_t m_val = m;
	/* This register houses the NOT(N-M) value for MND divider. */
	uint32_t n_val = ~((n)-(m)) * !!(n);
	/* This register houses the NOT 2D value for MND divider. */
	uint32_t d_val = ~(n);

	/* Program MND values */
	writel(m_val, base + regs->M); /* M */
	writel(n_val, base + regs->N); /* N */
	writel(d_val, base + regs->D); /* D */

	/* setup src select and divider */
	cfg  = readl(base + regs->cfg_rcgr);
	cfg &= ~CFG_MASK;
	cfg |= source & CFG_CLK_SRC_MASK; /* Select clock source */
	cfg |= BVAL(4, 0, (int)(2*(div) - 1))  | BVAL(10, 8, source);
	if (n_val)
		cfg |= CFG_MODE_DUAL_EDGE;

	writel(cfg, base + regs->cfg_rcgr); /* Write new clock configuration */

	/* Inform h/w to start using the new config. */
	clk_bcr_update(base + regs->cmd_rcgr);
}

static const struct bcr_regs sdc_regs[] = {
	{
	.cfg_rcgr = SDCC_CFG_RCGR(1),
	.cmd_rcgr = SDCC_CMD_RCGR(1),
	.M = SDCC_M(1),
	.N = SDCC_N(1),
	.D = SDCC_D(1),
	},
	{
	.cfg_rcgr = SDCC_CFG_RCGR(2),
	.cmd_rcgr = SDCC_CMD_RCGR(2),
	.M = SDCC_M(2),
	.N = SDCC_N(2),
	.D = SDCC_D(2),
	}
};

/* Init clock for SDHCI controller */
static int clk_init_sdc(struct msm_clk_priv *p, int slot, uint rate)
{
	int div = 8; /* 100MHz default */

	if (rate == 200000000)
		div = 4;

	clk_enable_cbc(p->base + SDCC_AHB_CBCR(slot));
	/* 800Mhz/div, gpll0 */
	clk_rcg_set_rate_mnd(p->base, &sdc_regs[slot], div, 0, 0,
			     CFG_CLK_SRC_GPLL0);
	clk_enable_gpll0(p->base);
	clk_enable_cbc(p->base + SDCC_APPS_CBCR(slot));
	return rate;
}

static const struct bcr_regs uart2_regs = {
	.cfg_rcgr = BLSP1_UART2_APPS_CFG_RCGR,
	.cmd_rcgr = BLSP1_UART2_APPS_CMD_RCGR,
	.M = BLSP1_UART2_APPS_M,
	.N = BLSP1_UART2_APPS_N,
	.D = BLSP1_UART2_APPS_D,
};

/* Init UART clock, 115200 */
static int clk_init_uart(struct msm_clk_priv *p)
{
	/* Enable iface clk */
	clk_enable_cbc(p->base + BLSP1_AHB_CBCR);
	/* 7372800 uart block clock @ GPLL0 */
	clk_rcg_set_rate_mnd(p->base, &uart2_regs, 1, 144, 15625,
			     CFG_CLK_SRC_GPLL0);
	clk_enable_gpll0(p->base);
	/* Enable core clk */
	clk_enable_cbc(p->base + BLSP1_UART2_APPS_CBCR);
	return 0;
}

ulong msm_set_periph_rate(struct udevice *dev, int periph, ulong rate)
{
	struct msm_clk_priv *priv = dev_get_priv(dev);

	switch (periph) {
	case 0: /* SDC1 */
		return clk_init_sdc(priv, 0, rate);
		break;
	case 1: /* SDC2 */
		return clk_init_sdc(priv, 1, rate);
		break;
	case 4: /* UART2 */
		return clk_init_uart(priv);
		break;
	default:
		return 0;
	}
	return 0;
}

static int msm_clk_probe(struct udevice *dev)
{
	struct msm_clk_priv *priv = dev_get_priv(dev);
	priv->base = (void *)dev_get_addr(dev);
	return 0;
}

static struct clk_ops msm_clk_ops = {
	.set_periph_rate = msm_set_periph_rate,
};

static const struct udevice_id msm_clk_ids[] = {
	{ .compatible = "qcom,gcc-msm8916" },
	{ }
};

U_BOOT_DRIVER(clk_msm) = {
	.name		= "clk_msm",
	.id		= UCLASS_CLK,
	.of_match	= msm_clk_ids,
	.ops		= &msm_clk_ops,
	.priv_auto_alloc_size = sizeof(struct msm_clk_priv),
	.probe		= msm_clk_probe,
};