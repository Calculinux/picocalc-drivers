// SPDX-License-Identifier: GPL-2.0
// Driver for RK3506 Cortex-M0 remoteproc
//
// Copyright (c) Viktor Nagy
// First version published at https://github.com/nvitya/rk3506-mcu
// Ported and extended for PicoCalc

#define FW_FORMAT_BIN 0

/*
 * Device-tree block:
 *
 *  mcu_rproc: mcu@fff84000 {
 *    compatible = "rockchip,rk3506-mcu";
 *    reg = <0xfff84000 0x8000>;
 *    firmware-name = "rk3506-m0-audio.elf";
 *    clocks = <&cru HCLK_M0>, <&cru STCLK_M0>;   (HCLK_M0 first; plus any
 *             peripheral clocks the firmware needs kept running)
 *    resets = <&cru SRST_H_M0>, <&cru SRST_M0_JTAG>, <&cru SRST_HRESETN_M0_AC>;
 *    reset-names = "h_m0", "m0_jtag", "hresetn_m0_ac";
 *    rockchip,tcm;                  (optional: run the image as TCM, below)
 *    picocalc,double-core-clock;    (optional: M0 at 375 MHz, below)
 *  };
 */

#include <linux/arm-smccc.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/remoteproc.h>
#include <linux/reset.h>
#include <dt-bindings/clock/rockchip,rk3506-cru.h>

extern int rproc_elf_sanity_check(struct rproc *rproc, const struct firmware *fw);
extern u64 rproc_elf_get_boot_addr(struct rproc *rproc, const struct firmware *fw);
extern int rproc_elf_load_segments(struct rproc *rproc, const struct firmware *fw);
extern int rproc_elf_load_rsc_table(struct rproc *rproc, const struct firmware *fw);
extern struct resource_table *rproc_elf_find_loaded_rsc_table(struct rproc *rproc, const struct firmware *fw);

/* Rockchip platform SiP call ID */
#define SIP_MCU_CFG 0x82000028

/* RK_SIP_MCU_CFG child configs, MCU ID */
#define ROCKCHIP_SIP_CONFIG_BUSMCU_0_ID 0x00

/* RK_SIP_MCU_CFG child configs */
#define ROCKCHIP_SIP_CONFIG_MCU_CODE_START_ADDR 0x01

#define RK3506_MCU_TCM_ADDR 0xFFF84000
#define RK3506_MCU_TCM_SIZE 0x8000

/*
 * Where the firmware image goes, and what the M0 sees at address 0.
 *
 * The Cortex-M0 has no VTOR: it fetches its vectors from address 0, and the
 * SoC's address converter maps M0 address 0 onto the "code start address"
 * given to the SiP call. Firmware is therefore linked at 0 (the core cannot
 * execute at 0xFFF8xxxx in any case: 0xE0000000 and up is execute-never on
 * ARMv6-M) and its ELF segments carry device addresses 0..size, which
 * da_to_va() places at RK3506_MCU_CODE_ADDR.
 *
 * Two modes, chosen by the "rockchip,tcm" property on the node:
 *
 *   bus mode (default): image at 0xFFF88000. The M0 fetches its code over the
 *   SoC bus, about 2.5 clock cycles per instruction, and the firmware can be
 *   stopped and reloaded at will.
 *
 *   TCM mode: image at 0xFFF84000. With stock OP-TEE, giving the SiP call that
 *   address switches the SRAM from 0xFFF84000 to 0xFFF8C000 into the M0's
 *   tightly-coupled memory: one cycle per instruction, nothing else on the bus
 *   can hold up a fetch, and the A7 is locked out of that SRAM until reboot
 *   (nvitya/rk3506-mcu issue #2). So the image is loaded exactly once per
 *   boot; stopping and starting again only resets the M0, which then runs the
 *   image still sitting in the TCM, and this module can no longer be removed.
 */
#define RK3506_MCU_CODE_ADDR 0xFFF88000
#define RK3506_MCU_CODE_SIZE 0x4000

