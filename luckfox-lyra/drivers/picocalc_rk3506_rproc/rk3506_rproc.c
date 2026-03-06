// SPDX-License-Identifier: GPL-2.0
// Driver for RK3506 Cortex-M0 remoteproc
//
// Copyright (c) Viktor Nagy
// First version published at https://github.com/nvitya/rk3506-mcu
// Ported and extended for PicoCalc (ELF boot address from rproc->bootaddr)

#define FW_FORMAT_BIN 0

/*
 * Device-tree block:
 *
 *  mcu_rproc: mcu@fff84000 {
 *    compatible = "rockchip,rk3506-mcu";
 *    reg = <0xfff84000 0x8000>;
 *    firmware-name = "rk3506-m0-audio.elf";
 *    clocks = <&cru HCLK_M0>, <&cru STCLK_M0>,
 *             <&cru PCLK_TIMER>, <&cru CLK_TIMER0_CH5>;
 *    resets = <&cru SRST_H_M0>, <&cru SRST_M0_JTAG>, <&cru SRST_HRESETN_M0_AC>;
 *    reset-names = "h_m0", "m0_jtag", "hresetn_m0_ac";
 *  };
 */

#include <linux/arm-smccc.h>
#include <linux/clk.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/remoteproc.h>
#include <linux/reset.h>

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

#define RK3506_MCU_SHMEM_ADDR 0x03C00000
#define RK3506_MCU_SHMEM_SIZE 0x100000

#define RK3506_PMU_BASE 0xFF900000
#define RK3506_CRU_BASE 0xFF9A0000
#define RK3506_GRF_BASE 0xFF288000

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
	uint8_t *shmem_virt;
	struct platform_device *pdev;
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
	uint32_t mcu_entry;

	/* Use ELF boot address so firmware at 0xFFF88000 avoids OP-TEE SRAM lockdown (issue #2) */
	mcu_entry = (uint32_t)rproc->bootaddr;
	if (mcu_entry == 0)
		mcu_entry = 0xFFF84000; /* fallback for BIN or legacy ELF */

	dev_info(&rproc->dev, "Starting M0 MCU at 0x%08X...", mcu_entry);

	arm_smccc_smc(SIP_MCU_CFG, ROCKCHIP_SIP_CONFIG_BUSMCU_0_ID,
		     ROCKCHIP_SIP_CONFIG_MCU_CODE_START_ADDR,
		     mcu_entry, 0, 0, 0, 0, &res);
	if (res.a0) {
		dev_err(&rproc->dev, "SMCCC CODE START call error: %i", (int)res.a0);
		return -EIO;
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

	if (fw->size > RK3506_MCU_TCM_SIZE) {
		dev_err(&rproc->dev, "M0 MCU FW is too big: size=%u", (uint32_t)fw->size);
		return -EINVAL;
	}
	dev_info(&rproc->dev, "Loading FW: virt_addr=%p, size=%u", mcu->tcm_virt, (uint32_t)fw->size);
	memcpy_toio(mcu->tcm_virt, fw->data, fw->size);
	return 0;
}

#else

static void *my_da_to_va(struct rproc *rproc, u64 da, size_t len, bool *is_iomem)
{
	rk3506_mcu_t *mcu = rproc->priv;
	void __iomem *va;

	/* TCM at 0xFFF84000, size 0x8000; ELF may use 0xFFF88000 (second half) */
	if (da >= RK3506_MCU_TCM_ADDR && (da + len) <= (RK3506_MCU_TCM_ADDR + RK3506_MCU_TCM_SIZE)) {
		va = mcu->tcm_virt + (da - RK3506_MCU_TCM_ADDR);
	} else if (da >= RK3506_MCU_SHMEM_ADDR && (da + len) <= (RK3506_MCU_SHMEM_ADDR + RK3506_MCU_SHMEM_SIZE)) {
		va = mcu->shmem_virt + (da - RK3506_MCU_SHMEM_ADDR);
	} else {
		dev_err(&rproc->dev, "Invalid rproc address: 0x%08llX, len=%zu", (u64)da, len);
		va = NULL;
	}
	return va;
}

#endif

static const struct rproc_ops rk3506_rproc_ops = {
	.start = rk3506_rproc_start,
	.stop = rk3506_rproc_stop,
#if FW_FORMAT_BIN
	.load = rk3506_rproc_load,
#else
	.da_to_va = my_da_to_va,
	.load = rproc_elf_load_segments,
	.find_loaded_rsc_table = rproc_elf_find_loaded_rsc_table,
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

	mcu->shmem_virt = ioremap(RK3506_MCU_SHMEM_ADDR, RK3506_MCU_SHMEM_SIZE);
	if (!mcu->shmem_virt) {
		dev_err(&pdev->dev, "failed to ioremap shared memory");
		ret = -ENOMEM;
		goto unmap_tcm;
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
	clk_bulk_disable_unprepare(mcu->num_clks, mcu->clks);
unmap_periph:
	if (mcu->regs_PMU) iounmap(mcu->regs_PMU);
	if (mcu->regs_CRU) iounmap(mcu->regs_CRU);
	if (mcu->regs_GRF) iounmap(mcu->regs_GRF);
	if (mcu->shmem_virt) iounmap(mcu->shmem_virt);
unmap_tcm:
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
	clk_bulk_disable_unprepare(mcu->num_clks, mcu->clks);
	if (mcu->tcm_virt) iounmap(mcu->tcm_virt);
	if (mcu->shmem_virt) iounmap(mcu->shmem_virt);
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
