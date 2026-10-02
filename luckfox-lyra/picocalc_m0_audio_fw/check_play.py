#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Run m0_play() from the built ELF in a CPU emulator and compare every GPIO
write with an independent model of play.S.

Needs: pip install unicorn pyelftools   (run via `make check-emu`)

WFI is replaced by a no-op and SysTick always reads "pending", so each loop
iteration is one tick. GPIO4 DR writes are recorded. The ring is pre-filled
and kept non-empty except in the underrun test. This checks the arithmetic
and the per-sample schedule; timing on the real core is what PROFILE=1 and
picocalc_m0_diag_fw are for.
"""
import math
import random
import struct
import sys

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
from unicorn.arm_const import (UC_CPU_ARM_CORTEX_M0, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_R4,
                               UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7, UC_ARM_REG_R8,
                               UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11)

# Hardware constants, written out independently of the firmware headers.
CODE_BASE, CODE_SIZE = 0x00000000, 0x1000   # M0 view of the SRAM at 0xFFF88000
SHMEM = 0xFFF89000
HDR = 64
RING = 8192
STACK_TOP = CODE_BASE + CODE_SIZE
PPB = 0xE000E000
SCB_ICSR = 0xE000ED04
PENDSTSET = 1 << 26
GPIO4_DR = 0xFF1E0000
RET_ADDR = 0x10000000
CTRL, WRITE_IDX, READ_IDX = 4, 8, 12
PS_TICKS_BASE = 28                         # m0_play_state: base, rem, den

SAMPLE_RATE = 48000
BATCH = 8
FS_SHIFT, CLAMP_SHIFT = 15, 20
MASK32 = 0xFFFFFFFF


def timing(core_hz, tick_cycles):
    den = tick_cycles * SAMPLE_RATE
    return core_hz // den, core_hz % den, den


def s32(v):
    v &= MASK32
    return v - (1 << 32) if v & 0x80000000 else v


def scale(x):
    return x - (x >> 3)


class Model:
    """Tick-level model of play.S, written from its header comment."""

    def __init__(self, frames, read_idx, write_idx_fn, tm):
        self.frames = frames            # list of (l, r), index = ring offset / 4
        self.read_idx = read_idx & (RING - 4)
        self.write_idx_fn = write_idx_fn
        self.base, self.rem, self.den = tm
        self.x = [0, 0]
        self.nxt = [0, 0]
        self.next_ticks = self.base
        self.left = self.base
        self.err = 0
        self.batch = 0
        self.fetched = False
        self.i1 = [0, 0]
        self.i2 = [0, 0]
        self.g = [1 << FS_SHIFT, -(1 << FS_SHIFT)]
        self.word = 0x0C000000
        self.published = []
        self.tick_no = 0

    def tick(self):
        out = self.word
        self.left -= 1
        if self.left == 0:
            self.x = list(self.nxt)
            self.left = self.next_ticks
        elif self.left == 3:
            self.err += self.rem
            self.next_ticks = self.base
            if self.err >= self.den:
                self.err -= self.den
                self.next_ticks += 1
        elif self.left == 2:
            self.fetched = self.read_idx != self.write_idx_fn(self.tick_no)
            if self.fetched:
                l, r = self.frames[self.read_idx // 4]
                self.nxt = [scale(l), scale(r)]
        elif self.left == 1 and self.fetched:
            self.read_idx = (self.read_idx + 4) & (RING - 4)
            self.batch += 1
            if self.batch >= BATCH:
                self.batch = 0
                self.published.append(self.read_idx)
        word = 0x0C000C00
        for ch, bit in ((0, 10), (1, 11)):
            self.i1[ch] = s32(self.i1[ch] + self.x[ch] - self.g[ch])
            y = s32(self.i2[ch] + self.i1[ch])
            s = -1 if y < 0 else 0
            self.g[ch] = (2 * s + 1) << FS_SHIFT
            i2 = s32(y - self.g[ch])
            lim = 1 << CLAMP_SHIFT
            if not -lim <= i2 < lim:
                i2 = lim - 1 if i2 >= 0 else -lim
            self.i2[ch] = i2
            if s:
                word -= 1 << bit
        self.word = word
        self.tick_no += 1
        return out


def load(path):
    uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
    uc.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_M0)
    uc.mem_map(CODE_BASE, CODE_SIZE)
    uc.mem_map(SHMEM, 0x3000)
    uc.mem_map(PPB, 0x1000)
    uc.mem_map(GPIO4_DR & ~0xFFF, 0x1000)
    uc.mem_map(RET_ADDR, 0x1000)
    with open(path, 'rb') as f:
        elf = ELFFile(f)
        for seg in elf.iter_segments():
            if seg['p_type'] == 'PT_LOAD' and seg['p_filesz']:
                assert seg['p_paddr'] + seg['p_memsz'] <= CODE_SIZE, 'image outside the 4 KB window'
                uc.mem_write(seg['p_paddr'], seg.data().replace(b'\x30\xbf', b'\x00\xbf'))  # wfi -> nop
        symtab = elf.get_section_by_name('.symtab')
        entry = symtab.get_symbol_by_name('m0_play')[0]['st_value']
        state = symtab.get_symbol_by_name('m0_play_state')[0]['st_value']
    uc.mem_write(SCB_ICSR, struct.pack('<I', PENDSTSET))
    return uc, entry, state


def run(path, name, frames, ticks, tm, read_idx=0, underrun_after=None):
    """Returns the number of mismatches."""
    uc, entry, state_addr = load(path)
    for i, (l, r) in enumerate(frames):
        uc.mem_write(SHMEM + HDR + 4 * i, struct.pack('<hh', l, r))
    # write_idx = 2 can never equal a frame-aligned read_idx: ring never empty.
    never_empty = 2
    uc.mem_write(SHMEM + CTRL, struct.pack('<III', 1, never_empty, read_idx))
    uc.mem_write(state_addr + PS_TICKS_BASE, struct.pack('<III', *tm))

    words = []
    state = {'stop_at': ticks}

    def write_idx_at(tick):
        if underrun_after is not None and tick >= underrun_after:
            return state.get('freeze_idx', never_empty)
        return never_empty

    def on_icsr(uc, access, addr, size, value, data):
        uc.mem_write(SCB_ICSR, struct.pack('<I', PENDSTSET))   # always a tick pending

    def on_gpio(uc, access, addr, size, value, data):
        words.append(value & MASK32)
        n = len(words)
        if underrun_after is not None and n == underrun_after:
            # empty the ring: write_idx := the firmware's current read position
            state['freeze_idx'] = model.read_idx
            uc.mem_write(SHMEM + WRITE_IDX, struct.pack('<I', model.read_idx))
        if n >= state['stop_at']:
            uc.mem_write(SHMEM + CTRL, struct.pack('<I', 0))

    uc.hook_add(UC_HOOK_MEM_READ, on_icsr, begin=SCB_ICSR, end=SCB_ICSR + 3)
    uc.hook_add(UC_HOOK_MEM_WRITE, on_icsr, begin=SCB_ICSR, end=SCB_ICSR + 3)
    uc.hook_add(UC_HOOK_MEM_WRITE, on_gpio, begin=GPIO4_DR, end=GPIO4_DR + 3)

    model = Model(frames, read_idx, write_idx_at, tm)
    saved = {UC_ARM_REG_R4: 0x44444444, UC_ARM_REG_R5: 0x55555555, UC_ARM_REG_R6: 0x66666666,
             UC_ARM_REG_R7: 0x77777777, UC_ARM_REG_R8: 0x88888888, UC_ARM_REG_R9: 0x99999999,
             UC_ARM_REG_R10: 0xAAAAAAAA, UC_ARM_REG_R11: 0xBBBBBBBB}
    for reg, val in saved.items():
        uc.reg_write(reg, val)
    uc.reg_write(UC_ARM_REG_SP, STACK_TOP)
    uc.reg_write(UC_ARM_REG_LR, RET_ADDR | 1)
    uc.emu_start(entry | 1, RET_ADDR, count=ticks * 400 + 100000)

    bad = 0
    expect = [model.tick() for _ in range(len(words))]
    for i, (got, want) in enumerate(zip(words, expect)):
        if got != want:
            if bad < 5:
                print(f'  {name}: tick {i}: GPIO {got:#010x}, model {want:#010x}')
            bad += 1
    if len(words) < ticks:
        print(f'  {name}: only {len(words)} ticks before return')
        bad += 1
    # m0_play must have returned (STOP seen within one sample) and kept r4-r11/sp
    if len(words) > ticks + 2 * (tm[0] + 1):
        print(f'  {name}: did not stop: {len(words)} ticks')
        bad += 1
    for reg, val in saved.items():
        if uc.reg_read(reg) != val:
            print(f'  {name}: callee-saved register clobbered')
            bad += 1
    if uc.reg_read(UC_ARM_REG_SP) != STACK_TOP:
        print(f'  {name}: stack not balanced')
        bad += 1
    pub = struct.unpack('<I', uc.mem_read(SHMEM + READ_IDX, 4))[0]
    want_pub = model.published[-1] if model.published else read_idx
    if pub != want_pub:
        print(f'  {name}: published read_idx {pub}, model {want_pub}')
        bad += 1
    ones_l = sum((w >> 10) & 1 for w in words) / max(len(words), 1)
    print(f'{name:36s} {len(words):7d} ticks  left duty {ones_l:.4f}  {"FAIL" if bad else "ok"}')
    return bad


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else 'rk3506-m0-audio.elf'
    rnd = random.Random(1)
    n = RING // 4
    sine = [(int(20000 * math.sin(2 * math.pi * k / 64)), int(-12000 * math.sin(2 * math.pi * k / 32))) for k in range(n)]
    noise = [(rnd.randint(-32768, 32767), rnd.randint(-32768, 32767)) for _ in range(n)]
    rails = [(rnd.choice((-32768, 32767)), rnd.choice((-32768, 32767))) for _ in range(n)]
    dc = [(16384, -8000)] * n
    t750k = timing(187500000, 250)      # 15.625 ticks per sample
    t1m = timing(187500000, 188)        # 20.78
    t5 = (5, 0, 48000)                  # the fewest ticks per sample allowed
    bad = 0
    bad += run(path, 'silence', [(0, 0)] * n, 20000, t750k)
    bad += run(path, 'dc', dc, 60000, t750k)
    bad += run(path, 'sine, 750 kHz', sine, 150000, t750k)
    bad += run(path, 'sine, ~1 MHz', sine, 150000, t1m)
    bad += run(path, 'sine, 5 ticks per sample', sine, 60000, t5)
    bad += run(path, 'noise, wraps the ring', noise, 150000, t750k, read_idx=RING - 40)
    bad += run(path, 'rail to rail (clamps)', rails, 150000, t1m)
    bad += run(path, 'underrun holds', sine, 60000, t750k, underrun_after=30000)
    bad += run(path, 'unaligned read_idx', sine, 20000, t750k, read_idx=0x12346)
    if bad:
        print(f'check_play: {bad} FAILED')
        return 1
    print('check_play: ok')
    return 0


if __name__ == '__main__':
    sys.exit(main())
