#!/usr/bin/env python3
"""pincount.py: an on-chip edge counter on Lyra pin 34 (GPIO1_B1 / RMIO24), meant to be jumpered to pin 31.

RMIO24 is routed to PWM1 channel 0 (RM_IO function 49 -> GRF_PMU 0xFF9100E0 = 49-15, pad mux 7 in
IOC1 0xFF660028) and PWM1's counter mode (COUNTER_ARBITER 0x200, COUNTER_CTRL 0x204, COUNTER_LOW/HIGH
0x208/0x20c of the channel block at 0xFF170000, register layout from drivers/pwm/pwm-rockchip.c) counts the
input. Usage: pincount.py setup | read | clear | status | off | calib N
`calib N` drives pin 31 from the A53 with N full toggles (M0 must be idle) and prints the counter change.
"""
import mmap, os, struct, sys, time
CRU, GRFPMU, IOC1, PWM1, G4 = 0xFF9A0000, 0xFF910000, 0xFF660000, 0xFF170000, 0xFF1E0000
fd = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)
maps = {}
def m(b):
    if b not in maps: maps[b] = mmap.mmap(fd, 0x1000, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE, offset=b)
    return maps[b]
rd = lambda b, o: struct.unpack("<I", m(b)[o:o + 4])[0]
def wr(b, o, v): m(b)[o:o + 4] = struct.pack("<I", v & 0xFFFFFFFF)
def hi(b, o, mask, val): wr(b, o, (mask << 16) | (val & mask))
def counter(): return (rd(PWM1, 0x20C) << 32) | rd(PWM1, 0x208)
def setup():
    hi(CRU, 0x830, (1 << 4) | (1 << 5) | (1 << 9), 0)          # CLKGATE_CON12: pclk_pwm1, clk_pwm1, clk_counter_pwm1 open
    hi(GRFPMU, 0x0E0, 0x7F, 49 - 15)                            # RM_IO24 -> pwm1_ch0
    hi(IOC1, 0x028, 0xF << 4, 7 << 4)                           # pad mux: rm_io
    print("PWM1 ch0 VERSION_ID %#010x" % rd(PWM1, 0))
    wr(PWM1, 0x200, (1 << 16) | 1)                              # COUNTER_ARBITER: grant + read lock for channel 0
    print("COUNTER_ARBITER reads %#x" % rd(PWM1, 0x200))
    hi(PWM1, 0x204, 0x3F, 0x01)                                 # COUNTER_CTRL: EN, CLK_SEL = IO, CHANNEL_SEL = 0
    wr(PWM1, 0x204, (1 << 22) | (1 << 6))                       # COUNTER_CLR
    print("COUNTER_CTRL %#x counter %d" % (rd(PWM1, 0x204), counter()))
cmd = sys.argv[1] if len(sys.argv) > 1 else "status"
if cmd == "setup": setup()
elif cmd == "read": print(counter())
elif cmd == "clear": wr(PWM1, 0x204, (1 << 22) | (1 << 6)); print(counter())
elif cmd == "status":
    print("CLKGATE_CON12 %#x (0 = open)" % rd(CRU, 0x830), "| PWM1 version %#x" % rd(PWM1, 0), "| arbiter %#x ctrl %#x" % (rd(PWM1, 0x200), rd(PWM1, 0x204)), "| counter", counter(), "| rmio %d pad mux %d" % (rd(GRFPMU, 0xE0) & 0x7F, (rd(IOC1, 0x28) >> 4) & 0xF))
elif cmd == "off":
    hi(PWM1, 0x204, 0x1, 0); wr(PWM1, 0x200, 1 << 16); hi(IOC1, 0x028, 0xF << 4, 0); hi(GRFPMU, 0x0E0, 0x7F, 0); print("off")
elif cmd == "calib":
    n = int(sys.argv[2]); ddr = rd(G4, 8) & 0xFFFF
    hi(G4, 8, 1 << 10, 1 << 10); hi(G4, 0, 1 << 10, 0); time.sleep(0.01)
    c0 = counter()
    for i in range(n):
        hi(G4, 0, 1 << 10, 1 << 10); hi(G4, 0, 1 << 10, 0)
    time.sleep(0.01); c1 = counter()
    hi(G4, 8, 1 << 10, ddr & (1 << 10))
    print("drove %d full toggles (%d rising + %d falling edges): counter %d -> %d, change %d" % (n, n, n, c0, c1, c1 - c0))
