# SPDX-License-Identifier: GPL-2.0

ifneq ($(KERNELRELEASE),)

obj-m := lazy_cma.o

else

KDIR ?= /lib/modules/$(shell uname -r)/build

all: modules lazy_cma_tool

modules:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

lazy_cma_tool: lazy_cma_tool.c
	$(CC) -Wall -O2 -o $@ $<

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	$(RM) lazy_cma_tool

endif
