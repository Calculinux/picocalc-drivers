#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Run the M0 diagnostic firmware in a CPU emulator with a modelled timer and
check that the results block comes out as the model says it should.

Needs: pip install unicorn pyelftools   (run via `make check-emu`)

This checks the firmware's logic and the layout of the results block. It
cannot say anything about the real SoC: in particular WFI is replaced by a
no-op here, so the WFI stages run as polling with one "spurious" return
per poll, and SysTick is not modelled, so the firmware finds none and skips
the stages that need it.
"""
import struct
import sys

from elftools.elf.elffile import ELFFile
from unicorn import (Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_MEM_READ,
                     UC_HOOK_MEM_WRITE, UC_HOOK_BLOCK)
from unicorn.arm_const import UC_CPU_ARM_CORTEX_M0, UC_ARM_REG_SP

CODE_BASE, CODE_SIZE = 0x00000000, 0x1000
RES = 0xFFF81000
CH4, CH5 = 0xFF254000, 0xFF255000
LOAD0, CURR0, CTRL, INTSTAT = 0x00, 0x08, 0x10, 0x18
PERIPH_PAGES = (0xFF254000, 0xFF255000, 0xFF1E0000, 0xFF4D8000, 0xFF9A0000, 0xE000E000)
GPIO4_DR = 0xFF1E0000
STOP_AT_HEARTBEAT = 5

# struct m0_diag field offsets (diag.h), written out independently
HEAD = ['magic', 'version', 'stage', 'fault', 'heartbeat',
        'cal_empty', 'cal_nop8', 'cal_gpio_wr', 'cal_timer_rd', 'cal_sram_abs',
        'cal_sram_low', 'cal_ppb_wr', 'period_n', 'period_counts',
        'poll_slow_ticks', 'wfi_slow_ticks', 'wfi_slow_spurious',
        'syst_present', 'syst_calib', 'syst_per_1000', 'stamp_first', 'syst_nowake']
LAT = ['n', 'min', 'max', 'sum', 'spurious', 'pend']
HIST = 64
F = {name: 4 * i for i, name in enumerate(HEAD)}
for j, block in enumerate(['poll', 'wfi', 'stamp', 'syst', 'acc_gpio', 'acc_timer',
                           'acc_gpio_rd', 'acc_gpio_wr_rd', 'acc_gpio_wr_wr']):
    for i, name in enumerate(LAT):
        F[f'{block}_{name}'] = 4 * (len(HEAD) + j * (len(LAT) + HIST) + i)
RUN_STAGE = 12


class Timer:
    """One channel: free-running count-up, period LOAD + 1 counts."""

    def __init__(self):
        self.load = 0
        self.ctrl = 0
        self.t0 = 0
        self.seen = 0

    def expiries(self, now):
        return (now - self.t0) // (self.load + 1) if self.ctrl & 1 else 0


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else 'rk3506-m0-diag.elf'
    uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
    uc.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_M0)
    uc.mem_map(CODE_BASE, CODE_SIZE)
    uc.mem_map(RES, 0x1000)
    for page in PERIPH_PAGES:
        uc.mem_map(page, 0x1000)
    with open(path, 'rb') as f:
        for seg in ELFFile(f).iter_segments():
            if seg['p_type'] == 'PT_LOAD' and seg['p_filesz']:
                assert seg['p_paddr'] + seg['p_memsz'] <= CODE_SIZE
                data = seg.data().replace(b'\x30\xbf', b'\x00\xbf')  # wfi -> nop
                uc.mem_write(seg['p_paddr'], data)

    clock = {'insns': 0, 'ff': 0, 'jit': 0}
    timers = {CH4: Timer(), CH5: Timer()}
    b2 = []

    def now():
        return clock['insns'] // 2 + clock['ff']   # ~2 instructions per 10 ns count

    def on_block(uc, addr, size, data):
        clock['insns'] += size // 2

    def on_read(uc, access, addr, size, value, data):
        base, off = addr & ~0xFFF, addr & 0xFFF
        t = timers[base]
        if off == CURR0:
            v = (now() - t.t0) % (t.load + 1) if t.ctrl & 1 else 0
            uc.mem_write(addr, struct.pack('<I', v & 0xFFFFFFFF))
        elif off == INTSTAT:
            pend = t.expiries(now()) > t.seen
            if not pend and t.ctrl & 1:
                # Idle poll: this read still says "not expired", but time
                # skips to a varying few counts past the next expiry, so the
                # following read detects it with a latency that varies.
                nxt = t.t0 + (t.seen + 1) * (t.load + 1)
                clock['jit'] = (clock['jit'] + 1) % 4
                gap = nxt - now() + clock['jit']
                if gap > 0:
                    clock['ff'] += gap
            uc.mem_write(addr, struct.pack('<I', 1 if pend else 0))

    def on_write(uc, access, addr, size, value, data):
        base, off = addr & ~0xFFF, addr & 0xFFF
        if base == GPIO4_DR and off == 0:
            b2.append((value >> 10) & 1)
            return
        if base == RES:
            if off == F['heartbeat'] and value >= STOP_AT_HEARTBEAT:
                uc.emu_stop()
            return
        t = timers.get(base)
        if t is None:
            return
        if off == LOAD0:
            t.load = value & 0xFFFFFFFF
        elif off == CTRL:
            if value & 1 and not t.ctrl & 1:
                t.t0, t.seen = now(), 0
            t.ctrl = value
        elif off == INTSTAT and value & 1:
            t.seen = t.expiries(now())

    uc.hook_add(UC_HOOK_BLOCK, on_block)
    for base in timers:
        uc.hook_add(UC_HOOK_MEM_READ, on_read, begin=base, end=base + 0xFFF)
        uc.hook_add(UC_HOOK_MEM_WRITE, on_write, begin=base, end=base + 0xFFF)
    uc.hook_add(UC_HOOK_MEM_WRITE, on_write, begin=GPIO4_DR, end=GPIO4_DR + 3)
    uc.hook_add(UC_HOOK_MEM_WRITE, on_write, begin=RES, end=RES + 0xFFF)

    sp, reset = struct.unpack('<II', uc.mem_read(CODE_BASE, 8))
    uc.reg_write(UC_ARM_REG_SP, sp)
    uc.emu_start(reset, CODE_BASE + CODE_SIZE, count=400_000_000)

    r = {k: struct.unpack('<I', uc.mem_read(RES + off, 4))[0] for k, off in F.items()}
    bad = []

    def expect(cond, msg):
        if not cond:
            bad.append(msg)

    expect(r['magic'] == 0x4D304447, 'magic')
    expect(r['version'] == 4, f"version {r['version']}")
    expect(r['stage'] == RUN_STAGE, f"stage {r['stage']}, want {RUN_STAGE} (RUN)")
    expect(r['syst_present'] == 0, 'SysTick found, but the emulator has none')
    expect(r['fault'] == 0, 'fault flag set')
    expect(r['heartbeat'] >= STOP_AT_HEARTBEAT, 'heartbeat')
    # Calibration: the model charges half a count per instruction, so the
    # 2-instruction loop costs 1 count per iteration and the 3-instruction
    # loops 1.5.
    expect(abs(r['cal_empty'] - 65536) < 64, f"cal_empty {r['cal_empty']}")
    for k in ('cal_gpio_wr', 'cal_timer_rd', 'cal_sram_abs', 'cal_sram_low', 'cal_ppb_wr'):
        expect(abs(r[k] - 98304) < 64, f"{k} {r[k]}")
    expect(abs(r['cal_nop8'] - 327680) < 64, f"cal_nop8 {r['cal_nop8']}")
    expect(r['period_n'] == 10000, 'period_n')
    per = r['period_counts'] / max(r['period_n'], 1)
    expect(abs(per - 1000.0) < 0.5, f'period {per} counts, model is LOAD + 1 = 1000')
    for name in ('poll', 'wfi'):
        n, lo, hi, total = r[name + '_n'], r[name + '_min'], r[name + '_max'], r[name + '_sum']
        expect(n == 4096, f'{name} n {n}')
        expect(lo <= hi < 32, f'{name} latency range {lo}..{hi}')
        expect(hi > lo, f'{name} latency shows no spread, model varies it')
        expect(lo * n <= total <= hi * n, f'{name} sum {total}')
    expect(r['poll_spurious'] == 0, 'poll spurious')
    expect(r['poll_slow_ticks'] == 200, f"poll_slow_ticks {r['poll_slow_ticks']}")
    expect(r['wfi_slow_ticks'] == 200, f"wfi_slow_ticks {r['wfi_slow_ticks']}")
    # wfi is a nop here: every poll that finds nothing counts as spurious
    expect(r['wfi_slow_spurious'] > 0 and r['wfi_spurious'] > 0, 'no spurious WFI counted')
    # B2 alternates on every tick of the two slow stages and is quiet otherwise
    toggles = sum(1 for a, b in zip(b2, b2[1:]) if a != b)
    expect(396 <= toggles <= 400, f'B2 toggled {toggles} times, want about 400')
    # the latency histograms must account for every sample
    for name in ('poll', 'wfi'):
        base = RES + F[name + '_n'] + 4 * len(LAT)
        hist = struct.unpack(f'<{HIST}I', uc.mem_read(base, 4 * HIST))
        expect(sum(hist) == 4096, f'{name} histogram holds {sum(hist)} samples')

    print(f"stage {r['stage']}, period {per:.3f} counts, poll latency {r['poll_min']}..{r['poll_max']}, "
          f"wfi-as-poll latency {r['wfi_min']}..{r['wfi_max']}, B2 toggles {toggles}")
    for msg in bad:
        print('  FAIL:', msg)
    print('check_diag: ' + (f'{len(bad)} FAILED' if bad else 'ok'))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
