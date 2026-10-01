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

# make TICK_WFI=1: sleep in WFI between ticks instead of polling the timer
# status bit (see play.S). Polling is the default because it depends on
# nothing but the timer.
ifeq ($(TICK_WFI),1)
CFLAGS += -DM0_TICK_WFI
endif

OBJS = startup.o main.o play.o resource_table.o
ELF  = rk3506-m0-audio.elf

all: $(ELF)

$(ELF): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS)

HDRS = rk3506_regs.h shmem.h

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

check_dsm: check_dsm.c $(HDRS)
	$(HOSTCC) -O2 -Wall -Wextra -o $@ $< -lm

# Runs m0_play() from the built ELF in a CPU emulator and compares every GPIO
# write with a model. Needs: pip install unicorn pyelftools
PYTHON ?= python3

check-emu: $(ELF)
	$(PYTHON) check_play.py $(ELF)

clean:
	rm -f $(OBJS) $(ELF) check_ring check_dsm

.PHONY: all clean check check-emu
