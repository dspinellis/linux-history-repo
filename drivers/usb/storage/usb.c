/* Driver for USB Mass Storage compliant devices
 *
 * $Id: usb.c,v 1.75 2002/04/22 03:39:43 mdharm Exp $
 *
 * Current development and maintenance by:
 *   (c) 1999-2003 Matthew Dharm (mdharm-usb@one-eyed-alien.net)
 *
 * Developed with the assistance of:
 *   (c) 2000 David L. Brown, Jr. (usb-storage@davidb.org)
 *   (c) 2003 Alan Stern (stern@rowland.harvard.edu)
 *
 * Initial work by:
 *   (c) 1999 Michael Gee (michael@linuxspecific.com)
 *
 * usb_device_id support by Adam J. Richter (adam@yggdrasil.com):
 *   (c) 2000 Yggdrasil Computing, Inc.
 *
 * This driver is based on the 'USB Mass Storage Class' document. This
 * describes in detail the protocol used to communicate with such
 * devices.  Clearly, the designers had SCSI and ATAPI commands in
 * mind when they created this document.  The commands are all very
 * similar to commands in the SCSI-II and ATAPI specifications.
 *
 * It is important to note that in a number of cases this class
 * exhibits class-specific exemptions from the USB specification.
 * Notably the usage of NAK, STALL and ACK differs from the norm, in
 * that they are used to communicate wait, failed and OK on commands.
 *
 * Also, for certain devices, the interrupt endpoint is used to convey
 * status of a command.
 *
 * Please see http://www.one-eyed-alien.net/~mdharm/linux-usb for more
 * information about this driver.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/sched.h>
#include <linux/errno.h>
#include <linux/freezer.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/kthread.h>
#include <linux/mutex.h>
#include <linux/utsname.h>

#include <scsi/scsi.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_device.h>

#include "usb.h"
#include "scsiglue.h"
#include "transport.h"
#include "protocol.h"
#include "debug.h"
#include "initializers.h"

#ifdef CONFIG_USB_STORAGE_USBAT
#include "shuttle_usbat.h"
#endif
#ifdef CONFIG_USB_STORAGE_SDDR09
#include "sddr09.h"
#endif
#ifdef CONFIG_USB_STORAGE_SDDR55
#include "sddr55.h"
#endif
#ifdef CONFIG_USB_STORAGE_DPCM
#include "dpcm.h"
#endif
#ifdef CONFIG_USB_STORAGE_FREECOM
#include "freecom.h"
#endif
#ifdef CONFIG_USB_STORAGE_ISD200
#include "isd200.h"
#endif
#ifdef CONFIG_USB_STORAGE_DATAFAB
#include "datafab.h"
#endif
#ifdef CONFIG_USB_STORAGE_JUMPSHOT
#include "jumpshot.h"
#endif
#ifdef CONFIG_USB_STORAGE_ONETOUCH
#include "onetouch.h"
#endif
#ifdef CONFIG_USB_STORAGE_ALAUDA
#include "alauda.h"
#endif
#ifdef CONFIG_USB_STORAGE_KARMA
#include "karma.h"
#endif
#ifdef CONFIG_USB_STORAGE_CYPRESS_ATACB
#include "cypress_atacb.h"
#endif

/* Some informational data */
MODULE_AUTHOR("Matthew Dharm <mdharm-usb@one-eyed-alien.net>");
MODULE_DESCRIPTION("USB Mass Storage driver for Linux");
MODULE_LICENSE("GPL");

static unsigned int delay_use = 5;
module_param(delay_use, uint, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(delay_use, "seconds to delay before using a new device");


/*
 * The entries in this table correspond, line for line,
 * with the entries of us_unusual_dev_list[].
 */
#ifndef CONFIG_USB_LIBUSUAL

#define UNUSUAL_DEV(id_vendor, id_product, bcdDeviceMin, bcdDeviceMax, \
		    vendorName, productName,useProtocol, useTransport, \
		    initFunction, flags) \
{ USB_DEVICE_VER(id_vendor, id_product, bcdDeviceMin,bcdDeviceMax), \
  .driver_info = (flags)|(USB_US_TYPE_STOR<<24) }

#define USUAL_DEV(useProto, useTrans, useType) \
{ USB_INTERFACE_INFO(USB_CLASS_MASS_STORAGE, useProto, useTrans), \
  .driver_info = (USB_US_TYPE_STOR<<24) }

static struct usb_device_id storage_usb_ids [] = {

#	include "unusual_devs.h"
#undef UNUSUAL_DEV
#undef USUAL_DEV
	/* Terminating entry */
	{ }
};

