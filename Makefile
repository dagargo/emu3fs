ifneq ($(KERNELRELEASE),)
include Kbuild

else
KDIR ?= /lib/modules/`uname -r`/build

all:
	$(MAKE) -C $(KDIR) M=$$PWD

clean:
	$(MAKE) -C $(KDIR) M=$$PWD clean
	rm -rf *~

format:
	indent -linux *.[ch]

install:
	$(MAKE) -C $(KDIR) M=$$PWD modules_install
endif
