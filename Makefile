obj-m += ouichefs.o
ouichefs-objs := fs.o super.o inode.o file.o dir.o snapshot.o walk.o sysfs.o

KERNELDIR ?= /lib/modules/$(shell uname -r)/build

.PHONY: all
all: mkfs modules

.PHONY: modules
modules:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

.PHONY: debug
debug:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) ccflags-y+="-DDEBUG -g" modules

.PHONY: clean
clean:
	$(MAKE) -C mkfs clean
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean
	rm -rf *~

.PHONY: mkfs
mkfs:
	$(MAKE) -C mkfs

.PHONY: install
install: modules_install mkfs_install

.PHONY: modules_install
modules_install:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules_install

.PHONY: mkfs_install
mkfs_install:
	$(MAKE) -C mkfs install