MODULE_DEVICE_TABLE (usb, storage_usb_ids);
#endif /* CONFIG_USB_LIBUSUAL */

/* This is the list of devices we recognize, along with their flag data */

/* The vendor name should be kept at eight characters or less, and
 * the product name should be kept at 16 characters or less. If a device
 * has the US_FL_FIX_INQUIRY flag, then the vendor and product names
 * normally generated by a device thorugh the INQUIRY response will be
 * taken from this list, and this is the reason for the above size
 * restriction. However, if the flag is not present, then you
 * are free to use as many characters as you like.
 */

#define UNUSUAL_DEV(idVendor, idProduct, bcdDeviceMin, bcdDeviceMax, \
		    vendor_name, product_name, use_protocol, use_transport, \
		    init_function, Flags) \
{ \
	.vendorName = vendor_name,	\
	.productName = product_name,	\
	.useProtocol = use_protocol,	\
	.useTransport = use_transport,	\
	.initFunction = init_function,	\
}

#define USUAL_DEV(use_protocol, use_transport, use_type) \
{ \
	.useProtocol = use_protocol,	\
	.useTransport = use_transport,	\
}

static struct us_unusual_dev us_unusual_dev_list[] = {
#	include "unusual_devs.h" 
#	undef UNUSUAL_DEV
#	undef USUAL_DEV

	/* Terminating entry */
	{ NULL }
};


#ifdef CONFIG_PM	/* Minimal support for suspend and resume */

static int storage_suspend(struct usb_interface *iface, pm_message_t message)
{
	struct us_data *us = usb_get_intfdata(iface);

	/* Wait until no command is running */
	mutex_lock(&us->dev_mutex);

	US_DEBUGP("%s\n", __FUNCTION__);
	if (us->suspend_resume_hook)
		(us->suspend_resume_hook)(us, US_SUSPEND);

	/* When runtime PM is working, we'll set a flag to indicate
	 * whether we should autoresume when a SCSI request arrives. */

	mutex_unlock(&us->dev_mutex);
	return 0;
}

static int storage_resume(struct usb_interface *iface)
{
	struct us_data *us = usb_get_intfdata(iface);

	mutex_lock(&us->dev_mutex);

	US_DEBUGP("%s\n", __FUNCTION__);
	if (us->suspend_resume_hook)
		(us->suspend_resume_hook)(us, US_RESUME);

	mutex_unlock(&us->dev_mutex);
	return 0;
}

static int storage_reset_resume(struct usb_interface *iface)
{
	struct us_data *us = usb_get_intfdata(iface);

	US_DEBUGP("%s\n", __FUNCTION__);

	/* Report the reset to the SCSI core */
	usb_stor_report_bus_reset(us);

	/* FIXME: Notify the subdrivers that they need to reinitialize
	 * the device */
	return 0;
}

#endif /* CONFIG_PM */

/*
 * The next two routines get called just before and just after
 * a USB port reset, whether from this driver or a different one.
 */

static int storage_pre_reset(struct usb_interface *iface)
{
	struct us_data *us = usb_get_intfdata(iface);

	US_DEBUGP("%s\n", __FUNCTION__);

	/* Make sure no command runs during the reset */
	mutex_lock(&us->dev_mutex);
	return 0;
}

static int storage_post_reset(struct usb_interface *iface)
{
	struct us_data *us = usb_get_intfdata(iface);

	US_DEBUGP("%s\n", __FUNCTION__);

	/* Report the reset to the SCSI core */
	usb_stor_report_bus_reset(us);

	/* FIXME: Notify the subdrivers that they need to reinitialize
	 * the device */

	mutex_unlock(&us->dev_mutex);
	return 0;
}

/*
 * fill_inquiry_response takes an unsigned char array (which must
 * be at least 36 characters) and populates the vendor name,
 * product name, and revision fields. Then the array is copied
 * into the SCSI command's response buffer (oddly enough
 * called request_buffer). data_len contains the length of the
 * data array, which again must be at least 36.
 */