#define RK3506_PMU_BASE 0xFF900000
#define RK3506_CRU_BASE 0xFF9A0000
#define RK3506_GRF_BASE 0xFF288000

/*
 * "picocalc,double-core-clock": run the M0 at twice the usual rate.
 *
 * hclk_m0 is a gate on aclk_bus_root, which is clk_gpll_div (GPLL / 8,
 * 187.5 MHz) undivided, and clk_gpll_div also feeds the low-speed peripheral
 * bus and the clocks listed below, each through a divider of its own. Halving
 * each of those first (so that none is ever above its usual rate), then
 * setting clk_gpll_div to GPLL / 4, leaves them all where they were and puts
 * the M0 (with the system SRAM and the DMA controllers) at 375 MHz, its GPIO
 * clock exactly a quarter of that. While the driver is bound none of them can
 * be set above its old rate: a driver asking for a new rate could otherwise
 * be given twice what it expects. Through the clock framework, so the rates
 * Linux reports stay true. The RK3506 TRM gives no limit for aclk_bus_root;
 * this has been run on one board (picocalc-drivers docs/m0-audio.md).
 */
#define RK3506_BUS_SLOW_HZ 187500000UL
#define RK3506_BUS_FAST_HZ 375000000UL

static const u32 rk3506_bus_siblings[] = {
	HCLK_VIO_ROOT, CLK_SPI1, CLK_SPI0, CLK_PWM1, CLK_I2C2, CLK_I2C1, CLK_I2C0,
	HCLK_LSPERI_ROOT, PCLK_BUS_ROOT, CLK_GPLL_DIV_100M,
};

struct rk3506_fast_bus {
	struct clk *gpll_div;
	struct clk *sib[ARRAY_SIZE(rk3506_bus_siblings)];
	unsigned long rate[ARRAY_SIZE(rk3506_bus_siblings)];
	bool on;
};

static struct clk *rk3506_cru_clk(struct device_node *cru, u32 id)
{
	struct of_phandle_args spec = { .np = cru, .args_count = 1, .args = { id } };

	return of_clk_get_from_provider(&spec);
}

static void rk3506_fast_bus_put(struct rk3506_fast_bus *fb)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(fb->sib); i++)
		if (!IS_ERR_OR_NULL(fb->sib[i]))
			clk_put(fb->sib[i]);
	if (!IS_ERR_OR_NULL(fb->gpll_div))
		clk_put(fb->gpll_div);
	fb->gpll_div = NULL;
}

static int rk3506_fast_bus_on(struct device *dev, struct rk3506_fast_bus *fb)
{
	struct device_node *cru = of_find_compatible_node(NULL, NULL, "rockchip,rk3506-cru");
	int i, ret = 0;

	if (!cru)
		return -ENODEV;
	fb->gpll_div = rk3506_cru_clk(cru, CLK_GPLL_DIV);
	for (i = 0; i < ARRAY_SIZE(fb->sib); i++)
		fb->sib[i] = rk3506_cru_clk(cru, rk3506_bus_siblings[i]);
	of_node_put(cru);
	if (IS_ERR(fb->gpll_div))
		ret = PTR_ERR(fb->gpll_div);
	for (i = 0; !ret && i < ARRAY_SIZE(fb->sib); i++)
		if (IS_ERR(fb->sib[i]))
			ret = PTR_ERR(fb->sib[i]);
	if (ret)
		goto put;
	if (clk_get_rate(fb->gpll_div) != RK3506_BUS_SLOW_HZ) {
		dev_warn(dev, "clk_gpll_div is %lu Hz, not %lu: core clock left alone\n",
			 clk_get_rate(fb->gpll_div), RK3506_BUS_SLOW_HZ);
		ret = -EINVAL;
		goto put;
	}

	for (i = 0; i < ARRAY_SIZE(fb->sib); i++) {
		fb->rate[i] = clk_get_rate(fb->sib[i]);
		ret = clk_set_rate(fb->sib[i], fb->rate[i] / 2);
		if (ret || clk_get_rate(fb->sib[i]) != fb->rate[i] / 2 ||
		    !clk_is_match(clk_get_parent(fb->sib[i]), fb->gpll_div)) {
			dev_warn(dev, "could not halve %s: core clock left alone\n",
				 __clk_get_name(fb->sib[i]));
			ret = ret ? ret : -EINVAL;
			goto undo;
		}
	}
	ret = clk_set_rate(fb->gpll_div, RK3506_BUS_FAST_HZ);
	if (ret)
		goto undo;
	for (i = 0; i < ARRAY_SIZE(fb->sib); i++) {
		/* Setting a limit makes the framework go back to the last rate
		 * asked for, which is half: so the limit first, then the rate. */
		clk_set_max_rate(fb->sib[i], fb->rate[i]);
		clk_set_rate(fb->sib[i], fb->rate[i]);
		if (clk_get_rate(fb->sib[i]) != fb->rate[i])
			dev_warn(dev, "%s is now %lu Hz, was %lu\n", __clk_get_name(fb->sib[i]),
				 clk_get_rate(fb->sib[i]), fb->rate[i]);
	}
	fb->on = true;
	return 0;

undo:
	i = ARRAY_SIZE(fb->sib);
	while (--i >= 0)
		if (fb->rate[i])
			clk_set_rate(fb->sib[i], fb->rate[i]);
put:
	rk3506_fast_bus_put(fb);
	return ret;
}

