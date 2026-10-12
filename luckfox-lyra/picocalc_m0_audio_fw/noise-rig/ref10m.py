#!/usr/bin/env python3
"""ref10m.py on|off|status: a 10 MHz reference from the SoC on Lyra pin 34 (GPIO1_B1 / RMIO24).

GPLL (1.5 GHz, the same PLL as the M0's 375 MHz core clock = GPLL / 4) -> clk_frac_common_matrix0
(1/150) -> mclk_sai1 -> mclk_out_sai1 -> mclk_sai1_to_io (GRF_PMU 0xFF910004 bit 9, hiword-mask) -> RMIO24
(RM_IO function 74 = sai1_mclk: GRF_PMU rmio reg 0xE0 = 74 - 15, pad mux 7 in IOC1 0xFF660028).
Register addresses from drivers/clk/rockchip/clk-rk3506.c, drivers/pinctrl/pinctrl-rockchip.c and
rk3502.dtsi in the kernel tree. Runtime only: a power cycle undoes it. `off` restores the values
`on` saved in /tmp/ref10m.save. Nothing else uses these clocks or this pad.
Pin 34 is a 3.3 V pad (a CMOS square wave): check what the scope's reference input accepts first.
"""
import mmap, os, struct, sys, json

CRU, GRFPMU, IOC1 = 0xFF9A0000, 0xFF910000, 0xFF660000
fd = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)
maps = {}
def m(base):
    if base not in maps:
        maps[base] = mmap.mmap(fd, 0x10000, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=base)
    return maps[base]
def rd(base, off): return struct.unpack("<I", m(base)[off:off + 4])[0]
def wr(base, off, v): m(base)[off:off + 4] = struct.pack("<I", v & 0xFFFFFFFF)
def hi(base, off, mask, val): wr(base, off, (mask << 16) | (val & mask))   # write-enable-in-upper-half registers

REGS = {   # name: (base, offset)
    "frac_div(CLKSEL_CON11)": (CRU, 0x32C),
    "frac_mux(CLKSEL_CON4)": (CRU, 0x310),
    "frac0_gate(CLKGATE_CON1)": (CRU, 0x804),
    "mclk_sai1(CLKSEL_CON41)": (CRU, 0x3A4),
    "sai1_gates(CLKGATE_CON14)": (CRU, 0x838),
    "mclkout_to_io(GRF_PMU+4)": (GRFPMU, 0x004),
    "rmio24(GRF_PMU+0xE0)": (GRFPMU, 0x0E0),
    "gpio1_b_iomux(IOC1+0x28)": (IOC1, 0x028),
}
def show():
    for k, (b, o) in REGS.items():
        print("  %-28s %#010x" % (k, rd(b, o)))
    d = rd(CRU, 0x32C); n, den = d >> 16, d & 0xFFFF
    print("  frac divider reads as %d/%d -> %.4f MHz from 1500 MHz" % (n, den, 1500.0 * n / den if den else 0))
    mux = (rd(CRU, 0x310) >> 2) & 3; print("  frac0 mux parent index %d (0 xin24m, 1 gpll, 2 v0pll, 3 v1pll)" % mux)
    print("  mclk_sai1 parent index %d (6 = clk_frac_common_matrix0), div field %d" % ((rd(CRU, 0x3A4) >> 8) & 0xF, rd(CRU, 0x3A4) & 0xFF))
    print("  gates (0 = running): frac0 %d, mclk_sai1 %d, mclk_out_sai1 %d; to_io bit9 %d" % ((rd(CRU, 0x804) >> 1) & 1, (rd(CRU, 0x838) >> 2) & 1, (rd(CRU, 0x838) >> 4) & 1, (rd(GRFPMU, 4) >> 9) & 1))
    print("  pad GPIO1_B1 mux %d (7 = rm_io), rmio function %d" % ((rd(IOC1, 0x28) >> 4) & 0xF, rd(GRFPMU, 0xE0) & 0x7F))

cmd = sys.argv[1] if len(sys.argv) > 1 else "status"
if cmd == "status":
    show()
elif cmd == "on":
    if not os.path.exists("/tmp/ref10m.save"):
        json.dump({k: rd(b, o) for k, (b, o) in REGS.items()}, open("/tmp/ref10m.save", "w"))
    assert (rd(CRU, 0x310) >> 2) & 3 == 1, "frac0 mux is not on GPLL"
    wr(CRU, 0x32C, (1 << 16) | 150)                 # numerator 1, denominator 150: GPLL / 150 = 10 MHz
    hi(CRU, 0x3A4, 0x0FFF, (6 << 8) | 0)            # mclk_sai1 <- clk_frac_common_matrix0, /1
    hi(CRU, 0x804, 1 << 1, 0)                       # frac0 gate open
    hi(CRU, 0x838, (1 << 2) | (1 << 4), 0)          # mclk_sai1 and mclk_out_sai1 gates open
    hi(GRFPMU, 0x004, 1 << 9, 1 << 9)               # mclk_sai1_to_io (hiword-mask gate)
    hi(GRFPMU, 0x0E0, 0x7F, 74 - 15)                # RM_IO24 function: sai1_mclk
    hi(IOC1, 0x028, 0xF << 4, 7 << 4)               # pad mux: rm_io
    show()
elif cmd == "off":
    s = json.load(open("/tmp/ref10m.save"))
    hi(IOC1, 0x028, 0xF << 4, (s["gpio1_b_iomux(IOC1+0x28)"] >> 4) & 0xF)
    hi(GRFPMU, 0x0E0, 0x7F, s["rmio24(GRF_PMU+0xE0)"] & 0x7F)
    hi(GRFPMU, 0x004, 1 << 9, s["mclkout_to_io(GRF_PMU+4)"] & (1 << 9))
    v = s["sai1_gates(CLKGATE_CON14)"]; hi(CRU, 0x838, (1 << 2) | (1 << 4), v & ((1 << 2) | (1 << 4)))
    hi(CRU, 0x804, 1 << 1, s["frac0_gate(CLKGATE_CON1)"] & (1 << 1))
    hi(CRU, 0x3A4, 0x0FFF, s["mclk_sai1(CLKSEL_CON41)"] & 0x0FFF)
    wr(CRU, 0x32C, s["frac_div(CLKSEL_CON11)"])
    os.remove("/tmp/ref10m.save"); show()
elif cmd == "drive":      # drive N (0 weakest .. 7): IOC1 GPIO1_B1 drive field, 8-bit level code (1 << (N + 1)) - 1
    lvl = int(sys.argv[2]); print("drive reg before %#010x" % rd(IOC1, 0x150))
    hi(IOC1, 0x150, 0xFF << 8, ((1 << (lvl + 1)) - 1) << 8); print("drive reg after  %#010x" % rd(IOC1, 0x150))
else:
    print(__doc__)
