# SPDX-License-Identifier: GPL-2.0
# PicoCalc M0 delta-sigma audio (ALSA)

obj-m += picocalc_snd_m0.o

KERNEL_SRC ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KERNEL_SRC) M=$(PWD) modules

clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(PWD) clean

install:
	$(MAKE) -C $(KERNEL_SRC) M=$(PWD) modules_install