static void rk3506_fast_bus_off(struct rk3506_fast_bus *fb)
{
	int i;

	if (!fb->on)
		return;
	for (i = 0; i < ARRAY_SIZE(fb->sib); i++)
		clk_set_max_rate(fb->sib[i], ULONG_MAX);
	clk_set_rate(fb->gpll_div, RK3506_BUS_SLOW_HZ);	/* the others at half for a moment */
	for (i = 0; i < ARRAY_SIZE(fb->sib); i++)
		clk_set_rate(fb->sib[i], fb->rate[i]);
	rk3506_fast_bus_put(fb);
	fb->on = false;
}

typedef struct {
	struct rproc *rproc;
	struct clk_bulk_data *clks;
	int num_clks;
	struct reset_control *rst_h_m0;
	struct reset_control *rst_m0_jtag;
	struct reset_control *rst_hresetn_m0_ac;
	uint8_t *tcm_virt;
	phys_addr_t tcm_phys;
	uint8_t *regs_PMU;
	uint8_t *regs_CRU;
	uint8_t *regs_GRF;
	struct platform_device *pdev;
	bool tcm;           /* "rockchip,tcm": run the image as TCM */
	bool tcm_engaged;   /* the SRAM is the M0's now; no more loading */
	uint32_t code_addr; /* where M0 address 0 is */
	uint32_t code_size;
	struct rk3506_fast_bus fast_bus; /* "picocalc,double-core-clock" */
} rk3506_mcu_t;

static void rk3506_rproc_mcu_run(rk3506_mcu_t *mcu, bool arun)
{
	if (arun) {
		/* Release M0 reset + enable M0 interrupts */
		writel(0x00060004, mcu->regs_PMU + 0x00C);
	} else {
		/* Assert M0 reset + disable M0 interrupts */
		writel(0x00060002, mcu->regs_PMU + 0x00C);
	}
}

