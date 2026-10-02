#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Run m0_play() from the built ELF in a CPU emulator and compare every GPIO
write with an independent model of play.S.

Needs: pip install unicorn pyelftools   (run via `make check-emu`)

WFI is replaced by a no-op, so each pass through a tick is one tick. GPIO4 DR
writes are recorded. The ring is pre-filled and kept non-empty except in the
underrun test. This checks the arithmetic and the per-sample schedule; timing
on the real core is what PROFILE=1 and picocalc_m0_diag_fw are for.
"""
import math
import random
import struct
import sys

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_MEM_WRITE
from unicorn.arm_const import (UC_CPU_ARM_CORTEX_M0, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_R4,
                               UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7, UC_ARM_REG_R8,
                               UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11)

# Hardware constants, written out independently of the firmware headers.
CODE_BASE, CODE_SIZE = 0x00000000, 0x2000   # M0 view of its image
SHMEM = 0xFFF81000
HDR = 64
RING = 8192
STACK_TOP = CODE_BASE + CODE_SIZE
PPB = 0xE000E000
GPIO4_DR = 0xFF1E0000
RET_ADDR = 0x10000000
CTRL, WRITE_IDX, READ_IDX = 4, 8, 12
PS_FRAC, PS_PLAIN = 4, 8                    # m0_play_state: what main.c sets
PS_SHIFT, PS_DX_MASK = 72, 76
BUS_STEPS, STEPS = 6, 15                    # a PROFILE build has three more after the bus steps
MAX_SHIFT = 6
TRACE = SHMEM + 0x2100                      # PROFILE builds: event log
TRACE_UNDERRUN = 30

SAMPLE_RATE = 48000
FS_SHIFT, CLAMP_SHIFT = 15, 20
MASK32 = 0xFFFFFFFF
IDX_MASK = RING - 4


def timing(core_hz, tick_cycles):
    """(whole ticks per sample, fraction in 2^-32 tick), as the host computes them."""
    den = tick_cycles * SAMPLE_RATE
    return core_hz // den, ((core_hz % den) << 32) // den


def s32(v):
    v &= MASK32
    return v - (1 << 32) if v & 0x80000000 else v


def scale(x):
    return x - (x >> 3)


class Model:
    """Tick-level model of play.S, written from its header comment.

    Each of the first `steps` ticks of a sample does one step after its
    modulator pass; the remaining ticks are plain."""

    def __init__(self, frames, read_idx, write_idx_fn, tm, steps, shift, interp):
        self.frames = frames            # list of (l, r), index = ring offset / 4
        self.write_idx_fn = write_idx_fn
        self.base, self.frac = tm
        self.steps = steps
        self.cur = ((read_idx & IDX_MASK) - 4) & IDX_MASK
        self.widx = 0
        self.frame = (0, 0)
        self.pub = read_idx & IDX_MASK
        self.published = read_idx
        self.acc = 0
        self.shift = shift
        self.interp = interp
        self.fs = 1 << (FS_SHIFT + shift)
        self.x = [0, 0]                 # modulator input, in 2^-shift LSB
        self.dx = [0, 0]                # and what it changes by per tick
        self.new = [0, 0]               # the sample just fetched (scaled 7/8)
        self.prev = [0, 0]              # the one before it
        self.next_x = [0, 0]
        self.next_dx = [0, 0]
        self.i1 = [0, 0]
        self.i2 = [0, 0]
        self.g = [self.fs, -self.fs]
        self.word = 0x0C000000
        self.empties = []               # sample numbers at which the ring was empty
        self.sample_no = 0
        self.pos = 0                    # tick number within the sample
        self.length = None              # ticks in this sample, known from step 2
        self.tick_no = 0
        self.stopped = False

    def clamp(self, ch):
        lim = 1 << (CLAMP_SHIFT + self.shift)
        if not -lim <= self.i2[ch] < lim:
            self.i2[ch] = lim - 1 if self.i2[ch] >= 0 else -lim

    def sample_in(self, ch):
        self.new[ch] = scale(self.frame[ch])
        self.next_dx[ch] = self.new[ch] - self.prev[ch]

    def sample_start(self, ch):
        self.next_x[ch] = self.prev[ch] << self.shift
        self.prev[ch] = self.new[ch]

    def sample_go(self, ch):
        self.x[ch] = self.next_x[ch]
        self.dx[ch] = self.next_dx[ch] if self.interp else 0

    def step(self, n, ctrl):
        profile = self.steps - STEPS    # 3 in a PROFILE build
        if n == 1:
            if ctrl != 1:
                self.stopped = True
        elif n == 2:
            self.widx = self.write_idx_fn(self.tick_no)
        elif n == 3:
            nxt = (self.cur + 4) & IDX_MASK
            if nxt != self.widx:
                self.cur = nxt
            else:
                self.empties.append(self.sample_no)
        elif n == 4:
            self.frame = self.frames[self.cur // 4]
        elif n == 5:
            self.pub = (self.cur + 4) & IDX_MASK
        elif n == 6:
            self.published = self.pub
        elif n <= BUS_STEPS + profile:
            # PROFILE builds: two steps publish statistics, the third counts
            if n == BUS_STEPS + 3:
                self.sample_no += 1
        else:
            n -= BUS_STEPS + profile
            if n == 1:
                self.acc += self.frac
                carry = self.acc >> 32
                self.acc &= MASK32
                self.length = self.base + carry
            elif n == 2:
                self.sample_in(0)
            elif n == 3:
                self.sample_start(0)
            elif n == 4:
                self.sample_in(1)
            elif n == 5:
                self.sample_start(1)
            elif n == 6:
                self.sample_go(0)
            elif n == 7:
                self.sample_go(1)
            elif n == 8:
                self.clamp(0)
            elif n == 9:
                self.clamp(1)

    def tick(self, ctrl):
        """Returns the GPIO word written at this tick."""
        out = self.word
        word = 0x0C000C00
        for ch, bit in ((0, 10), (1, 11)):
            self.i1[ch] = s32(self.i1[ch] + self.x[ch] - self.g[ch])
            self.x[ch] = s32(self.x[ch] + self.dx[ch])
            y = s32(self.i2[ch] + self.i1[ch])
            s = -1 if y < 0 else 0
            self.g[ch] = -self.fs - 1 if s else self.fs     # FS ^ s
            self.i2[ch] = s32(y - self.g[ch])
            if s:
                word -= 1 << bit
        self.word = word
        self.pos += 1
        if self.pos <= self.steps:
            self.step(self.pos, ctrl)
        if self.length is not None and self.pos >= self.length:
            self.pos = 0
            self.length = None
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
                assert seg['p_paddr'] + seg['p_memsz'] <= CODE_SIZE, 'image outside the 8 KB window'
                uc.mem_write(seg['p_paddr'], seg.data().replace(b'\x30\xbf', b'\x00\xbf'))  # wfi -> nop
        symtab = elf.get_section_by_name('.symtab')
        entry = symtab.get_symbol_by_name('m0_play')[0]['st_value']
        state = symtab.get_symbol_by_name('m0_play_state')[0]['st_value']
        steps = symtab.get_symbol_by_name('m0_step_ticks')[0]['st_value']
    return uc, entry, state, steps


def run(path, name, frames, ticks, tm, read_idx=0, underrun_after=None, interp=True):
    """Returns the number of mismatches."""
    uc, entry, state_addr, steps = load(path)
    # as main.c does: the smallest shift the longest sample fits in
    shift = 0
    while (1 << shift) < tm[0] + 1:
        shift += 1
    if shift > MAX_SHIFT:
        interp = False
    if not interp:
        shift = 0
    for i, (l, r) in enumerate(frames):
        uc.mem_write(SHMEM + HDR + 4 * i, struct.pack('<hh', l, r))
    # write_idx = 2 can never equal a frame-aligned index: ring never empty.
    never_empty = 2
    uc.mem_write(SHMEM + CTRL, struct.pack('<III', 1, never_empty, read_idx))
    assert tm[0] > steps, 'sample shorter than its step ticks'
    uc.mem_write(state_addr + PS_FRAC, struct.pack('<II', tm[1], tm[0] - steps))
    uc.mem_write(state_addr + PS_SHIFT, struct.pack('<II', shift, MASK32 if interp else 0))

    words = []
    ctrl_at = []                        # ctrl as the firmware would read it after tick n
    state = {'stop_at': ticks}

    def write_idx_at(tick):
        # exactly what the firmware reads in the same tick
        return struct.unpack('<I', uc.mem_read(SHMEM + WRITE_IDX, 4))[0]

    def on_gpio(uc, access, addr, size, value, data):
        words.append(value & MASK32)
        n = len(words)
        if underrun_after is not None and n == underrun_after:
            # the host "stops writing": two frames ahead of the one being
            # played, so the ring runs empty within two samples whichever
            # step of the sample this tick happens to be
            uc.mem_write(SHMEM + WRITE_IDX, struct.pack('<I', (model.cur + 8) & IDX_MASK))
        if n >= state['stop_at']:
            uc.mem_write(SHMEM + CTRL, struct.pack('<I', 0))
        ctrl_at.append(0 if n >= state['stop_at'] else 1)
        # keep the model in step so the hooks above can look at its state
        expect.append(model.tick(ctrl_at[-1]))

    uc.hook_add(UC_HOOK_MEM_WRITE, on_gpio, begin=GPIO4_DR, end=GPIO4_DR + 3)

    model = Model(frames, read_idx, write_idx_at, tm, steps, shift, interp)
    expect = []
    saved = {UC_ARM_REG_R4: 0x44444444, UC_ARM_REG_R5: 0x55555555, UC_ARM_REG_R6: 0x66666666,
             UC_ARM_REG_R7: 0x77777777, UC_ARM_REG_R8: 0x88888888, UC_ARM_REG_R9: 0x99999999,
             UC_ARM_REG_R10: 0xAAAAAAAA, UC_ARM_REG_R11: 0xBBBBBBBB}
    for reg, val in saved.items():
        uc.reg_write(reg, val)
    uc.reg_write(UC_ARM_REG_SP, STACK_TOP)
    uc.reg_write(UC_ARM_REG_LR, RET_ADDR | 1)
    uc.emu_start(entry | 1, RET_ADDR, count=ticks * 400 + 100000)

    bad = 0
    for i, (got, want) in enumerate(zip(words, expect)):
        if got != want:
            if bad < 5:
                print(f'  {name}: tick {i}: GPIO {got:#010x}, model {want:#010x}')
            bad += 1
    if len(words) < ticks:
        print(f'  {name}: only {len(words)} ticks before return')
        bad += 1
    # m0_play must have returned (STOP seen within one sample) and kept r4-r11/sp
    if not model.stopped:
        print(f'  {name}: firmware returned after {len(words)} ticks, model has not stopped')
        bad += 1
    if len(words) > ticks + tm[0] + 1:
        print(f'  {name}: did not stop within a sample: {len(words)} ticks')
        bad += 1
    for reg, val in saved.items():
        if uc.reg_read(reg) != val:
            print(f'  {name}: callee-saved register clobbered')
            bad += 1
    if uc.reg_read(UC_ARM_REG_SP) != STACK_TOP:
        print(f'  {name}: stack pointer not restored')
        bad += 1
    pub = struct.unpack('<I', uc.mem_read(SHMEM + READ_IDX, 4))[0]
    if pub != model.published:
        print(f'  {name}: published read_idx {pub}, model {model.published}')
        bad += 1
    if steps > STEPS:
        # PROFILE build: nothing overruns here, so the log is the empty-ring events
        total = struct.unpack('<I', uc.mem_read(TRACE, 4))[0]
        if total != len(model.empties):
            print(f'  {name}: {total} events logged, model has {len(model.empties)} empty-ring samples')
            bad += 1
        elif total:
            entry = struct.unpack('<I', uc.mem_read(TRACE + 4 + 4 * (total & 255), 4))[0]
            if entry != (model.empties[-1] << 5 | TRACE_UNDERRUN):
                print(f'  {name}: last log entry {entry:#x}, model sample {model.empties[-1]}')
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
    t915k = timing(187500000, 205)      # 19.05 ticks per sample: the shortest allowed
    t1m2 = timing(187500000, 156)       # 25.04
    t2m4 = timing(187500000, 78)        # 50.08
    t2m9 = timing(187500000, 64)        # 61.04
    t3m0 = timing(187500000, 62)        # 63.004: now and then 64 ticks, all the shift allows
    t3m7 = timing(187500000, 50)        # 78.1: too many ticks to interpolate
    bad = 0
    bad += run(path, 'silence', [(0, 0)] * n, 20000, t1m2)
    bad += run(path, 'dc', dc, 60000, t1m2)
    bad += run(path, 'sine, 915 kHz', sine, 150000, t915k)
    bad += run(path, 'sine, 1.2 MHz', sine, 150000, t1m2)
    bad += run(path, 'sine, 1.2 MHz, held', sine, 150000, t1m2, interp=False)
    bad += run(path, 'sine, 2.4 MHz', sine, 150000, t2m4)
    bad += run(path, 'sine, 2.9 MHz', sine, 250000, t2m9)
    bad += run(path, 'sine, 3.0 MHz', sine, 250000, t3m0)
    bad += run(path, 'sine, 3.75 MHz (holds)', sine, 250000, t3m7)
    bad += run(path, 'noise, wraps the ring', noise, 250000, t1m2, read_idx=RING - 40)
    bad += run(path, 'noise, 3.0 MHz', noise, 400000, t3m0)
    bad += run(path, 'rail to rail (clamps)', rails, 250000, t2m4)
    bad += run(path, 'rail to rail, 3.0 MHz', rails, 400000, t3m0)
    bad += run(path, 'underrun holds', sine, 60000, t1m2, underrun_after=30000)
    bad += run(path, 'unaligned read_idx', sine, 20000, t1m2, read_idx=0x12346)
    if bad:
        print(f'check_play: {bad} FAILED')
        return 1
    print('check_play: ok')
    return 0


if __name__ == '__main__':
    sys.exit(main())
