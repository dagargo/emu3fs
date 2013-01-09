obj-m += emu3_fs.o
emu3_fs-objs := super.o inode.o file.o dir.o
MOD_PATH := /lib/modules/$(shell uname -r)/build

all:
	make -C $(MOD_PATH) M=$(PWD) modules

clean:
	make -C $(MOD_PATH) M=$(PWD) clean
	rm -rf *~

install:
	make -C $(MOD_PATH) M=$(shell pwd) modules_install
	depmod -a

