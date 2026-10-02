// SPDX-License-Identifier: GPL-2.0
/*
 * Experiment: run the RK3506's bus root clock (aclk_bus_root: the M0 core,
 * system SRAM, DMA controllers) at twice its usual rate, leaving every other
 * clock where it is.
 *
 * aclk_bus_root is clk_gpll_div undivided. clk_gpll_div (GPLL / 8 = 187.5 MHz)
 * also feeds the low-speed peripheral bus and some peripheral clocks. Loading
 * this module halves each of those first (so nothing is ever above its usual
 * rate), then sets clk_gpll_div to GPLL / 4: the others are back where they
 * were and aclk_bus_root is at 375 MHz. Unloading undoes it in the opposite
 * order. All through the clock framework, so the rates Linux reports stay
 * true; while loaded, none of the other clocks can be set above its old rate.
 */
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/module.h>
#include <linux/of.h>
#include <dt-bindings/clock/rockchip,rk3506-cru.h>

#define SLOW_HZ 187500000UL
#define FAST_HZ 375000000UL

static const struct {
	u32 id;
	const char *name;
} sib[] = {
	{ HCLK_VIO_ROOT, "hclk_vio_root" }, { CLK_SPI1, "clk_spi1" },
	{ CLK_SPI0, "clk_spi0" }, { CLK_PWM1, "clk_pwm1" },
	{ CLK_I2C2, "clk_i2c2" }, { CLK_I2C1, "clk_i2c1" }, { CLK_I2C0, "clk_i2c0" },
	{ HCLK_LSPERI_ROOT, "hclk_lsperi_root" }, { PCLK_BUS_ROOT, "pclk_bus_root" },
	{ CLK_GPLL_DIV_100M, "clk_gpll_div_100m" },
};

static struct clk *sib_clk[ARRAY_SIZE(sib)];
static unsigned long sib_rate[ARRAY_SIZE(sib)];
static struct clk *gpll_div, *aclk_bus, *hclk_m0, *pclk_gpio4;

static struct clk *get(struct device_node *cru, u32 id)
{
	struct of_phandle_args spec = { .np = cru, .args_count = 1, .args = { id } };

	return of_clk_get_from_provider(&spec);
}

static void report(const char *when)
{
	pr_info("m0clk: %s: clk_gpll_div %lu, aclk_bus_root %lu, hclk_m0 %lu, pclk_gpio4 %lu\n",
		when, clk_get_rate(gpll_div), clk_get_rate(aclk_bus),
		clk_get_rate(hclk_m0), clk_get_rate(pclk_gpio4));
}

static int __init m0clk_init(void)
{
	struct device_node *cru = of_find_compatible_node(NULL, NULL, "rockchip,rk3506-cru");
	int i, ret = 0;

	if (!cru)
		return -ENODEV;
	gpll_div = get(cru, CLK_GPLL_DIV);
	aclk_bus = get(cru, ACLK_BUS_ROOT);
	hclk_m0 = get(cru, HCLK_M0);
	pclk_gpio4 = get(cru, PCLK_GPIO4);
	for (i = 0; i < ARRAY_SIZE(sib); i++)
		sib_clk[i] = get(cru, sib[i].id);
	of_node_put(cru);
	if (IS_ERR(gpll_div) || IS_ERR(aclk_bus) || IS_ERR(hclk_m0) || IS_ERR(pclk_gpio4))
		return -ENOENT;
	for (i = 0; i < ARRAY_SIZE(sib); i++)
		if (IS_ERR(sib_clk[i]))
			return -ENOENT;

	report("before");
	if (clk_get_rate(gpll_div) != SLOW_HZ || clk_get_rate(aclk_bus) != SLOW_HZ) {
		pr_err("m0clk: not the clock setup this was written for\n");
		return -EINVAL;
	}

	for (i = 0; i < ARRAY_SIZE(sib); i++) {
		sib_rate[i] = clk_get_rate(sib_clk[i]);
		ret = clk_set_rate(sib_clk[i], sib_rate[i] / 2);
		if (ret || clk_get_rate(sib_clk[i]) != sib_rate[i] / 2 ||
		    clk_get_rate(gpll_div) != SLOW_HZ ||
		    !clk_is_match(clk_get_parent(sib_clk[i]), gpll_div)) {
			pr_err("m0clk: could not halve %s (%d, now %lu from %s)\n", sib[i].name,
			       ret, clk_get_rate(sib_clk[i]),
			       __clk_get_name(clk_get_parent(sib_clk[i])));
			ret = ret ? ret : -EINVAL;
			goto undo;
		}
	}
	ret = clk_set_rate(gpll_div, FAST_HZ);
	if (ret) {
		pr_err("m0clk: clk_gpll_div: %d\n", ret);
		i = ARRAY_SIZE(sib);
		goto undo;
	}
	for (i = 0; i < ARRAY_SIZE(sib); i++) {
		/* Their parent is twice as fast now: nobody may take that as
		 * leave to run one of them faster than it ever was. Setting a
		 * limit makes the framework go back to the last rate asked for,
		 * which is half; so ask for the old rate again after it. */
		clk_set_max_rate(sib_clk[i], sib_rate[i]);
		clk_set_rate(sib_clk[i], sib_rate[i]);
		if (clk_get_rate(sib_clk[i]) != sib_rate[i])
			pr_warn("m0clk: %s is now %lu, was %lu\n", sib[i].name,
				clk_get_rate(sib_clk[i]), sib_rate[i]);
	}
	report("after");
	return 0;

undo:
	while (--i >= 0)
		clk_set_rate(sib_clk[i], sib_rate[i]);
	return ret;
}

static void __exit m0clk_exit(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(sib); i++)
		clk_set_max_rate(sib_clk[i], ULONG_MAX);
	clk_set_rate(gpll_div, SLOW_HZ);	/* the others are at half for a moment */
	for (i = 0; i < ARRAY_SIZE(sib); i++)
		clk_set_rate(sib_clk[i], sib_rate[i]);
	report("restored");
}

module_init(m0clk_init);
module_exit(m0clk_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Experiment: RK3506 bus root clock at 375 MHz");
