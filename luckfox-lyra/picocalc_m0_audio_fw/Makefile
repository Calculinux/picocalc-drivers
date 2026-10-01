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

OBJS = startup.o main.o isr.o resource_table.o
ELF  = rk3506-m0-audio.elf

all: $(ELF)

$(ELF): $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.S
	$(CC) $(CFLAGS) -c -o $@ $<

HOSTCC ?= cc

check: check_ring
	./check_ring

check_ring: check_ring.c
	$(HOSTCC) -O2 -Wall -Wextra -o $@ $<

# Host check of the ISR DSM maths. NOTE: the default stimulus (1, DC +FS)
# currently FAILS (non-zero exit) by design — it encodes the spec that
# finding B3 in docs/m0-audio-review.md violates. Re-run after any isr.S DSM
# change.
check_dsm: check_dsm.c
	$(HOSTCC) -O2 -Wall -Wextra -lm -o $@ $<

clean:
	rm -f $(OBJS) $(ELF) check_ring check_dsm

.PHONY: all clean check