void fill_inquiry_response(struct us_data *us, unsigned char *data,
		unsigned int data_len)
{
	if (data_len<36) // You lose.
		return;

	if(data[0]&0x20) { /* USB device currently not connected. Return
			      peripheral qualifier 001b ("...however, the
			      physical device is not currently connected
			      to this logical unit") and leave vendor and
			      product identification empty. ("If the target
			      does store some of the INQUIRY data on the
			      device, it may return zeros or ASCII spaces 
			      (20h) in those fields until the data is
			      available from the device."). */
		memset(data+8,0,28);
	} else {
		u16 bcdDevice = le16_to_cpu(us->pusb_dev->descriptor.bcdDevice);
		memcpy(data+8, us->unusual_dev->vendorName, 
			strlen(us->unusual_dev->vendorName) > 8 ? 8 :
			strlen(us->unusual_dev->vendorName));
		memcpy(data+16, us->unusual_dev->productName, 
			strlen(us->unusual_dev->productName) > 16 ? 16 :
			strlen(us->unusual_dev->productName));
		data[32] = 0x30 + ((bcdDevice>>12) & 0x0F);
		data[33] = 0x30 + ((bcdDevice>>8) & 0x0F);
		data[34] = 0x30 + ((bcdDevice>>4) & 0x0F);
		data[35] = 0x30 + ((bcdDevice) & 0x0F);
	}

	usb_stor_set_xfer_buf(data, data_len, us->srb);
}

static int usb_stor_control_thread(void * __us)
{
	struct us_data *us = (struct us_data *)__us;
	struct Scsi_Host *host = us_to_host(us);

	for(;;) {
		US_DEBUGP("*** thread sleeping.\n");
		if(down_interruptible(&us->sema))
			break;
			
		US_DEBUGP("*** thread awakened.\n");

		/* lock the device pointers */
		mutex_lock(&(us->dev_mutex));

		/* if the device has disconnected, we are free to exit */
		if (test_bit(US_FLIDX_DISCONNECTING, &us->flags)) {
			US_DEBUGP("-- exiting\n");
			mutex_unlock(&us->dev_mutex);
			break;
		}

		/* lock access to the state */
		scsi_lock(host);

		/* has the command timed out *already* ? */
		if (test_bit(US_FLIDX_TIMED_OUT, &us->flags)) {
			us->srb->result = DID_ABORT << 16;
			goto SkipForAbort;
		}

		scsi_unlock(host);

		/* reject the command if the direction indicator 
		 * is UNKNOWN
		 */
		if (us->srb->sc_data_direction == DMA_BIDIRECTIONAL) {
			US_DEBUGP("UNKNOWN data direction\n");
			us->srb->result = DID_ERROR << 16;
		}

		/* reject if target != 0 or if LUN is higher than
		 * the maximum known LUN
		 */
		else if (us->srb->device->id && 
				!(us->flags & US_FL_SCM_MULT_TARG)) {
			US_DEBUGP("Bad target number (%d:%d)\n",
				  us->srb->device->id, us->srb->device->lun);
			us->srb->result = DID_BAD_TARGET << 16;
		}

		else if (us->srb->device->lun > us->max_lun) {
			US_DEBUGP("Bad LUN (%d:%d)\n",
				  us->srb->device->id, us->srb->device->lun);
			us->srb->result = DID_BAD_TARGET << 16;
		}

		/* Handle those devices which need us to fake 
		 * their inquiry data */
		else if ((us->srb->cmnd[0] == INQUIRY) &&
			    (us->flags & US_FL_FIX_INQUIRY)) {
			unsigned char data_ptr[36] = {
			    0x00, 0x80, 0x02, 0x02,
			    0x1F, 0x00, 0x00, 0x00};

			US_DEBUGP("Faking INQUIRY command\n");
			fill_inquiry_response(us, data_ptr, 36);
			us->srb->result = SAM_STAT_GOOD;
		}

		/* we've got a command, let's do it! */
		else {
			US_DEBUG(usb_stor_show_command(us->srb));
			us->proto_handler(us->srb, us);
		}

		/* lock access to the state */
		scsi_lock(host);

		/* did the command already complete because of a disconnect? */
		if (!us->srb)
			;		/* nothing to do */

		/* indicate that the command is done */
		else if (us->srb->result != DID_ABORT << 16) {
			US_DEBUGP("scsi cmd done, result=0x%x\n", 
				   us->srb->result);
			us->srb->scsi_done(us->srb);
		} else {
SkipForAbort:
			US_DEBUGP("scsi command aborted\n");
		}

		/* If an abort request was received we need to signal that
		 * the abort has finished.  The proper test for this is
		 * the TIMED_OUT flag, not srb->result == DID_ABORT, because
		 * the timeout might have occurred after the command had
		 * already completed with a different result code. */
		if (test_bit(US_FLIDX_TIMED_OUT, &us->flags)) {
			complete(&(us->notify));

			/* Allow USB transfers to resume */
			clear_bit(US_FLIDX_ABORTING, &us->flags);
			clear_bit(US_FLIDX_TIMED_OUT, &us->flags);
		}

		/* finished working on this command */
		us->srb = NULL;
		scsi_unlock(host);

		/* unlock the device pointers */
		mutex_unlock(&us->dev_mutex);
	} /* for (;;) */

	/* Wait until we are told to stop */
	for (;;) {
		set_current_state(TASK_INTERRUPTIBLE);
		if (kthread_should_stop())
			break;
		schedule();
	}
	__set_current_state(TASK_RUNNING);
	return 0;
}	

