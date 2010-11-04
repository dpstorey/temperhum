/*
 * USB temperhum driver - 1.0
 *
 * Copyright (C) 2010 Dominic Storey (dstorey@barossafarm.com
 * based on code by Greg Kroah-Hartman (greg@kroah.com)
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License as
 *	published by the Free Software Foundation, version 2.
 *
 */

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/string.h>
#include <linux/ctype.h>
#include <linux/unistd.h>
#include <linux/string.h>
#include <linux/delay.h>
//#include <linux/bounds.h>

#define DRIVER_AUTHOR "Dominic Storey (dstorey@barossafarm.com)"
#define DRIVER_DESC "Tempur Temperature and Humidity Sensor"

#define VENDOR_ID	0x1130
#define PRODUCT_ID	0x660c
#define TEMPERBUFSIZ	32
#define USBTENKI_GETRAW	0x10
#define TEMP_COMMAND	0
#define RH_COMMAND	1
#define RECV_BYTES	4

#define USB_REQ_GET_REPORT	1
#define USB_REQ_SET_REPORT	9

static DEFINE_MUTEX(temperhum_lock);
	
/* table of devices that work with this driver */
static struct usb_device_id id_table [] = {
	{ USB_DEVICE(VENDOR_ID, PRODUCT_ID) },
	{ },
};
MODULE_DEVICE_TABLE (usb, id_table);

struct usb_temperhum {
	struct usb_device *	udev;
	long	 		T,
				rh;
	char			unit;	/* 'C' or 'F' for centrigrade or farenheight */
};

/*--------------------------------------------------------------------------------------------*/

static ssize_t temperhum_set(struct device *dev, struct device_attribute *attr, char *buf, size_t len)
{
	struct usb_interface *intf = to_usb_interface(dev);
	struct usb_temperhum *temperhumdev = usb_get_intfdata(intf); 

	mutex_lock(&temperhum_lock);
	if ((toupper(buf[0]) == 'C'))
		temperhumdev->unit = 'C';
	if ((toupper(buf[0]) == 'F'))
		temperhumdev->unit = 'F';
	buf[0] = temperhumdev->unit;
	buf[1] = '\n';

	mutex_unlock(&temperhum_lock);
	return 2;
}

/*--------------------------------------------------------------------------------------------*/

static int sendcommand(struct usb_temperhum *td, char *data)
{
	unsigned char 	*buf;
	int 		retval;

	buf = kzalloc(TEMPERBUFSIZ, GFP_KERNEL);

	if (!buf) {
		dev_err(&td->udev->dev, "Out of memory\n");
		return 1;
	}

	memcpy(buf, data, 8);
	retval = usb_control_msg(td->udev, usb_sndctrlpipe(td->udev, 0), 
	9, 0x21, 0x200, 1, buf, TEMPERBUFSIZ, 5000);


	kfree(buf);
/*	printk("sendcommand() Cmd Data[] = {%d %d %d %d %d %d %d %d}, retval = %d\n", 
		data[0],data[1], data[2],data[3], data[4],data[5], data[6],data[7], retval);
*/
	if(retval == TEMPERBUFSIZ)
		return 0;
	else
		return 1;
}

/*--------------------------------------------------------------------------------------------*/

static int getdata(struct usb_temperhum *td, int *SOt, int *SOrh, char *buf)
{
	unsigned char 	*rxbuf;
	int 		retval, i;

	char cmd_data[4][8] = {
		{10, 11, 12, 13, 0, 0, 2, 0},
 		{0x48,  0,  0,  0, 0, 0, 0, 0},
		{0,   0,  0,  0, 0, 0, 0, 0},
		{10, 11, 12, 13, 0, 0, 1, 0}
	};



	rxbuf = kmalloc(256, GFP_KERNEL);

	if (!rxbuf) {
		dev_err(&td->udev->dev, "out of memory\n");
		strcpy(buf, "ERROR\n");
		retval = strlen(buf);
		return retval;
	}

	if (sendcommand(td, cmd_data[0])){
		/*printk("initprobe() - first call to sendcommand() failed\n"); */
		return -EINVAL;
	}
	if (sendcommand(td, cmd_data[1])){
		/*printk("initprobe() - second call to sendcommand() failed\n"); */
		return -EINVAL;
	}
		
	for(i = 0; i < 7; ++i)
		if(sendcommand(td, cmd_data[2])){
			/*printk("initprobe() - third call to sendcommand() failed\n"); */
			return -EINVAL;
		}	
	if (sendcommand(td, cmd_data[3])){
		/*printk("initprobe() - third call to sendcommand() failed\n"); */
		return -EINVAL;
	}
	
	msleep(400);


	retval = usb_control_msg(td->udev, usb_rcvctrlpipe(td->udev, 0), 
		1, 0xa1, 0x300, 1, rxbuf, RECV_BYTES, 5000);


/*	printk("td->udev->devnum value = %d, retval = %d\n", td->udev->devnum,retval); */

	if(retval != RECV_BYTES)
		goto error;

/*	printk("setting SOt, SOh\n"); */

	*SOt  = (rxbuf[0] << 8) | (rxbuf[1] & 0xff);
	*SOrh = (rxbuf[2] << 8) | (rxbuf[3] & 0xff);
/*	printk("rxbuf[0..3] = %02x %02x %02x %02x,  SOt = %x, SOrh = %x\n", 
		rxbuf[0], rxbuf[1], rxbuf[2], rxbuf[3], *SOt, *SOrh);
*/	
	kfree(rxbuf);
	return 0;

error:
	dev_err(&td->udev->dev, "USB communication error\n");
	kfree(rxbuf);
	strcpy(buf, "ERROR\n");
	retval = strlen(buf);
	return retval;
}

