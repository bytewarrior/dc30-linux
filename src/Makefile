# SPDX-License-Identifier: GPL-2.0-only

obj-m := dc30.o dc30_vpx3220.o
dc30-y := dc30_core.o dc30_i2c.o dc30_power.o zr36057.o dc30_video.o dc30_audio.o dc30_alsa.o \
	  dc30_jpeg.o zr36050.o zr36016.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
	depmod -a

.PHONY: all clean install
