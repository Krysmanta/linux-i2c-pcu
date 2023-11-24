obj-m = i2c-pcu.o

KVERSION = $(shell uname -r)
all:
	make -C /lib/modules/$(KVERSION)/build M=$(PWD) modules
clean:
	make -C /lib/modules/$(KVERSION)/build M=$(PWD) clean
reload:
	rmmod i2c-pcu.ko; insmod i2c-pcu.ko