static int rk3506_rproc_start(struct rproc *rproc)
{
	rk3506_mcu_t *mcu = rproc->priv;
	struct arm_smccc_res res;
	/* Base of the vector table, not the ELF entry point: the M0 takes its
	 * initial SP and reset vector from here. */
	uint32_t mcu_entry = mcu->code_addr;

	if (mcu->tcm_engaged) {
		/* Address map and image are already in place; just let it run. */
		dev_info(&rproc->dev, "Restarting M0 MCU from its TCM image");
		rk3506_rproc_mcu_run(mcu, true);
		return 0;
	}

	dev_info(&rproc->dev, "Starting M0 MCU at 0x%08X%s...", mcu_entry,
		 mcu->tcm ? " (TCM)" : "");

	/* Everything written to the SRAM must have landed before the M0 (or,
	 * in TCM mode, OP-TEE's switch of the SRAM port) gets to it. */
	wmb();
	arm_smccc_smc(SIP_MCU_CFG, ROCKCHIP_SIP_CONFIG_BUSMCU_0_ID,
		     ROCKCHIP_SIP_CONFIG_MCU_CODE_START_ADDR,
		     mcu_entry, 0, 0, 0, 0, &res);
	if (res.a0) {
		dev_err(&rproc->dev, "SMCCC CODE START call error: %i", (int)res.a0);
		return -EIO;
	}
	if (mcu->tcm) {
		mcu->tcm_engaged = true;
		/* The state above must outlive any attempt to unload us. */
		__module_get(THIS_MODULE);
		dev_info(&rproc->dev,
			 "SRAM 0x%08X-0x%08X is now M0 TCM: no firmware reload until reboot",
			 mcu->code_addr, mcu->code_addr + mcu->code_size);
	}

	rk3506_rproc_mcu_run(mcu, true);
	return 0;
}

static int rk3506_rproc_stop(struct rproc *rproc)
{
	rk3506_mcu_t *mcu = rproc->priv;

	dev_info(&rproc->dev, "Stopping M0 MCU");
	rk3506_rproc_mcu_run(mcu, false);
	return 0;
}

#if FW_FORMAT_BIN

static int rk3506_rproc_load(struct rproc *rproc, const struct firmware *fw)
{
	rk3506_mcu_t *mcu = rproc->priv;

	void __iomem *dst = mcu->tcm_virt + (mcu->code_addr - RK3506_MCU_TCM_ADDR);

	if (mcu->tcm_engaged)
		return 0;
	if (fw->size > mcu->code_size) {
		dev_err(&rproc->dev, "M0 MCU FW is too big: size=%u", (uint32_t)fw->size);
		return -EINVAL;
	}
	dev_info(&rproc->dev, "Loading FW: virt_addr=%p, size=%u", dst, (uint32_t)fw->size);
	memcpy_toio(dst, fw->data, fw->size);
	return 0;
}

#else

static void *my_da_to_va(struct rproc *rproc, u64 da, size_t len, bool *is_iomem)
{
	rk3506_mcu_t *mcu = rproc->priv;
	void __iomem *va;

	/* M0-local addresses (image linked at 0) land at the code address.
	 * Absolute SRAM addresses are accepted too, for segments an image wants
	 * placed outside the window the M0 sees at 0. The audio ring, in the
	 * first SRAM bank, is mapped separately by picocalc_snd_m0 (WC). */
	if (da + len <= mcu->code_size) {
		va = mcu->tcm_virt + (mcu->code_addr - RK3506_MCU_TCM_ADDR) + da;
	} else if (da >= RK3506_MCU_TCM_ADDR && (da + len) <= (RK3506_MCU_TCM_ADDR + RK3506_MCU_TCM_SIZE)) {
		va = mcu->tcm_virt + (da - RK3506_MCU_TCM_ADDR);
	} else {
		dev_err(&rproc->dev, "Invalid rproc address: 0x%08llX, len=%zu", (u64)da, len);
		va = NULL;
	}
	return va;
}

/* Once the SRAM is TCM the A7 must not touch it: the image stays as loaded. */
static int rk3506_rproc_elf_load(struct rproc *rproc, const struct firmware *fw)
{
	rk3506_mcu_t *mcu = rproc->priv;

	if (mcu->tcm_engaged)
		return 0;
	return rproc_elf_load_segments(rproc, fw);
}

/* The core copies its cached table over the loaded one; in TCM mode that
 * would be a write into SRAM the A7 may no longer own. The table is empty. */
static struct resource_table *rk3506_rproc_find_loaded_rsc_table(struct rproc *rproc,
								  const struct firmware *fw)
{
	rk3506_mcu_t *mcu = rproc->priv;

	if (mcu->tcm)
		return NULL;
	return rproc_elf_find_loaded_rsc_table(rproc, fw);
}

#endif

