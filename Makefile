obj-m += temperhum.o 
KVERSION = /lib/modules/$(shell uname -r)
CC = $(CROSS_COMPILE)gcc
all:
	make -C $(KVERSION)/build M=$(PWD) modules

clean:
	make -C $(KVERSION)/build M=$(PWD) clean
