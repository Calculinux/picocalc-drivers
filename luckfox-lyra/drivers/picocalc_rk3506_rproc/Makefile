# SPDX-License-Identifier: GPL-2.0
# RK3506 Cortex-M0 remoteproc driver (nvitya/rk3506-mcu port)

obj-m += rk3506_rproc.o

KERNEL_SRC ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KERNEL_SRC) M=$(PWD) modules

clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(PWD) clean

install:
	$(MAKE) -C $(KERNEL_SRC) M=$(PWD) modules_install
