/*
 * dell_rbu.c
 * Bios Update driver for Dell systems
 * Author: Dell Inc
 *         Abhay Salunke <abhay_salunke@dell.com>
 *
 * Copyright (C) 2005 Dell Inc.
 *
 * Remote BIOS Update (rbu) driver is used for updating DELL BIOS by
 * creating entries in the /sys file systems on Linux 2.6 and higher
 * kernels. The driver supports two mechanism to update the BIOS namely
 * contiguous and packetized. Both these methods still require having some
 * application to set the CMOS bit indicating the BIOS to update itself
 * after a reboot.
 *
 * Contiguous method:
 * This driver writes the incoming data in a monolithic image by allocating
 * contiguous physical pages large enough to accommodate the incoming BIOS
 * image size.
 *
 * Packetized method:
 * The driver writes the incoming packet image by allocating a new packet
 * on every time the packet data is written. This driver requires an
 * application to break the BIOS image in to fixed sized packet chunks.
 *
 * See Documentation/dell_rbu.txt for more info.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License v2.0 as published by
 * the Free Software Foundation
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <linux/version.h>
#include <linux/config.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/blkdev.h>
#include <linux/device.h>
#include <linux/spinlock.h>
#include <linux/moduleparam.h>
#include <linux/firmware.h>
#include <linux/dma-mapping.h>

MODULE_AUTHOR("Abhay Salunke <abhay_salunke@dell.com>");
MODULE_DESCRIPTION("Driver for updating BIOS image on DELL systems");
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");

#define BIOS_SCAN_LIMIT 0xffffffff
#define MAX_IMAGE_LENGTH 16
static struct _rbu_data {
	void *image_update_buffer;
	unsigned long image_update_buffer_size;
	unsigned long bios_image_size;
	int image_update_ordernum;
	int dma_alloc;
	spinlock_t lock;
	unsigned long packet_read_count;
	unsigned long packet_write_count;
	unsigned long num_packets;
	unsigned long packetsize;
} rbu_data;

static char image_type[MAX_IMAGE_LENGTH] = "mono";
module_param_string(image_type, image_type, sizeof(image_type), 0);
MODULE_PARM_DESC(image_type, "BIOS image type. choose- mono or packet");

struct packet_data {
	struct list_head list;
	size_t length;
	void *data;
	int ordernum;
};

static struct packet_data packet_data_head;

static struct platform_device *rbu_device;
static int context;
static dma_addr_t dell_rbu_dmaaddr;

static void init_packet_head(void)
{
	INIT_LIST_HEAD(&packet_data_head.list);
	rbu_data.packet_write_count = 0;
	rbu_data.packet_read_count = 0;
	rbu_data.num_packets = 0;
	rbu_data.packetsize = 0;
}

static int fill_last_packet(void *data, size_t length)
{
	struct list_head *ptemp_list;
	struct packet_data *packet = NULL;
	int packet_count = 0;

	pr_debug("fill_last_packet: entry \n");

	if (!rbu_data.num_packets) {
		pr_debug("fill_last_packet: num_packets=0\n");
		return -ENOMEM;
	}

	packet_count = rbu_data.num_packets;

	ptemp_list = (&packet_data_head.list)->prev;

	packet = list_entry(ptemp_list, struct packet_data, list);

	if ((rbu_data.packet_write_count + length) > rbu_data.packetsize) {
		pr_debug("dell_rbu:%s: packet size data "
			 "overrun\n", __FUNCTION__);
		return -EINVAL;
	}

	pr_debug("fill_last_packet : buffer = %p\n", packet->data);

	memcpy((packet->data + rbu_data.packet_write_count), data, length);

	if ((rbu_data.packet_write_count + length) == rbu_data.packetsize) {
		/*
		 * this was the last data chunk in the packet
		 * so reinitialize the packet data counter to zero
		 */
		rbu_data.packet_write_count = 0;
	} else
		rbu_data.packet_write_count += length;

	pr_debug("fill_last_packet: exit \n");
	return 0;
}

static int create_packet(size_t length)
{
	struct packet_data *newpacket;
	int ordernum = 0;

	pr_debug("create_packet: entry \n");

	if (!rbu_data.packetsize) {
		pr_debug("create_packet: packetsize not specified\n");
		return -EINVAL;
	}

	newpacket = kmalloc(sizeof(struct packet_data), GFP_KERNEL);
	if (!newpacket) {
		printk(KERN_WARNING
		       "dell_rbu:%s: failed to allocate new "
		       "packet\n", __FUNCTION__);
		return -ENOMEM;
	}

	ordernum = get_order(length);
	/*
	 * there is no upper limit on memory
	 * address for packetized mechanism
	 */
	newpacket->data = (unsigned char *)__get_free_pages(GFP_KERNEL,
							    ordernum);

	pr_debug("create_packet: newpacket %p\n", newpacket->data);

	if (!newpacket->data) {
		printk(KERN_WARNING
		       "dell_rbu:%s: failed to allocate new "
		       "packet\n", __FUNCTION__);
		kfree(newpacket);
		return -ENOMEM;
	}

	newpacket->ordernum = ordernum;
	++rbu_data.num_packets;
	/*
	 * initialize the newly created packet headers
	 */
	INIT_LIST_HEAD(&newpacket->list);
	list_add_tail(&newpacket->list, &packet_data_head.list);
	/*
	 * packets have fixed size
	 */
	newpacket->length = rbu_data.packetsize;

	pr_debug("create_packet: exit \n");

	return 0;
}