/***********************************************************************
 * Device probing and disconnecting
 ***********************************************************************/

/* Associate our private data with the USB device */
static int associate_dev(struct us_data *us, struct usb_interface *intf)
{
	US_DEBUGP("-- %s\n", __FUNCTION__);

	/* Fill in the device-related fields */
	us->pusb_dev = interface_to_usbdev(intf);
	us->pusb_intf = intf;
	us->ifnum = intf->cur_altsetting->desc.bInterfaceNumber;
	US_DEBUGP("Vendor: 0x%04x, Product: 0x%04x, Revision: 0x%04x\n",
			le16_to_cpu(us->pusb_dev->descriptor.idVendor),
			le16_to_cpu(us->pusb_dev->descriptor.idProduct),
			le16_to_cpu(us->pusb_dev->descriptor.bcdDevice));
	US_DEBUGP("Interface Subclass: 0x%02x, Protocol: 0x%02x\n",
			intf->cur_altsetting->desc.bInterfaceSubClass,
			intf->cur_altsetting->desc.bInterfaceProtocol);

	/* Store our private data in the interface */
	usb_set_intfdata(intf, us);

	/* Allocate the device-related DMA-mapped buffers */
	us->cr = usb_buffer_alloc(us->pusb_dev, sizeof(*us->cr),
			GFP_KERNEL, &us->cr_dma);
	if (!us->cr) {
		US_DEBUGP("usb_ctrlrequest allocation failed\n");
		return -ENOMEM;
	}

	us->iobuf = usb_buffer_alloc(us->pusb_dev, US_IOBUF_SIZE,
			GFP_KERNEL, &us->iobuf_dma);
	if (!us->iobuf) {
		US_DEBUGP("I/O buffer allocation failed\n");
		return -ENOMEM;
	}

	us->sensebuf = kmalloc(US_SENSE_SIZE, GFP_KERNEL);
	if (!us->sensebuf) {
		US_DEBUGP("Sense buffer allocation failed\n");
		return -ENOMEM;
	}
	return 0;
}

/* Find an unusual_dev descriptor (always succeeds in the current code) */
static struct us_unusual_dev *find_unusual(const struct usb_device_id *id)
{
	const int id_index = id - storage_usb_ids;
	return &us_unusual_dev_list[id_index];
}

/* Get the unusual_devs entries and the string descriptors */
static int get_device_info(struct us_data *us, const struct usb_device_id *id)
{
	struct usb_device *dev = us->pusb_dev;
	struct usb_interface_descriptor *idesc =
		&us->pusb_intf->cur_altsetting->desc;
	struct us_unusual_dev *unusual_dev = find_unusual(id);

	/* Store the entries */
	us->unusual_dev = unusual_dev;
	us->subclass = (unusual_dev->useProtocol == US_SC_DEVICE) ?
			idesc->bInterfaceSubClass :
			unusual_dev->useProtocol;
	us->protocol = (unusual_dev->useTransport == US_PR_DEVICE) ?
			idesc->bInterfaceProtocol :
			unusual_dev->useTransport;
	us->flags = USB_US_ORIG_FLAGS(id->driver_info);

	if (us->flags & US_FL_IGNORE_DEVICE) {
		printk(KERN_INFO USB_STORAGE "device ignored\n");
		return -ENODEV;
	}

	/*
	 * This flag is only needed when we're in high-speed, so let's
	 * disable it if we're in full-speed
	 */
	if (dev->speed != USB_SPEED_HIGH)
		us->flags &= ~US_FL_GO_SLOW;

	/* Log a message if a non-generic unusual_dev entry contains an
	 * unnecessary subclass or protocol override.  This may stimulate
	 * reports from users that will help us remove unneeded entries
	 * from the unusual_devs.h table.
	 */
	if (id->idVendor || id->idProduct) {
		static const char *msgs[3] = {
			"an unneeded SubClass entry",
			"an unneeded Protocol entry",
			"unneeded SubClass and Protocol entries"};
		struct usb_device_descriptor *ddesc = &dev->descriptor;
		int msg = -1;

		if (unusual_dev->useProtocol != US_SC_DEVICE &&
			us->subclass == idesc->bInterfaceSubClass)
			msg += 1;
		if (unusual_dev->useTransport != US_PR_DEVICE &&
			us->protocol == idesc->bInterfaceProtocol)
			msg += 2;
		if (msg >= 0 && !(us->flags & US_FL_NEED_OVERRIDE))
			printk(KERN_NOTICE USB_STORAGE "This device "
				"(%04x,%04x,%04x S %02x P %02x)"
				" has %s in unusual_devs.h (kernel"
				" %s)\n"
				"   Please send a copy of this message to "
				"<linux-usb-devel@lists.sourceforge.net>\n",
				le16_to_cpu(ddesc->idVendor),
				le16_to_cpu(ddesc->idProduct),
				le16_to_cpu(ddesc->bcdDevice),
				idesc->bInterfaceSubClass,
				idesc->bInterfaceProtocol,
				msgs[msg],
				utsname()->release);
	}

	return 0;
}

