obj-m += emu3_fs.o
emu3_fs-objs := super.o inode.o file.o dir.o
MOD_PATH := /lib/modules/$(shell uname -r)/build

all:
	make -C $(MOD_PATH) M=$(PWD) modules

clean:
	make -C $(MOD_PATH) M=$(PWD) clean
	rm -rf *~

format:
	indent -nbad -bap -nbc -bbo -hnl -br -brs -c33 -cd33 -ncdb -ce -ci4 -cli0 -d0 -di1 -nfc1 -i8 -ip0 -l80 -lp -npcs -nprs -npsl -sai -saf -saw -ncs -nsc -sob -nfca -cp33 -ss -ts8 -il1 *.[ch]

install:
	make -C $(MOD_PATH) M=$(shell pwd) modules_install
	depmod -a

