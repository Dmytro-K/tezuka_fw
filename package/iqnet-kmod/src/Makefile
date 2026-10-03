# SPDX-License-Identifier: GPL-2.0
#
# iqnet: zero-copy UDP streaming of IIO DMA blocks.
#
# Out-of-tree kbuild module. Buildroot's kernel-module infrastructure runs
#   $(MAKE) -C $(LINUX_DIR) $(LINUX_MAKE_FLAGS) M=<this dir> modules
# Standalone:
#   make KERNELDIR=/path/to/linux ARCH=arm CROSS_COMPILE=arm-linux-

ifneq ($(KERNELRELEASE),)

# One module, iqnet.ko, from two objects. kbuild does not allow a composite
# module to list an object of its own name, hence iqnet_main.c.
obj-m := iqnet.o
iqnet-y := iqnet_main.o iqnet_pl.o

else

KERNELDIR ?= /lib/modules/$(shell uname -r)/build
M ?= $(CURDIR)

all: modules

modules modules_install clean:
	$(MAKE) -C $(KERNELDIR) M=$(M) $@

.PHONY: all modules modules_install clean

endif