/* Get the transport settings */
static int get_transport(struct us_data *us)
{
	switch (us->protocol) {
	case US_PR_CB:
		us->transport_name = "Control/Bulk";
		us->transport = usb_stor_CB_transport;
		us->transport_reset = usb_stor_CB_reset;
		us->max_lun = 7;
		break;

	case US_PR_CBI:
		us->transport_name = "Control/Bulk/Interrupt";
		us->transport = usb_stor_CBI_transport;
		us->transport_reset = usb_stor_CB_reset;
		us->max_lun = 7;
		break;

	case US_PR_BULK:
		us->transport_name = "Bulk";
		us->transport = usb_stor_Bulk_transport;
		us->transport_reset = usb_stor_Bulk_reset;
		break;

#ifdef CONFIG_USB_STORAGE_USBAT
	case US_PR_USBAT:
		us->transport_name = "Shuttle USBAT";
		us->transport = usbat_transport;
		us->transport_reset = usb_stor_CB_reset;
		us->max_lun = 1;
		break;
#endif

#ifdef CONFIG_USB_STORAGE_SDDR09
	case US_PR_EUSB_SDDR09:
		us->transport_name = "EUSB/SDDR09";
		us->transport = sddr09_transport;
		us->transport_reset = usb_stor_CB_reset;
		us->max_lun = 0;
		break;
#endif

#ifdef CONFIG_USB_STORAGE_SDDR55
	case US_PR_SDDR55:
		us->transport_name = "SDDR55";
		us->transport = sddr55_transport;
		us->transport_reset = sddr55_reset;
		us->max_lun = 0;
		break;
#endif

#ifdef CONFIG_USB_STORAGE_DPCM
	case US_PR_DPCM_USB:
		us->transport_name = "Control/Bulk-EUSB/SDDR09";
		us->transport = dpcm_transport;
		us->transport_reset = usb_stor_CB_reset;
		us->max_lun = 1;
		break;
#endif

#ifdef CONFIG_USB_STORAGE_FREECOM
	case US_PR_FREECOM:
		us->transport_name = "Freecom";
		us->transport = freecom_transport;
		us->transport_reset = usb_stor_freecom_reset;
		us->max_lun = 0;
		break;
#endif

#ifdef CONFIG_USB_STORAGE_DATAFAB
	case US_PR_DATAFAB:
		us->transport_name  = "Datafab Bulk-Only";
		us->transport = datafab_transport;
		us->transport_reset = usb_stor_Bulk_reset;
		us->max_lun = 1;
		break;
#endif

#ifdef CONFIG_USB_STORAGE_JUMPSHOT
	case US_PR_JUMPSHOT:
		us->transport_name  = "Lexar Jumpshot Control/Bulk";
		us->transport = jumpshot_transport;
		us->transport_reset = usb_stor_Bulk_reset;
		us->max_lun = 1;
		break;
#endif

#ifdef CONFIG_USB_STORAGE_ALAUDA
	case US_PR_ALAUDA:
		us->transport_name  = "Alauda Control/Bulk";
		us->transport = alauda_transport;
		us->transport_reset = usb_stor_Bulk_reset;
		us->max_lun = 1;
		break;
#endif

#ifdef CONFIG_USB_STORAGE_KARMA
	case US_PR_KARMA:
		us->transport_name = "Rio Karma/Bulk";
		us->transport = rio_karma_transport;
		us->transport_reset = usb_stor_Bulk_reset;
		break;
#endif

	default:
		return -EIO;
	}
	US_DEBUGP("Transport: %s\n", us->transport_name);

	/* fix for single-lun devices */
	if (us->flags & US_FL_SINGLE_LUN)
		us->max_lun = 0;
	return 0;
}

