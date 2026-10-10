obj-m := kjump.o
kjump-objs := kjump_main.o trampoline.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

default:
	$(MAKE) -C $(KDIR) M=$(PWD) ARCH=mips CROSS_COMPILE=mipsel-linux-gnu- modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
