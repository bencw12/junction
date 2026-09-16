obj-m += junction_as.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all: junction_as.ko as_test

junction_as.ko: junction_as.c junction_as.h
	$(MAKE) -C $(KDIR) M=$(PWD) modules

as_test: as_test.c junction_as.h
	$(CC) -O2 -g -Wall -Wextra -o $@ $<

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f as_test

load: junction_as.ko
	sudo insmod ./junction_as.ko

unload:
	sudo rmmod junction_as

reload: unload load

.PHONY: all clean load unload reload