/* Get the protocol settings */
static int get_protocol(struct us_data *us)
{
	switch (us->subclass) {
	case US_SC_RBC:
		us->protocol_name = "Reduced Block Commands (RBC)";
		us->proto_handler = usb_stor_transparent_scsi_command;
		break;

	case US_SC_8020:
		us->protocol_name = "8020i";
		us->proto_handler = usb_stor_ATAPI_command;
		us->max_lun = 0;
		break;

	case US_SC_QIC:
		us->protocol_name = "QIC-157";
		us->proto_handler = usb_stor_qic157_command;
		us->max_lun = 0;
		break;

	case US_SC_8070:
		us->protocol_name = "8070i";
		us->proto_handler = usb_stor_ATAPI_command;
		us->max_lun = 0;
		break;

	case US_SC_SCSI:
		us->protocol_name = "Transparent SCSI";
		us->proto_handler = usb_stor_transparent_scsi_command;
		break;

	case US_SC_UFI:
		us->protocol_name = "Uniform Floppy Interface (UFI)";
		us->proto_handler = usb_stor_ufi_command;
		break;

#ifdef CONFIG_USB_STORAGE_ISD200
	case US_SC_ISD200:
		us->protocol_name = "ISD200 ATA/ATAPI";
		us->proto_handler = isd200_ata_command;
		break;
#endif

#ifdef CONFIG_USB_STORAGE_CYPRESS_ATACB
	case US_SC_CYP_ATACB:
		us->protocol_name = "Transparent SCSI with Cypress ATACB";
		us->proto_handler = cypress_atacb_passthrough;
		break;
#endif

	default:
		return -EIO;
	}
	US_DEBUGP("Protocol: %s\n", us->protocol_name);
	return 0;
}

/* Get the pipe settings */
static int get_pipes(struct us_data *us)
{
	struct usb_host_interface *altsetting =
		us->pusb_intf->cur_altsetting;
	int i;
	struct usb_endpoint_descriptor *ep;
	struct usb_endpoint_descriptor *ep_in = NULL;
	struct usb_endpoint_descriptor *ep_out = NULL;
	struct usb_endpoint_descriptor *ep_int = NULL;

	/*
	 * Find the first endpoint of each type we need.
	 * We are expecting a minimum of 2 endpoints - in and out (bulk).
	 * An optional interrupt-in is OK (necessary for CBI protocol).
	 * We will ignore any others.
	 */
	for (i = 0; i < altsetting->desc.bNumEndpoints; i++) {
		ep = &altsetting->endpoint[i].desc;

		if (usb_endpoint_xfer_bulk(ep)) {
			if (usb_endpoint_dir_in(ep)) {
				if (!ep_in)
					ep_in = ep;
			} else {
				if (!ep_out)
					ep_out = ep;
			}
		}

		else if (usb_endpoint_is_int_in(ep)) {
			if (!ep_int)
				ep_int = ep;
		}
	}

	if (!ep_in || !ep_out || (us->protocol == US_PR_CBI && !ep_int)) {
		US_DEBUGP("Endpoint sanity check failed! Rejecting dev.\n");
		return -EIO;
	}

	/* Calculate and store the pipe values */
	us->send_ctrl_pipe = usb_sndctrlpipe(us->pusb_dev, 0);
	us->recv_ctrl_pipe = usb_rcvctrlpipe(us->pusb_dev, 0);
	us->send_bulk_pipe = usb_sndbulkpipe(us->pusb_dev,
		ep_out->bEndpointAddress & USB_ENDPOINT_NUMBER_MASK);
	us->recv_bulk_pipe = usb_rcvbulkpipe(us->pusb_dev, 
		ep_in->bEndpointAddress & USB_ENDPOINT_NUMBER_MASK);
	if (ep_int) {
		us->recv_intr_pipe = usb_rcvintpipe(us->pusb_dev,
			ep_int->bEndpointAddress & USB_ENDPOINT_NUMBER_MASK);
		us->ep_bInterval = ep_int->bInterval;
	}
	return 0;
}

/* Initialize all the dynamic resources we need */
static int usb_stor_acquire_resources(struct us_data *us)
{
	int p;
	struct task_struct *th;

	us->current_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!us->current_urb) {
		US_DEBUGP("URB allocation failed\n");
		return -ENOMEM;
	}

	/* Just before we start our control thread, initialize
	 * the device if it needs initialization */
	if (us->unusual_dev->initFunction) {
		p = us->unusual_dev->initFunction(us);
		if (p)
			return p;
	}

	/* Start up our control thread */
	th = kthread_run(usb_stor_control_thread, us, "usb-storage");
	if (IS_ERR(th)) {
		printk(KERN_WARNING USB_STORAGE 
		       "Unable to start control thread\n");
		return PTR_ERR(th);
	}
	us->ctl_thread = th;

	return 0;
}

