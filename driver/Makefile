# SPDX-License-Identifier: GPL-2.0-only
obj-m += rpi3_gpio_driver.o

# KDIR must point to the configured & built source tree (or headers package)
# of the *target* kernel (RPi3, arm64):
#   - native build, run directly on the Pi : /lib/modules/$(shell uname -r)/build
#     (provided by the raspberrypi-kernel-headers package), then also
#     override ARCH= and CROSS_COMPILE= (empty) on the command line.
#   - cross build under WSL2               : path to a configured/built
#     `raspberrypi/linux` tree, matching the kernel actually running on the Pi.
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

ARCH          ?= arm64
CROSS_COMPILE ?= aarch64-linux-gnu-

all:
	$(MAKE) -C $(KDIR) M=$(PWD) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

modules_install:
	$(MAKE) -C $(KDIR) M=$(PWD) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules_install

.PHONY: all clean modules_install
