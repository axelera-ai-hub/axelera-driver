# Copyright (c) 2025 Axelera AI. All rights reserved.
obj-m += metis.o

metis-y += metis-core.o metis-edma.o metis-edma-debugfs.o metis-hdma.o metis-hdma-debugfs.o metis-ioctl.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	make -C $(KDIR) M=$(PWD) modules

modules_install:
	make -C $(KDIR) M=$(PWD) modules_install

clean:
	make -C $(KDIR) M=$(PWD) clean