static int packetize_data(void *data, size_t length)
{
	int rc = 0;

	if (!rbu_data.packet_write_count) {
		if ((rc = create_packet(length)))
			return rc;
	}
	if ((rc = fill_last_packet(data, length)))
		return rc;

	return rc;
}

static int
do_packet_read(char *data, struct list_head *ptemp_list,
	       int length, int bytes_read, int *list_read_count)
{
	void *ptemp_buf;
	struct packet_data *newpacket = NULL;
	int bytes_copied = 0;
	int j = 0;

	newpacket = list_entry(ptemp_list, struct packet_data, list);
	*list_read_count += newpacket->length;

	if (*list_read_count > bytes_read) {
		/* point to the start of unread data */
		j = newpacket->length - (*list_read_count - bytes_read);
		/* point to the offset in the packet buffer */
		ptemp_buf = (u8 *) newpacket->data + j;
		/*
		 * check if there is enough room in
		 * * the incoming buffer
		 */
		if (length > (*list_read_count - bytes_read))
			/*
			 * copy what ever is there in this
			 * packet and move on
			 */
			bytes_copied = (*list_read_count - bytes_read);
		else
			/* copy the remaining */
			bytes_copied = length;
		memcpy(data, ptemp_buf, bytes_copied);
	}
	return bytes_copied;
}

static int packet_read_list(char *data, size_t * pread_length)
{
	struct list_head *ptemp_list;
	int temp_count = 0;
	int bytes_copied = 0;
	int bytes_read = 0;
	int remaining_bytes = 0;
	char *pdest = data;

	/* check if we have any packets */
	if (0 == rbu_data.num_packets)
		return -ENOMEM;

	remaining_bytes = *pread_length;
	bytes_read = rbu_data.packet_read_count;

	ptemp_list = (&packet_data_head.list)->next;
	while (!list_empty(ptemp_list)) {
		bytes_copied = do_packet_read(pdest, ptemp_list,
					      remaining_bytes, bytes_read,
					      &temp_count);
		remaining_bytes -= bytes_copied;
		bytes_read += bytes_copied;
		pdest += bytes_copied;
		/*
		 * check if we reached end of buffer before reaching the
		 * last packet
		 */
		if (remaining_bytes == 0)
			break;

		ptemp_list = ptemp_list->next;
	}
	/*finally set the bytes read */
	*pread_length = bytes_read - rbu_data.packet_read_count;
	rbu_data.packet_read_count = bytes_read;
	return 0;
}

static void packet_empty_list(void)
{
	struct list_head *ptemp_list;
	struct list_head *pnext_list;
	struct packet_data *newpacket;

	ptemp_list = (&packet_data_head.list)->next;
	while (!list_empty(ptemp_list)) {
		newpacket =
		    list_entry(ptemp_list, struct packet_data, list);
		pnext_list = ptemp_list->next;
		list_del(ptemp_list);
		ptemp_list = pnext_list;
		/*
		 * zero out the RBU packet memory before freeing
		 * to make sure there are no stale RBU packets left in memory
		 */
		memset(newpacket->data, 0, rbu_data.packetsize);
		free_pages((unsigned long)newpacket->data,
			   newpacket->ordernum);
		kfree(newpacket);
	}
	rbu_data.packet_write_count = 0;
	rbu_data.packet_read_count = 0;
	rbu_data.num_packets = 0;
	rbu_data.packetsize = 0;
}

/*
 * img_update_free: Frees the buffer allocated for storing BIOS image
 * Always called with lock held and returned with lock held
 */
static void img_update_free(void)
{
	if (!rbu_data.image_update_buffer)
		return;
	/*
	 * zero out this buffer before freeing it to get rid of any stale
	 * BIOS image copied in memory.
	 */
	memset(rbu_data.image_update_buffer, 0,
	       rbu_data.image_update_buffer_size);
	if (rbu_data.dma_alloc == 1)
		dma_free_coherent(NULL, rbu_data.bios_image_size,
				  rbu_data.image_update_buffer,
				  dell_rbu_dmaaddr);
	else
		free_pages((unsigned long)rbu_data.image_update_buffer,
			   rbu_data.image_update_ordernum);

	/*
	 * Re-initialize the rbu_data variables after a free
	 */
	rbu_data.image_update_ordernum = -1;
	rbu_data.image_update_buffer = NULL;
	rbu_data.image_update_buffer_size = 0;
	rbu_data.bios_image_size = 0;
	rbu_data.dma_alloc = 0;
}

