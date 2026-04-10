# Copyright (c) 2025 Axelera AI. All rights reserved.

obj-m += metis.o

metis-y += axl-aipu-core.o axl-aipu-edma.o axl-aipu-edma-debugfs.o axl-aipu-hdma.o axl-aipu-hdma-debugfs.o axl-aipu-ioctl.o axl-aipu-msi.o axl-aipu-msi-metis.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	make -C $(KDIR) M=$(PWD) modules

modules_install:
	make -C $(KDIR) M=$(PWD) CONFIG_MODULE_SIG_ALL= modules_install

clean:
	make -C $(KDIR) M=$(PWD) clean