/* Release all our dynamic resources */
static void usb_stor_release_resources(struct us_data *us)
{
	US_DEBUGP("-- %s\n", __FUNCTION__);

	/* Tell the control thread to exit.  The SCSI host must
	 * already have been removed so it won't try to queue
	 * any more commands.
	 */
	US_DEBUGP("-- sending exit command to thread\n");
	set_bit(US_FLIDX_DISCONNECTING, &us->flags);
	up(&us->sema);
	if (us->ctl_thread)
		kthread_stop(us->ctl_thread);

	/* Call the destructor routine, if it exists */
	if (us->extra_destructor) {
		US_DEBUGP("-- calling extra_destructor()\n");
		us->extra_destructor(us->extra);
	}

	/* Free the extra data and the URB */
	kfree(us->extra);
	usb_free_urb(us->current_urb);
}

/* Dissociate from the USB device */
static void dissociate_dev(struct us_data *us)
{
	US_DEBUGP("-- %s\n", __FUNCTION__);

	kfree(us->sensebuf);

	/* Free the device-related DMA-mapped buffers */
	if (us->cr)
		usb_buffer_free(us->pusb_dev, sizeof(*us->cr), us->cr,
				us->cr_dma);
	if (us->iobuf)
		usb_buffer_free(us->pusb_dev, US_IOBUF_SIZE, us->iobuf,
				us->iobuf_dma);

	/* Remove our private data from the interface */
	usb_set_intfdata(us->pusb_intf, NULL);
}

/* First stage of disconnect processing: stop all commands and remove
 * the host */
static void quiesce_and_remove_host(struct us_data *us)
{
	struct Scsi_Host *host = us_to_host(us);

	/* Prevent new USB transfers, stop the current command, and
	 * interrupt a SCSI-scan or device-reset delay */
	scsi_lock(host);
	set_bit(US_FLIDX_DISCONNECTING, &us->flags);
	scsi_unlock(host);
	usb_stor_stop_transport(us);
	wake_up(&us->delay_wait);

	/* queuecommand won't accept any new commands and the control
	 * thread won't execute a previously-queued command.  If there
	 * is such a command pending, complete it with an error. */
	mutex_lock(&us->dev_mutex);
	if (us->srb) {
		us->srb->result = DID_NO_CONNECT << 16;
		scsi_lock(host);
		us->srb->scsi_done(us->srb);
		us->srb = NULL;
		complete(&us->notify);		/* in case of an abort */
		scsi_unlock(host);
	}
	mutex_unlock(&us->dev_mutex);

	/* Now we own no commands so it's safe to remove the SCSI host */
	scsi_remove_host(host);

	/* Wait for the SCSI-scanning thread to stop */
	wait_for_completion(&us->scanning_done);
}

/* Second stage of disconnect processing: deallocate all resources */
static void release_everything(struct us_data *us)
{
	usb_stor_release_resources(us);
	dissociate_dev(us);

	/* Drop our reference to the host; the SCSI core will free it
	 * (and "us" along with it) when the refcount becomes 0. */
	scsi_host_put(us_to_host(us));
}

/* Thread to carry out delayed SCSI-device scanning */
static int usb_stor_scan_thread(void * __us)
{
	struct us_data *us = (struct us_data *)__us;

	printk(KERN_DEBUG
		"usb-storage: device found at %d\n", us->pusb_dev->devnum);

	set_freezable();
	/* Wait for the timeout to expire or for a disconnect */
	if (delay_use > 0) {
		printk(KERN_DEBUG "usb-storage: waiting for device "
				"to settle before scanning\n");
		wait_event_freezable_timeout(us->delay_wait,
				test_bit(US_FLIDX_DISCONNECTING, &us->flags),
				delay_use * HZ);
	}

	/* If the device is still connected, perform the scanning */
	if (!test_bit(US_FLIDX_DISCONNECTING, &us->flags)) {

		/* For bulk-only devices, determine the max LUN value */
		if (us->protocol == US_PR_BULK &&
				!(us->flags & US_FL_SINGLE_LUN)) {
			mutex_lock(&us->dev_mutex);
			us->max_lun = usb_stor_Bulk_max_lun(us);
			mutex_unlock(&us->dev_mutex);
		}
		scsi_scan_host(us_to_host(us));
		printk(KERN_DEBUG "usb-storage: device scan complete\n");

		/* Should we unbind if no devices were detected? */
	}

	complete_and_exit(&us->scanning_done, 0);
}