/*--------------------------------------------------------------------------------------------*/

static ssize_t temperhum_get(struct device *dev, struct device_attribute *attr, char *buf)
{
	int 		SOrh = 0, 		/* sensor raw reading of humidity, 12 bits */ 
			SOt = 0,  		/* sensor raw reading of temp, 14 bits  */
			T,		/* temperature, x 100 */
			rh_unc; 	/* uncorrected relative humidity, x 10,000 */

	ssize_t		retval;

	struct usb_interface *intf = to_usb_interface(dev);
	struct usb_temperhum *td   = usb_get_intfdata(intf); 

/*	printk("Obtaining lock\n"); */

	mutex_lock(&temperhum_lock);

/*	printk("getting data\n"); */


	retval = getdata(td, &SOt, &SOrh, buf);
	if(retval != 0)
		goto error;

/*	printk("got data: S0t = %d, SOrh = %d\n", SOt, SOrh); */

	T = SOt - 4010; 	/* true temp, x 100 */

	/* from datasheet v4 T = d1 +d2 * SOt (where d1=40.1 and d2=0.01) */

	if(!strcmp(attr->attr.name, "t")){ 
		td->T = T;
		if(td->unit == 'F')
			td->T = (td->T * 9 / 5) + 3200;
		retval = sprintf(buf, "%ld.%ld\n", td->T / 100, td->T % 100);
	}
	else { /* humidity */ 
	
		/* uncorrected humidity x 10,000 */
		rh_unc =  367 * (long)SOrh - (20468 + ((long)SOrh * (long)SOrh *100 / 6267));

		td->rh =  (T - 2500) * (1 + (long)SOrh / 125) + rh_unc;
		td->rh = td->rh > 1000000 ? 1000000 : td->rh; 

		/* now normalize to real values to 2 decimal places */
		retval = sprintf(buf, "%ld.%ld\n", td->rh / 10000, (td->rh % 10000) / 100);
	}
error:
	mutex_unlock(&temperhum_lock);
	return retval;
}
/*--------------------------------------------------------------------------------------------*/

static DEVICE_ATTR(t,  (S_IWUGO | S_IRUGO), temperhum_get, temperhum_set);
static DEVICE_ATTR(rh, (S_IWUGO | S_IRUGO), temperhum_get, temperhum_set);

/*--------------------------------------------------------------------------------------------*/

static int temperhum_probe(struct usb_interface *interface, const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(interface);
	struct usb_temperhum *dev = NULL;
	int retval = -ENOMEM;

	dev = kzalloc(sizeof(struct usb_temperhum), GFP_KERNEL);
	if (dev == NULL) {
		dev_err(&interface->dev, "Out of memory\n");
		goto error_mem;
	}
	dev->unit = 'C'; /* set to Centigrade conversion, initially */

	dev->udev = usb_get_dev(udev);

	usb_set_intfdata (interface, dev);

	retval = device_create_file(&interface->dev, &dev_attr_t);
	if (retval)
		goto error;
	retval = device_create_file(&interface->dev, &dev_attr_rh);
	if (retval)
		goto error;

	dev_info(&interface->dev, "Temperhum attached\n");
	return 0;

error:
	device_remove_file(&interface->dev, &dev_attr_t);
	device_remove_file(&interface->dev, &dev_attr_rh);
	usb_set_intfdata (interface, NULL);
	usb_put_dev(dev->udev);
	kfree(dev);
error_mem:
	return retval;
}
/*--------------------------------------------------------------------------------------------*/

static void temperhum_disconnect(struct usb_interface *interface)
{
	struct usb_temperhum *dev;

	dev_info(&interface->dev, "Disconnecting temperhum\n");

	dev = usb_get_intfdata (interface);

	device_remove_file(&interface->dev, &dev_attr_t);
	device_remove_file(&interface->dev, &dev_attr_rh);

	usb_set_intfdata (interface, NULL);

	/*printk("calling usb_put_dev()\n"); */
	usb_put_dev(dev->udev);

	/*printk("calling kfree()\n");*/

	kfree(dev);

	dev_info(&interface->dev, "Temperhum now disconnected\n");
}
/*--------------------------------------------------------------------------------------------*/

static struct usb_driver temperhum_driver = {
	.name =		"temperhum",
	.probe =	temperhum_probe,
	.disconnect =	temperhum_disconnect,
	.id_table =	id_table,
};
/*--------------------------------------------------------------------------------------------*/

static int __init usb_temperhum_init(void)
{
	int retval = 0;

	retval = usb_register(&temperhum_driver);
	if (retval)
		err("usb_register failed. Error number %d", retval);
	return retval;
}
/*--------------------------------------------------------------------------------------------*/

static void __exit usb_temperhum_exit(void)
{
	usb_deregister(&temperhum_driver);
}
/*--------------------------------------------------------------------------------------------*/

module_init (usb_temperhum_init);
module_exit (usb_temperhum_exit);

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");
