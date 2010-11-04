#!/bin/sh
# fixes case where symlink of usb device node can't be made by udev
# D. P. Storey 19/07/10

if [ "$1" = "add" ];then
	echo -n $(basename $2) | tee /tmp/temperhum.txt > /sys/bus/usb/drivers/usbhid/unbind
	CMD="ln -fs /sys$2 /dev/$(basename $3)"
fi

if [ "$1" = "remove" ]; then
	CMD="rm /dev/$(basename $2)"
fi

$CMD 
echo "$(basename $0) ($1): $CMD ($3 $4)" | tee -a /tmp/temperhum.txt >> /tmp/$(basename $0)

