# SPDX-License-Identifier: GPL-2.0
# RK3506 Cortex-M0 bring-up diagnostics (see README.md). Separate from the
# audio firmware; it borrows that firmware's startup code, linker script and
# register header so the two cannot drift apart on how the M0 is started.

CROSS_COMPILE ?= arm-none-eabi-
CC      = $(CROSS_COMPILE)gcc

AUDIO   = ../picocalc_m0_audio_fw

CFLAGS  = -mcpu=cortex-m0 -mthumb -Os -ffunction-sections -fdata-sections \
          -Wall -Wextra -nostdlib -I. -I$(AUDIO)
LDFLAGS = -T $(AUDIO)/rk3506-m0-audio.ld -nostdlib -Wl,--gc-sections

OBJS = startup.o main.o loops.o resource_table.o
ELF  = rk3506-m0-diag.elf
HDRS = diag.h $(AUDIO)/rk3506_regs.h

all: $(ELF)

$(ELF): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS)

startup.o: $(AUDIO)/startup.S
	$(CC) $(CFLAGS) -c -o $@ $<

resource_table.o: $(AUDIO)/resource_table.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.S
	$(CC) $(CFLAGS) -c -o $@ $<

# Linux-side reader. Cross-compile for the board, e.g.
#   make m0diag LINUX_CC=arm-linux-gnueabihf-gcc
LINUX_CC ?= cc

m0diag: m0diag.c diag.h
	$(LINUX_CC) -O2 -Wall -Wextra -o $@ $<

# Runs the firmware in a CPU emulator with a modelled timer and checks the
# results block. Needs: pip install unicorn pyelftools
PYTHON ?= python3

check-emu: $(ELF)
	$(PYTHON) check_diag.py $(ELF)

clean:
	rm -f $(OBJS) $(ELF) m0diag

.PHONY: all clean check-emu