static const struct rproc_ops rk3506_rproc_ops = {
	.start = rk3506_rproc_start,
	.stop = rk3506_rproc_stop,
#if FW_FORMAT_BIN
	.load = rk3506_rproc_load,
#else
	.da_to_va = my_da_to_va,
	.load = rk3506_rproc_elf_load,
	.find_loaded_rsc_table = rk3506_rproc_find_loaded_rsc_table,
	.sanity_check = rproc_elf_sanity_check,
	.get_boot_addr = rproc_elf_get_boot_addr,
#endif
};

static const char *rk3506_rproc_get_firmware(struct platform_device *pdev)
{
	const char *fw_name;
	int ret;

	ret = of_property_read_string(pdev->dev.of_node, "firmware-name", &fw_name);
	if (ret)
		return ERR_PTR(ret);
	return fw_name;
}

static int rk3506_rproc_probe(struct platform_device *pdev)
{
	struct rproc *rproc;
	rk3506_mcu_t *mcu;
	int ret;
	const char *firmware;

	firmware = rk3506_rproc_get_firmware(pdev);
	if (IS_ERR(firmware)) {
		dev_err(&pdev->dev, "error getting firmware-name from the device-tree");
		return PTR_ERR(firmware);
	}

	rproc = rproc_alloc(&pdev->dev, dev_name(&pdev->dev),
			    &rk3506_rproc_ops, firmware, sizeof(rk3506_mcu_t));
	if (!rproc)
		return -ENOMEM;

	mcu = rproc->priv;
	mcu->rproc = rproc;
	/* Whoever uses the M0 decides when it runs. Booting at probe would
	 * leave picocalc_snd_m0's rproc_boot() a no-op on a core that is
	 * already up and idling, so its firmware would never see the stream. */
	rproc->auto_boot = false;
	mcu->tcm = of_property_read_bool(pdev->dev.of_node, "rockchip,tcm");
	mcu->code_addr = mcu->tcm ? RK3506_MCU_TCM_ADDR : RK3506_MCU_CODE_ADDR;
	mcu->code_size = mcu->tcm ? RK3506_MCU_TCM_SIZE : RK3506_MCU_CODE_SIZE;

	mcu->num_clks = devm_clk_bulk_get_all(&pdev->dev, &mcu->clks);
	if (mcu->num_clks < 0) {
		dev_err(&pdev->dev, "error getting all clocks from the device-tree");
		ret = -ENODEV;
		goto free_rproc;
	}

	mcu->tcm_phys = RK3506_MCU_TCM_ADDR;
	mcu->tcm_virt = ioremap(RK3506_MCU_TCM_ADDR, RK3506_MCU_TCM_SIZE);
	if (!mcu->tcm_virt) {
		dev_err(&pdev->dev, "failed to ioremap TCM");
		ret = -ENOMEM;
		goto free_rproc;
	}

	mcu->regs_PMU = ioremap(RK3506_PMU_BASE, 4096);
	mcu->regs_CRU = ioremap(RK3506_CRU_BASE, 4096);
	mcu->regs_GRF = ioremap(RK3506_GRF_BASE, 4096);
	if (!mcu->regs_PMU || !mcu->regs_CRU || !mcu->regs_GRF) {
		ret = -ENOMEM;
		goto unmap_periph;
	}

	mcu->pdev = pdev;
	rk3506_rproc_mcu_run(mcu, false);

	ret = clk_bulk_prepare_enable(mcu->num_clks, mcu->clks);
	if (ret) {
		dev_err(&pdev->dev, "Error enabling clocks: %d", ret);
		goto unmap_periph;
	}

	if (of_property_read_bool(pdev->dev.of_node, "picocalc,double-core-clock")) {
		ret = rk3506_fast_bus_on(&pdev->dev, &mcu->fast_bus);
		if (ret)
			dev_warn(&pdev->dev, "core clock not doubled: %d\n", ret);
	}

	/* hclk_m0 is a gate on aclk_bus_root (CRU_CLKSEL_CON21), so the M0 runs
	 * at whatever the bus runs at. Firmware with a per-tick cycle budget
	 * (the audio firmware) is tuned against this number. */
	if (mcu->num_clks > 0)
		dev_info(&pdev->dev, "M0 core clock (hclk_m0): %lu Hz\n",
			 clk_get_rate(mcu->clks[0].clk));

	writel(0x0c000000, mcu->regs_CRU + 0x814);
	writel(0xbcd3d80, mcu->regs_GRF + 0x090);

	mcu->rst_h_m0 = devm_reset_control_get(&pdev->dev, "h_m0");
	if (IS_ERR(mcu->rst_h_m0)) {
		dev_err(&pdev->dev, "error getting reset: h_m0");
		ret = PTR_ERR(mcu->rst_h_m0);
		goto disable_clks;
	}
	mcu->rst_m0_jtag = devm_reset_control_get(&pdev->dev, "m0_jtag");
	if (IS_ERR(mcu->rst_m0_jtag)) {
		dev_err(&pdev->dev, "error getting reset: m0_jtag");
		ret = PTR_ERR(mcu->rst_m0_jtag);
		goto disable_clks;
	}
	mcu->rst_hresetn_m0_ac = devm_reset_control_get(&pdev->dev, "hresetn_m0_ac");
	if (IS_ERR(mcu->rst_hresetn_m0_ac)) {
		dev_err(&pdev->dev, "error getting reset: hresetn_m0_ac");
		ret = PTR_ERR(mcu->rst_hresetn_m0_ac);
		goto disable_clks;
	}

	reset_control_deassert(mcu->rst_m0_jtag);
	reset_control_deassert(mcu->rst_h_m0);
	reset_control_deassert(mcu->rst_hresetn_m0_ac);

	platform_set_drvdata(pdev, rproc);
	ret = rproc_add(rproc);
	if (ret)
		goto disable_clks;
	return 0;

disable_clks:
	rk3506_fast_bus_off(&mcu->fast_bus);
	clk_bulk_disable_unprepare(mcu->num_clks, mcu->clks);
unmap_periph:
	if (mcu->regs_PMU) iounmap(mcu->regs_PMU);
	if (mcu->regs_CRU) iounmap(mcu->regs_CRU);
	if (mcu->regs_GRF) iounmap(mcu->regs_GRF);
	if (mcu->tcm_virt) iounmap(mcu->tcm_virt);
free_rproc:
	rproc_free(rproc);
	return ret;
}

