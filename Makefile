KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

obj-m += osfs.o

osfs-objs := super.o inode.o file.o dir.o osfs_init.o

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

load:
	sudo insmod osfs.ko

unload:
	sudo rmmod osfs

mount:
	mkdir -p mnt
	sudo mount -t osfs none mnt/

umount:
	sudo umount mnt/
	rmdir mnt