/*
 * img_update_realloc: This function allocates the contiguous pages to
 * accommodate the requested size of data. The memory address and size
 * values are stored globally and on every call to this function the new
 * size is checked to see if more data is required than the existing size.
 * If true the previous memory is freed and new allocation is done to
 * accommodate the new size. If the incoming size is less then than the
 * already allocated size, then that memory is reused. This function is
 * called with lock held and returns with lock held.
 */
static int img_update_realloc(unsigned long size)
{
	unsigned char *image_update_buffer = NULL;
	unsigned long rc;
	unsigned long img_buf_phys_addr;
	int ordernum;
	int dma_alloc = 0;

	/*
	 * check if the buffer of sufficient size has been
	 * already allocated
	 */
	if (rbu_data.image_update_buffer_size >= size) {
		/*
		 * check for corruption
		 */
		if ((size != 0) && (rbu_data.image_update_buffer == NULL)) {
			printk(KERN_ERR "dell_rbu:%s: corruption "
			       "check failed\n", __FUNCTION__);
			return -EINVAL;
		}
		/*
		 * we have a valid pre-allocated buffer with
		 * sufficient size
		 */
		return 0;
	}

	/*
	 * free any previously allocated buffer
	 */
	img_update_free();

	spin_unlock(&rbu_data.lock);

	ordernum = get_order(size);
	image_update_buffer =
	    (unsigned char *)__get_free_pages(GFP_KERNEL, ordernum);

	img_buf_phys_addr =
	    (unsigned long)virt_to_phys(image_update_buffer);

	if (img_buf_phys_addr > BIOS_SCAN_LIMIT) {
		free_pages((unsigned long)image_update_buffer, ordernum);
		ordernum = -1;
		image_update_buffer = dma_alloc_coherent(NULL, size,
							 &dell_rbu_dmaaddr,
							 GFP_KERNEL);
		dma_alloc = 1;
	}

	spin_lock(&rbu_data.lock);

	if (image_update_buffer != NULL) {
		rbu_data.image_update_buffer = image_update_buffer;
		rbu_data.image_update_buffer_size = size;
		rbu_data.bios_image_size =
		    rbu_data.image_update_buffer_size;
		rbu_data.image_update_ordernum = ordernum;
		rbu_data.dma_alloc = dma_alloc;
		rc = 0;
	} else {
		pr_debug("Not enough memory for image update:"
			 "size = %ld\n", size);
		rc = -ENOMEM;
	}

	return rc;
}

static ssize_t read_packet_data(char *buffer, loff_t pos, size_t count)
{
	int retval;
	size_t bytes_left;
	size_t data_length;
	char *ptempBuf = buffer;
	unsigned long imagesize;

	/* check to see if we have something to return */
	if (rbu_data.num_packets == 0) {
		pr_debug("read_packet_data: no packets written\n");
		retval = -ENOMEM;
		goto read_rbu_data_exit;
	}

	imagesize = rbu_data.num_packets * rbu_data.packetsize;

	if (pos > imagesize) {
		retval = 0;
		printk(KERN_WARNING "dell_rbu:read_packet_data: "
		       "data underrun\n");
		goto read_rbu_data_exit;
	}

	bytes_left = imagesize - pos;
	data_length = min(bytes_left, count);

	if ((retval = packet_read_list(ptempBuf, &data_length)) < 0)
		goto read_rbu_data_exit;

	if ((pos + count) > imagesize) {
		rbu_data.packet_read_count = 0;
		/* this was the last copy */
		retval = bytes_left;
	} else
		retval = count;

      read_rbu_data_exit:
	return retval;
}

static ssize_t read_rbu_mono_data(char *buffer, loff_t pos, size_t count)
{
	unsigned char *ptemp = NULL;
	size_t bytes_left = 0;
	size_t data_length = 0;
	ssize_t ret_count = 0;

	/* check to see if we have something to return */
	if ((rbu_data.image_update_buffer == NULL) ||
	    (rbu_data.bios_image_size == 0)) {
		pr_debug("read_rbu_data_mono: image_update_buffer %p ,"
			 "bios_image_size %lu\n",
			 rbu_data.image_update_buffer,
			 rbu_data.bios_image_size);
		ret_count = -ENOMEM;
		goto read_rbu_data_exit;
	}

	if (pos > rbu_data.bios_image_size) {
		ret_count = 0;
		goto read_rbu_data_exit;
	}

	bytes_left = rbu_data.bios_image_size - pos;
	data_length = min(bytes_left, count);

	ptemp = rbu_data.image_update_buffer;
	memcpy(buffer, (ptemp + pos), data_length);

	if ((pos + count) > rbu_data.bios_image_size)
		/* this was the last copy */
		ret_count = bytes_left;
	else
		ret_count = count;
      read_rbu_data_exit:
	return ret_count;
}