static void rk3506_rproc_shutdown(struct platform_device *pdev)
{
	struct rproc *rproc = platform_get_drvdata(pdev);

	if (rproc)
		rk3506_rproc_stop(rproc);
}

static int rk3506_rproc_remove(struct platform_device *pdev)
{
	struct rproc *rproc = platform_get_drvdata(pdev);
	rk3506_mcu_t *mcu = rproc->priv;

	rk3506_rproc_shutdown(pdev);
	rk3506_fast_bus_off(&mcu->fast_bus);
	clk_bulk_disable_unprepare(mcu->num_clks, mcu->clks);
	if (mcu->tcm_virt) iounmap(mcu->tcm_virt);
	if (mcu->regs_PMU) iounmap(mcu->regs_PMU);
	if (mcu->regs_CRU) iounmap(mcu->regs_CRU);
	if (mcu->regs_GRF) iounmap(mcu->regs_GRF);
	rproc_del(rproc);
	rproc_free(rproc);
	return 0;
}

static const struct of_device_id rk3506_rproc_match[] = {
	{ .compatible = "rockchip,rk3506-mcu" },
	{ }
};
MODULE_DEVICE_TABLE(of, rk3506_rproc_match);

static struct platform_driver rk3506_rproc_driver = {
	.probe = rk3506_rproc_probe,
	.remove = rk3506_rproc_remove,
	.shutdown = rk3506_rproc_shutdown,
	.driver = {
		.name = "rk3506_mcu_rproc",
		.of_match_table = rk3506_rproc_match,
	},
};

module_platform_driver(rk3506_rproc_driver);

MODULE_AUTHOR("Viktor Nagy <nvitya@users.noreply.github.com>");
MODULE_DESCRIPTION("RK3506 Cortex-M0 Remote Processor Driver");
MODULE_LICENSE("GPL v2");
