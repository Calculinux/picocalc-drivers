# SPDX-License-Identifier: GPL-2.0
# M0 delta-sigma audio firmware for RK3506 (Cortex-M0)

CROSS_COMPILE ?= arm-none-eabi-
CC      = $(CROSS_COMPILE)gcc
AS      = $(CROSS_COMPILE)as
LD      = $(CROSS_COMPILE)ld
OBJCOPY = $(CROSS_COMPILE)objcopy
OBJDUMP = $(CROSS_COMPILE)objdump

CFLAGS  = -mcpu=cortex-m0 -mthumb -Os -ffunction-sections -fdata-sections \
          -Wall -Wextra -nostdlib -I.
LDFLAGS = -T rk3506-m0-audio.ld -nostdlib -Wl,--gc-sections

# make PROFILE=1: record the longest tick and count overruns (see play.S);
# the host driver reports them when playback stops. Costs a few cycles per
# tick, so not for normal use.
ifeq ($(PROFILE),1)
CFLAGS += -DM0_PROFILE
endif

# make COMP_STATS=1: the corrected loop (m0_play_comp) also adds up how late
# its pin writes were, which the host driver reports when playback stops.
# Costs 5 of that loop's spare cycles per tick.
# make COMP_STATS=2: it counts instead how often each lateness occurred (10
# cycles); the 32 counts, for 0, 4, 8, ... cycles late, are left at
# M0_TRACE_ADDR when a stream ends.
ifneq ($(COMP_STATS),)
CFLAGS += -DM0_COMP_STATS=$(COMP_STATS)
endif

OBJS = startup.o main.o play.o resource_table.o
ELF  = rk3506-m0-audio.elf

all: $(ELF)

$(ELF): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS)

HDRS = rk3506_regs.h shmem.h play.h

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.S $(HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

HOSTCC ?= cc

check: check_ring check_dsm
	./check_ring
	./check_dsm

check_ring: check_ring.c
	$(HOSTCC) -O2 -Wall -Wextra -o $@ $<

# Reader for the PROFILE build's event log. Cross-compile for the board, e.g.
#   make m0trace LINUX_CC=arm-linux-gnueabihf-gcc
LINUX_CC ?= cc

m0trace: m0trace.c
	$(LINUX_CC) -O2 -Wall -Wextra -o $@ $<

# Host accesses to a bus slave, to see what they cost the M0
m0hammer: m0hammer.c
	$(LINUX_CC) -O2 -Wall -Wextra -o $@ $<

# The output level actually on the pins, read back from Linux
m0level: m0level.c
	$(LINUX_CC) -O2 -Wall -Wextra -o $@ $< -lm

check_dsm: check_dsm.c $(HDRS)
	$(HOSTCC) -O2 -Wall -Wextra -o $@ $< -lm

# Runs m0_play() from the built ELF in a CPU emulator and compares every GPIO
# write with a model. Needs: pip install unicorn pyelftools
PYTHON ?= python3

check-emu: $(ELF)
	$(PYTHON) check_play.py $(ELF)

clean:
	rm -f $(OBJS) $(ELF) check_ring check_dsm m0trace m0hammer m0level

.PHONY: all clean check check-emu