static ssize_t
read_rbu_data(struct kobject *kobj, char *buffer, loff_t pos, size_t count)
{
	ssize_t ret_count = 0;

	spin_lock(&rbu_data.lock);

	if (!strcmp(image_type, "mono"))
		ret_count = read_rbu_mono_data(buffer, pos, count);
	else if (!strcmp(image_type, "packet"))
		ret_count = read_packet_data(buffer, pos, count);
	else
		pr_debug("read_rbu_data: invalid image type specified\n");

	spin_unlock(&rbu_data.lock);
	return ret_count;
}

static ssize_t
read_rbu_image_type(struct kobject *kobj, char *buffer, loff_t pos,
		    size_t count)
{
	int size = 0;
	if (!pos)
		size = sprintf(buffer, "%s\n", image_type);
	return size;
}

static ssize_t
write_rbu_image_type(struct kobject *kobj, char *buffer, loff_t pos,
		     size_t count)
{
	int rc = count;
	spin_lock(&rbu_data.lock);

	if (strlen(buffer) < MAX_IMAGE_LENGTH)
		sscanf(buffer, "%s", image_type);
	else
		printk(KERN_WARNING "dell_rbu: image_type is invalid"
		       "max chars = %d, \n incoming str--%s-- \n",
		       MAX_IMAGE_LENGTH, buffer);

	/* we must free all previous allocations */
	packet_empty_list();
	img_update_free();

	spin_unlock(&rbu_data.lock);
	return rc;

}

static struct bin_attribute rbu_data_attr = {
	.attr = {.name = "data",.owner = THIS_MODULE,.mode = 0444},
	.read = read_rbu_data,
};

static struct bin_attribute rbu_image_type_attr = {
	.attr = {.name = "image_type",.owner = THIS_MODULE,.mode = 0644},
	.read = read_rbu_image_type,
	.write = write_rbu_image_type,
};

static void callbackfn_rbu(const struct firmware *fw, void *context)
{
	int rc = 0;

	if (!fw || !fw->size)
		return;

	spin_lock(&rbu_data.lock);
	if (!strcmp(image_type, "mono")) {
		if (!img_update_realloc(fw->size))
			memcpy(rbu_data.image_update_buffer,
			       fw->data, fw->size);
	} else if (!strcmp(image_type, "packet")) {
		if (!rbu_data.packetsize)
			rbu_data.packetsize = fw->size;
		else if (rbu_data.packetsize != fw->size) {
			packet_empty_list();
			rbu_data.packetsize = fw->size;
		}
		packetize_data(fw->data, fw->size);
	} else
		pr_debug("invalid image type specified.\n");
	spin_unlock(&rbu_data.lock);

	rc = request_firmware_nowait(THIS_MODULE, FW_ACTION_NOHOTPLUG,
				     "dell_rbu", &rbu_device->dev,
				     &context, callbackfn_rbu);
	if (rc)
		printk(KERN_ERR
		       "dell_rbu:%s request_firmware_nowait failed"
		       " %d\n", __FUNCTION__, rc);
}

static int __init dcdrbu_init(void)
{
	int rc = 0;
	spin_lock_init(&rbu_data.lock);

	init_packet_head();
	rbu_device =
	    platform_device_register_simple("dell_rbu", -1, NULL, 0);
	if (!rbu_device) {
		printk(KERN_ERR
		       "dell_rbu:%s:platform_device_register_simple "
		       "failed\n", __FUNCTION__);
		return -EIO;
	}

	sysfs_create_bin_file(&rbu_device->dev.kobj, &rbu_data_attr);
	sysfs_create_bin_file(&rbu_device->dev.kobj, &rbu_image_type_attr);

	rc = request_firmware_nowait(THIS_MODULE, FW_ACTION_NOHOTPLUG,
				     "dell_rbu", &rbu_device->dev,
				     &context, callbackfn_rbu);
	if (rc)
		printk(KERN_ERR "dell_rbu:%s:request_firmware_nowait"
		       " failed %d\n", __FUNCTION__, rc);

	return rc;

}

static __exit void dcdrbu_exit(void)
{
	spin_lock(&rbu_data.lock);
	packet_empty_list();
	img_update_free();
	spin_unlock(&rbu_data.lock);
	platform_device_unregister(rbu_device);
}

module_exit(dcdrbu_exit);
module_init(dcdrbu_init);
