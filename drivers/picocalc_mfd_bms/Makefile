# While on staging, keep debug enabled
DEFINES += -DDEBUG

obj-m += picocalc_mfd_bms.o

# Kernel build targets
KERNEL_SRC ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KERNEL_SRC) M=$(PWD) modules

clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(PWD) clean

install:
	$(MAKE) -C $(KERNEL_SRC) M=$(PWD) modules_install
