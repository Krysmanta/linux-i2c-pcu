obj-m = i2c-pcu.o

KVERSION = $(shell uname -r)

DKMS_NAME?=i2c-pcu
DKMS_VER?=0.2.0

all:
	make -C /lib/modules/$(KVERSION)/build M=$(PWD) modules
clean:
	make -C /lib/modules/$(KVERSION)/build M=$(PWD) clean
reload:
	rmmod i2c-pcu.ko; insmod i2c-pcu.ko

# DKMS targets
setup_dkms:
	@echo -e "\n::\033[34m Installing DKMS files\033[0m"
	@echo "====================================================="
	install -m 644 -v -D Makefile $(DESTDIR)/usr/src/$(DKMS_NAME)-$(DKMS_VER)/Makefile
	install -m 644 -v -D dkms/dkms.conf $(DESTDIR)/usr/src/$(DKMS_NAME)-$(DKMS_VER)/dkms.conf
	install -m 644 -v *.c $(DESTDIR)/usr/src/$(DKMS_NAME)-$(DKMS_VER)/

remove_dkms:
	@echo -e "\n::\033[34m Removing DKMS files\033[0m"
	@echo "====================================================="
	rm -rf $(DESTDIR)/usr/src/$(DKMS_NAME)-$(DKMS_VER)
