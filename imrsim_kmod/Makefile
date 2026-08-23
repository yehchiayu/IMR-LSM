NAM_PROJ = imrsim
KERNVER  = $(shell uname -r)
D_KERNEL_SOURCE = /usr/src/linux-$(KERNVER)

ccflags-y :=-g -I$(src) -I$(D_KERNEL_SOURCE)/drivers/md/
obj-m           += dm-$(NAM_PROJ).o

all:
	$(MAKE) -C /lib/modules/$(KERNVER)/build M=$(CURDIR) modules
clean:
	$(MAKE) -C /lib/modules/$(KERNVER)/build M=$(CURDIR) clean
modules_install:
	$(MAKE) INSTALL_MOD_DIR=kernel/drivers/md -C /lib/modules/$(KERNVER)/build M=$(CURDIR) modules_install