/* Probe to see if we can drive a newly-connected USB device */
static int storage_probe(struct usb_interface *intf,
			 const struct usb_device_id *id)
{
	struct Scsi_Host *host;
	struct us_data *us;
	int result;
	struct task_struct *th;

	if (usb_usual_check_type(id, USB_US_TYPE_STOR))
		return -ENXIO;

	US_DEBUGP("USB Mass Storage device detected\n");

	/*
	 * Ask the SCSI layer to allocate a host structure, with extra
	 * space at the end for our private us_data structure.
	 */
	host = scsi_host_alloc(&usb_stor_host_template, sizeof(*us));
	if (!host) {
		printk(KERN_WARNING USB_STORAGE
			"Unable to allocate the scsi host\n");
		return -ENOMEM;
	}

	/*
	 * Allow 16-byte CDBs and thus > 2TB
	 */
	host->max_cmd_len = 16;
	us = host_to_us(host);
	memset(us, 0, sizeof(struct us_data));
	mutex_init(&(us->dev_mutex));
	init_MUTEX_LOCKED(&(us->sema));
	init_completion(&(us->notify));
	init_waitqueue_head(&us->delay_wait);
	init_completion(&us->scanning_done);

	/* Associate the us_data structure with the USB device */
	result = associate_dev(us, intf);
	if (result)
		goto BadDevice;

	/*
	 * Get the unusual_devs entries and the descriptors
	 *
	 * id_index is calculated in the declaration to be the index number
	 * of the match from the usb_device_id table, so we can find the
	 * corresponding entry in the private table.
	 */
	result = get_device_info(us, id);
	if (result)
		goto BadDevice;

	/* Get the transport, protocol, and pipe settings */
	result = get_transport(us);
	if (result)
		goto BadDevice;
	result = get_protocol(us);
	if (result)
		goto BadDevice;
	result = get_pipes(us);
	if (result)
		goto BadDevice;

	/* Acquire all the other resources and add the host */
	result = usb_stor_acquire_resources(us);
	if (result)
		goto BadDevice;
	result = scsi_add_host(host, &intf->dev);
	if (result) {
		printk(KERN_WARNING USB_STORAGE
			"Unable to add the scsi host\n");
		goto BadDevice;
	}

	/* Start up the thread for delayed SCSI-device scanning */
	th = kthread_create(usb_stor_scan_thread, us, "usb-stor-scan");
	if (IS_ERR(th)) {
		printk(KERN_WARNING USB_STORAGE 
		       "Unable to start the device-scanning thread\n");
		quiesce_and_remove_host(us);
		result = PTR_ERR(th);
		goto BadDevice;
	}

	wake_up_process(th);

	return 0;

	/* We come here if there are any problems */
BadDevice:
	US_DEBUGP("storage_probe() failed\n");
	release_everything(us);
	return result;
}

/* Handle a disconnect event from the USB core */
static void storage_disconnect(struct usb_interface *intf)
{
	struct us_data *us = usb_get_intfdata(intf);

	US_DEBUGP("storage_disconnect() called\n");
	quiesce_and_remove_host(us);
	release_everything(us);
}

/***********************************************************************
 * Initialization and registration
 ***********************************************************************/

static struct usb_driver usb_storage_driver = {
	.name =		"usb-storage",
	.probe =	storage_probe,
	.disconnect =	storage_disconnect,
#ifdef CONFIG_PM
	.suspend =	storage_suspend,
	.resume =	storage_resume,
	.reset_resume =	storage_reset_resume,
#endif
	.pre_reset =	storage_pre_reset,
	.post_reset =	storage_post_reset,
	.id_table =	storage_usb_ids,
};

static int __init usb_stor_init(void)
{
	int retval;
	printk(KERN_INFO "Initializing USB Mass Storage driver...\n");

	/* register the driver, return usb_register return code if error */
	retval = usb_register(&usb_storage_driver);
	if (retval == 0) {
		printk(KERN_INFO "USB Mass Storage support registered.\n");
		usb_usual_set_present(USB_US_TYPE_STOR);
	}
	return retval;
}

static void __exit usb_stor_exit(void)
{
	US_DEBUGP("usb_stor_exit() called\n");

	/* Deregister the driver
	 * This will cause disconnect() to be called for each
	 * attached unit
	 */
	US_DEBUGP("-- calling usb_deregister()\n");
	usb_deregister(&usb_storage_driver) ;

	usb_usual_clear_present(USB_US_TYPE_STOR);
}

module_init(usb_stor_init);
module_exit(usb_stor_exit);
