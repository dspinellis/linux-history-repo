/*
 * VME Bridge Framework
 *
 * Author: Martyn Welch <martyn.welch@gefanuc.com>
 * Copyright 2008 GE Fanuc Intelligent Platforms Embedded Systems, Inc.
 *
 * Based on work by Tom Armistead and Ajit Prem
 * Copyright 2004 Motorola Inc.
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

#include <linux/version.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mm.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/pci.h>
#include <linux/poll.h>
#include <linux/highmem.h>
#include <linux/interrupt.h>
#include <linux/pagemap.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/syscalls.h>
#include <linux/semaphore.h>
#include <linux/spinlock.h>

#include "vme.h"
#include "vme_bridge.h"

/* Bitmask and semaphore to keep track of bridge numbers */
static unsigned int vme_bus_numbers;
DECLARE_MUTEX(vme_bus_num_sem);

static void __exit vme_exit (void);
static int __init vme_init (void);


/*
 * Find the bridge resource associated with a specific device resource
 */
static struct vme_bridge *dev_to_bridge(struct device *dev)
{
	return dev->platform_data;
}

/*
 * Find the bridge that the resource is associated with.
 */
static struct vme_bridge *find_bridge(struct vme_resource *resource)
{
	/* Get list to search */
	switch (resource->type) {
	case VME_MASTER:
		return list_entry(resource->entry, struct vme_master_resource,
			list)->parent;
		break;
	case VME_SLAVE:
		return list_entry(resource->entry, struct vme_slave_resource,
			list)->parent;
		break;
	case VME_DMA:
		return list_entry(resource->entry, struct vme_dma_resource,
			list)->parent;
		break;
	default:
		printk(KERN_ERR "Unknown resource type\n");
		return NULL;
		break;
	}
}

/*
 * Allocate a contiguous block of memory for use by the driver. This is used to
 * create the buffers for the slave windows.
 *
 * XXX VME bridges could be available on buses other than PCI. At the momment
 *     this framework only supports PCI devices.
 */
void * vme_alloc_consistent(struct vme_resource *resource, size_t size,
	dma_addr_t *dma)
{
	struct vme_bridge *bridge;
	struct pci_dev *pdev;

	if(resource == NULL) {
		printk("No resource\n");
		return NULL;
	}

	bridge = find_bridge(resource);
	if(bridge == NULL) {
		printk("Can't find bridge\n");
		return NULL;
	}

	/* Find pci_dev container of dev */
	if (bridge->parent == NULL) {
		printk("Dev entry NULL\n");
		return NULL;
	}
	pdev = container_of(bridge->parent, struct pci_dev, dev);

	return pci_alloc_consistent(pdev, size, dma);
}
EXPORT_SYMBOL(vme_alloc_consistent);

/*
 * Free previously allocated contiguous block of memory.
 *
 * XXX VME bridges could be available on buses other than PCI. At the momment
 *     this framework only supports PCI devices.
 */
void vme_free_consistent(struct vme_resource *resource, size_t size,
	void *vaddr, dma_addr_t dma)
{
	struct vme_bridge *bridge;
	struct pci_dev *pdev;

	if(resource == NULL) {
		printk("No resource\n");
		return;
	}

	bridge = find_bridge(resource);
	if(bridge == NULL) {
		printk("Can't find bridge\n");
		return;
	}

	/* Find pci_dev container of dev */
	pdev = container_of(bridge->parent, struct pci_dev, dev);

	pci_free_consistent(pdev, size, vaddr, dma);
}
EXPORT_SYMBOL(vme_free_consistent);

size_t vme_get_size(struct vme_resource *resource)
{
	int enabled, retval;
	unsigned long long base, size;
	dma_addr_t buf_base;
	vme_address_t aspace;
	vme_cycle_t cycle;
	vme_width_t dwidth;

	switch (resource->type) {
	case VME_MASTER:
		retval = vme_master_get(resource, &enabled, &base, &size,
			&aspace, &cycle, &dwidth);

		return size;
		break;
	case VME_SLAVE:
		retval = vme_slave_get(resource, &enabled, &base, &size,
			&buf_base, &aspace, &cycle);

		return size;
		break;
	case VME_DMA:
		return 0;
		break;
	default:
		printk(KERN_ERR "Unknown resource type\n");
		return 0;
		break;
	}
}
EXPORT_SYMBOL(vme_get_size);

static int vme_check_window(vme_address_t aspace, unsigned long long vme_base,
	unsigned long long size)
{
	int retval = 0;

	switch (aspace) {
	case VME_A16:
		if (((vme_base + size) > VME_A16_MAX) ||
				(vme_base > VME_A16_MAX))
			retval = -EFAULT;
		break;
	case VME_A24:
		if (((vme_base + size) > VME_A24_MAX) ||
				(vme_base > VME_A24_MAX))
			retval = -EFAULT;
		break;
	case VME_A32:
		if (((vme_base + size) > VME_A32_MAX) ||
				(vme_base > VME_A32_MAX))
			retval = -EFAULT;
		break;
	case VME_A64:
		/*
		 * Any value held in an unsigned long long can be used as the
		 * base
		 */
		break;
	case VME_CRCSR:
		if (((vme_base + size) > VME_CRCSR_MAX) ||
				(vme_base > VME_CRCSR_MAX))
			retval = -EFAULT;
		break;
	case VME_USER1:
	case VME_USER2:
	case VME_USER3:
	case VME_USER4:
		/* User Defined */
		break;
	default:
		printk("Invalid address space\n");
		retval = -EINVAL;
		break;
	}

	return retval;
}

/*
 * Request a slave image with specific attributes, return some unique
 * identifier.
 */
struct vme_resource * vme_slave_request(struct device *dev,
	vme_address_t address, vme_cycle_t cycle)
{
	struct vme_bridge *bridge;
	struct list_head *slave_pos = NULL;
	struct vme_slave_resource *allocated_image = NULL;
	struct vme_slave_resource *slave_image = NULL;
	struct vme_resource *resource = NULL;

	bridge = dev_to_bridge(dev);
	if (bridge == NULL) {
		printk(KERN_ERR "Can't find VME bus\n");
		goto err_bus;
	}

	/* Loop through slave resources */
	list_for_each(slave_pos, &(bridge->slave_resources)) {
		slave_image = list_entry(slave_pos,
			struct vme_slave_resource, list);

		if (slave_image == NULL) {
			printk("Registered NULL Slave resource\n");
			continue;
		}

		/* Find an unlocked and compatible image */
		down(&(slave_image->sem));
		if(((slave_image->address_attr & address) == address) &&
			((slave_image->cycle_attr & cycle) == cycle) &&
			(slave_image->locked == 0)) {

			slave_image->locked = 1;
			up(&(slave_image->sem));
			allocated_image = slave_image;
			break;
		}
		up(&(slave_image->sem));
	}

	/* No free image */
	if (allocated_image == NULL)
		goto err_image;

	resource = kmalloc(sizeof(struct vme_resource), GFP_KERNEL);
	if (resource == NULL) {
		printk(KERN_WARNING "Unable to allocate resource structure\n");
		goto err_alloc;
	}
	resource->type = VME_SLAVE;
	resource->entry = &(allocated_image->list);

	return resource;

err_alloc:
	/* Unlock image */
	down(&(slave_image->sem));
	slave_image->locked = 0;
	up(&(slave_image->sem));
err_image:
err_bus:
	return NULL;
}
EXPORT_SYMBOL(vme_slave_request);

int vme_slave_set (struct vme_resource *resource, int enabled,
	unsigned long long vme_base, unsigned long long size,
	dma_addr_t buf_base, vme_address_t aspace, vme_cycle_t cycle)
{
	struct vme_bridge *bridge = find_bridge(resource);
	struct vme_slave_resource *image;
	int retval;

	if (resource->type != VME_SLAVE) {
		printk("Not a slave resource\n");
		return -EINVAL;
	}

	image = list_entry(resource->entry, struct vme_slave_resource, list);

	if (bridge->slave_set == NULL) {
		printk("Function not supported\n");
		return -ENOSYS;
	}

	if(!(((image->address_attr & aspace) == aspace) &&
		((image->cycle_attr & cycle) == cycle))) {
		printk("Invalid attributes\n");
		return -EINVAL;
	}

	retval = vme_check_window(aspace, vme_base, size);
	if(retval)
		return retval;

	return bridge->slave_set(image, enabled, vme_base, size, buf_base,
		aspace, cycle);
}
EXPORT_SYMBOL(vme_slave_set);

int vme_slave_get (struct vme_resource *resource, int *enabled,
	unsigned long long *vme_base, unsigned long long *size,
	dma_addr_t *buf_base, vme_address_t *aspace, vme_cycle_t *cycle)
{
	struct vme_bridge *bridge = find_bridge(resource);
	struct vme_slave_resource *image;

	if (resource->type != VME_SLAVE) {
		printk("Not a slave resource\n");
		return -EINVAL;
	}

	image = list_entry(resource->entry, struct vme_slave_resource, list);

	if (bridge->slave_get == NULL) {
		printk("vme_slave_get not supported\n");
		return -EINVAL;
	}

	return bridge->slave_get(image, enabled, vme_base, size, buf_base,
		aspace, cycle);
}
EXPORT_SYMBOL(vme_slave_get);

void vme_slave_free(struct vme_resource *resource)
{
	struct vme_slave_resource *slave_image;

	if (resource->type != VME_SLAVE) {
		printk("Not a slave resource\n");
		return;
	}

	slave_image = list_entry(resource->entry, struct vme_slave_resource,
		list);
	if (slave_image == NULL) {
		printk("Can't find slave resource\n");
		return;
	}

	/* Unlock image */
	down(&(slave_image->sem));
	if (slave_image->locked == 0)
		printk(KERN_ERR "Image is already free\n");

	slave_image->locked = 0;
	up(&(slave_image->sem));

	/* Free up resource memory */
	kfree(resource);
}
EXPORT_SYMBOL(vme_slave_free);

/*
 * Request a master image with specific attributes, return some unique
 * identifier.
 */
struct vme_resource * vme_master_request(struct device *dev,
	vme_address_t address, vme_cycle_t cycle, vme_width_t dwidth)
{
	struct vme_bridge *bridge;
	struct list_head *master_pos = NULL;
	struct vme_master_resource *allocated_image = NULL;
	struct vme_master_resource *master_image = NULL;
	struct vme_resource *resource = NULL;

	bridge = dev_to_bridge(dev);
	if (bridge == NULL) {
		printk(KERN_ERR "Can't find VME bus\n");
		goto err_bus;
	}

	/* Loop through master resources */
	list_for_each(master_pos, &(bridge->master_resources)) {
		master_image = list_entry(master_pos,
			struct vme_master_resource, list);

		if (master_image == NULL) {
			printk(KERN_WARNING "Registered NULL master resource\n");
			continue;
		}

		/* Find an unlocked and compatible image */
		spin_lock(&(master_image->lock));
		if(((master_image->address_attr & address) == address) &&
			((master_image->cycle_attr & cycle) == cycle) &&
			((master_image->width_attr & dwidth) == dwidth) &&
			(master_image->locked == 0)) {

			master_image->locked = 1;
			spin_unlock(&(master_image->lock));
			allocated_image = master_image;
			break;
		}
		spin_unlock(&(master_image->lock));
	}

	/* Check to see if we found a resource */
	if (allocated_image == NULL) {
		printk(KERN_ERR "Can't find a suitable resource\n");
		goto err_image;
	}

	resource = kmalloc(sizeof(struct vme_resource), GFP_KERNEL);
	if (resource == NULL) {
		printk(KERN_ERR "Unable to allocate resource structure\n");
		goto err_alloc;
	}
	resource->type = VME_MASTER;
	resource->entry = &(allocated_image->list);

	return resource;

	kfree(resource);
err_alloc:
	/* Unlock image */
	spin_lock(&(master_image->lock));
	master_image->locked = 0;
	spin_unlock(&(master_image->lock));
err_image:
err_bus:
	return NULL;
}
EXPORT_SYMBOL(vme_master_request);

int vme_master_set (struct vme_resource *resource, int enabled,
	unsigned long long vme_base, unsigned long long size,
	vme_address_t aspace, vme_cycle_t cycle, vme_width_t dwidth)
{
	struct vme_bridge *bridge = find_bridge(resource);
	struct vme_master_resource *image;
	int retval;

	if (resource->type != VME_MASTER) {
		printk("Not a master resource\n");
		return -EINVAL;
	}

	image = list_entry(resource->entry, struct vme_master_resource, list);

	if (bridge->master_set == NULL) {
		printk("vme_master_set not supported\n");
		return -EINVAL;
	}

	if(!(((image->address_attr & aspace) == aspace) &&
		((image->cycle_attr & cycle) == cycle) &&
		((image->width_attr & dwidth) == dwidth))) {
		printk("Invalid attributes\n");
		return -EINVAL;
	}

	retval = vme_check_window(aspace, vme_base, size);
	if(retval)
		return retval;

	return bridge->master_set(image, enabled, vme_base, size, aspace,
		cycle, dwidth);
}
EXPORT_SYMBOL(vme_master_set);

int vme_master_get (struct vme_resource *resource, int *enabled,
	unsigned long long *vme_base, unsigned long long *size,
	vme_address_t *aspace, vme_cycle_t *cycle, vme_width_t *dwidth)
{
	struct vme_bridge *bridge = find_bridge(resource);
	struct vme_master_resource *image;

	if (resource->type != VME_MASTER) {
		printk("Not a master resource\n");
		return -EINVAL;
	}

	image = list_entry(resource->entry, struct vme_master_resource, list);

	if (bridge->master_get == NULL) {
		printk("vme_master_set not supported\n");
		return -EINVAL;
	}

	return bridge->master_get(image, enabled, vme_base, size, aspace,
		cycle, dwidth);
}
EXPORT_SYMBOL(vme_master_get);

/*
 * Read data out of VME space into a buffer.
 */
ssize_t vme_master_read (struct vme_resource *resource, void *buf, size_t count,
	loff_t offset)
{
	struct vme_bridge *bridge = find_bridge(resource);
	struct vme_master_resource *image;
	size_t length;

	if (bridge->master_read == NULL) {
		printk("Reading from resource not supported\n");
		return -EINVAL;
	}

	if (resource->type != VME_MASTER) {
		printk("Not a master resource\n");
		return -EINVAL;
	}

	image = list_entry(resource->entry, struct vme_master_resource, list);

	length = vme_get_size(resource);

	if (offset > length) {
		printk("Invalid Offset\n");
		return -EFAULT;
	}

	if ((offset + count) > length)
		count = length - offset;

	return bridge->master_read(image, buf, count, offset);

}
EXPORT_SYMBOL(vme_master_read);

/*
 * Write data out to VME space from a buffer.
 */
ssize_t vme_master_write (struct vme_resource *resource, void *buf,
	size_t count, loff_t offset)
{
	struct vme_bridge *bridge = find_bridge(resource);
	struct vme_master_resource *image;
	size_t length;

	if (bridge->master_write == NULL) {
		printk("Writing to resource not supported\n");
		return -EINVAL;
	}

	if (resource->type != VME_MASTER) {
		printk("Not a master resource\n");
		return -EINVAL;
	}

	image = list_entry(resource->entry, struct vme_master_resource, list);

	length = vme_get_size(resource);

	if (offset > length) {
		printk("Invalid Offset\n");
		return -EFAULT;
	}

	if ((offset + count) > length)
		count = length - offset;

	return bridge->master_write(image, buf, count, offset);
}
EXPORT_SYMBOL(vme_master_write);

/*
 * Perform RMW cycle to provided location.
 */
unsigned int vme_master_rmw (struct vme_resource *resource, unsigned int mask,
	unsigned int compare, unsigned int swap, loff_t offset)
{
	struct vme_bridge *bridge = find_bridge(resource);
	struct vme_master_resource *image;

	if (bridge->master_rmw == NULL) {
		printk("Writing to resource not supported\n");
		return -EINVAL;
	}

	if (resource->type != VME_MASTER) {
		printk("Not a master resource\n");
		return -EINVAL;
	}

	image = list_entry(resource->entry, struct vme_master_resource, list);

	return bridge->master_rmw(image, mask, compare, swap, offset);
}
EXPORT_SYMBOL(vme_master_rmw);

void vme_master_free(struct vme_resource *resource)
{
	struct vme_master_resource *master_image;

	if (resource->type != VME_MASTER) {
		printk("Not a master resource\n");
		return;
	}

	master_image = list_entry(resource->entry, struct vme_master_resource,
		list);
	if (master_image == NULL) {
		printk("Can't find master resource\n");
		return;
	}

	/* Unlock image */
	spin_lock(&(master_image->lock));
	if (master_image->locked == 0)
		printk(KERN_ERR "Image is already free\n");

	master_image->locked = 0;
	spin_unlock(&(master_image->lock));

	/* Free up resource memory */
	kfree(resource);
}
EXPORT_SYMBOL(vme_master_free);

/*
 * Request a DMA controller with specific attributes, return some unique
 * identifier.
 */
struct vme_resource *vme_request_dma(struct device *dev)
{
	struct vme_bridge *bridge;
	struct list_head *dma_pos = NULL;
	struct vme_dma_resource *allocated_ctrlr = NULL;
	struct vme_dma_resource *dma_ctrlr = NULL;
	struct vme_resource *resource = NULL;

	/* XXX Not checking resource attributes */
	printk(KERN_ERR "No VME resource Attribute tests done\n");

	bridge = dev_to_bridge(dev);
	if (bridge == NULL) {
		printk(KERN_ERR "Can't find VME bus\n");
		goto err_bus;
	}

	/* Loop through DMA resources */
	list_for_each(dma_pos, &(bridge->dma_resources)) {
		dma_ctrlr = list_entry(dma_pos,
			struct vme_dma_resource, list);

		if (dma_ctrlr == NULL) {
			printk("Registered NULL DMA resource\n");
			continue;
		}

		/* Find an unlocked controller */
		down(&(dma_ctrlr->sem));
		if(dma_ctrlr->locked == 0) {
			dma_ctrlr->locked = 1;
			up(&(dma_ctrlr->sem));
			allocated_ctrlr = dma_ctrlr;
			break;
		}
		up(&(dma_ctrlr->sem));
	}

	/* Check to see if we found a resource */
	if (allocated_ctrlr == NULL)
		goto err_ctrlr;

	resource = kmalloc(sizeof(struct vme_resource), GFP_KERNEL);
	if (resource == NULL) {
		printk(KERN_WARNING "Unable to allocate resource structure\n");
		goto err_alloc;
	}
	resource->type = VME_DMA;
	resource->entry = &(allocated_ctrlr->list);

	return resource;

err_alloc:
	/* Unlock image */
	down(&(dma_ctrlr->sem));
	dma_ctrlr->locked = 0;
	up(&(dma_ctrlr->sem));
err_ctrlr:
err_bus:
	return NULL;
}
EXPORT_SYMBOL(vme_request_dma);

/*
 * Start new list
 */
struct vme_dma_list *vme_new_dma_list(struct vme_resource *resource)
{
	struct vme_dma_resource *ctrlr;
	struct vme_dma_list *dma_list;

	if (resource->type != VME_DMA) {
		printk("Not a DMA resource\n");
		return NULL;
	}

	ctrlr = list_entry(resource->entry, struct vme_dma_resource, list);

	dma_list = (struct vme_dma_list *)kmalloc(
		sizeof(struct vme_dma_list), GFP_KERNEL);
	if(dma_list == NULL) {
		printk("Unable to allocate memory for new dma list\n");
		return NULL;
	}
	INIT_LIST_HEAD(&(dma_list->entries));
	dma_list->parent = ctrlr;
	init_MUTEX(&(dma_list->sem));

	return dma_list;
}
EXPORT_SYMBOL(vme_new_dma_list);

/*
 * Create "Pattern" type attributes
 */
struct vme_dma_attr *vme_dma_pattern_attribute(u32 pattern,
	vme_pattern_t type)
{
	struct vme_dma_attr *attributes;
	struct vme_dma_pattern *pattern_attr;

	attributes = (struct vme_dma_attr *)kmalloc(
		sizeof(struct vme_dma_attr), GFP_KERNEL);
	if(attributes == NULL) {
		printk("Unable to allocate memory for attributes structure\n");
		goto err_attr;
	}

	pattern_attr = (struct vme_dma_pattern *)kmalloc(
		sizeof(struct vme_dma_pattern), GFP_KERNEL);
	if(pattern_attr == NULL) {
		printk("Unable to allocate memory for pattern attributes\n");
		goto err_pat;
	}

	attributes->type = VME_DMA_PATTERN;
	attributes->private = (void *)pattern_attr;

	pattern_attr->pattern = pattern;
	pattern_attr->type = type;

	return attributes;

	kfree(pattern_attr);
err_pat:
	kfree(attributes);
err_attr:
	return NULL;
}
EXPORT_SYMBOL(vme_dma_pattern_attribute);

/*
 * Create "PCI" type attributes
 */
struct vme_dma_attr *vme_dma_pci_attribute(dma_addr_t address)
{
	struct vme_dma_attr *attributes;
	struct vme_dma_pci *pci_attr;

	/* XXX Run some sanity checks here */

	attributes = (struct vme_dma_attr *)kmalloc(
		sizeof(struct vme_dma_attr), GFP_KERNEL);
	if(attributes == NULL) {
		printk("Unable to allocate memory for attributes structure\n");
		goto err_attr;
	}

	pci_attr = (struct vme_dma_pci *)kmalloc(sizeof(struct vme_dma_pci),
		GFP_KERNEL);
	if(pci_attr == NULL) {
		printk("Unable to allocate memory for pci attributes\n");
		goto err_pci;
	}



	attributes->type = VME_DMA_PCI;
	attributes->private = (void *)pci_attr;

	pci_attr->address = address;

	return attributes;

	kfree(pci_attr);
err_pci:
	kfree(attributes);
err_attr:
	return NULL;
}
EXPORT_SYMBOL(vme_dma_pci_attribute);

/*
 * Create "VME" type attributes
 */
struct vme_dma_attr *vme_dma_vme_attribute(unsigned long long address,
	vme_address_t aspace, vme_cycle_t cycle, vme_width_t dwidth)
{
	struct vme_dma_attr *attributes;
	struct vme_dma_vme *vme_attr;

	/* XXX Run some sanity checks here */

	attributes = (struct vme_dma_attr *)kmalloc(
		sizeof(struct vme_dma_attr), GFP_KERNEL);
	if(attributes == NULL) {
		printk("Unable to allocate memory for attributes structure\n");
		goto err_attr;
	}

	vme_attr = (struct vme_dma_vme *)kmalloc(sizeof(struct vme_dma_vme),
		GFP_KERNEL);
	if(vme_attr == NULL) {
		printk("Unable to allocate memory for vme attributes\n");
		goto err_vme;
	}

	attributes->type = VME_DMA_VME;
	attributes->private = (void *)vme_attr;

	vme_attr->address = address;
	vme_attr->aspace = aspace;
	vme_attr->cycle = cycle;
	vme_attr->dwidth = dwidth;

	return attributes;

	kfree(vme_attr);
err_vme:
	kfree(attributes);
err_attr:
	return NULL;
}
EXPORT_SYMBOL(vme_dma_vme_attribute);

/*
 * Free attribute
 */
void vme_dma_free_attribute(struct vme_dma_attr *attributes)
{
	kfree(attributes->private);
	kfree(attributes);
}
EXPORT_SYMBOL(vme_dma_free_attribute);

int vme_dma_list_add(struct vme_dma_list *list, struct vme_dma_attr *src,
	struct vme_dma_attr *dest, size_t count)
{
	struct vme_bridge *bridge = list->parent->parent;
	int retval;

	if (bridge->dma_list_add == NULL) {
		printk("Link List DMA generation not supported\n");
		return -EINVAL;
	}

	if (down_trylock(&(list->sem))) {
		printk("Link List already submitted\n");
		return -EINVAL;
	}

	retval = bridge->dma_list_add(list, src, dest, count);

	up(&(list->sem));

	return retval;
}
EXPORT_SYMBOL(vme_dma_list_add);

int vme_dma_list_exec(struct vme_dma_list *list)
{
	struct vme_bridge *bridge = list->parent->parent;
	int retval;

	if (bridge->dma_list_exec == NULL) {
		printk("Link List DMA execution not supported\n");
		return -EINVAL;
	}

	down(&(list->sem));

	retval = bridge->dma_list_exec(list);

	up(&(list->sem));

	return retval;
}
EXPORT_SYMBOL(vme_dma_list_exec);

int vme_dma_list_free(struct vme_dma_list *list)
{
	struct vme_bridge *bridge = list->parent->parent;
	int retval;

	if (bridge->dma_list_empty == NULL) {
		printk("Emptying of Link Lists not supported\n");
		return -EINVAL;
	}

	if (down_trylock(&(list->sem))) {
		printk("Link List in use\n");
		return -EINVAL;
	}

	/*
	 * Empty out all of the entries from the dma list. We need to go to the
	 * low level driver as dma entries are driver specific.
	 */
	retval = bridge->dma_list_empty(list);
	if (retval) {
		printk("Unable to empty link-list entries\n");
		up(&(list->sem));
		return retval;
	}
	up(&(list->sem));
	kfree(list);

	return retval;
}
EXPORT_SYMBOL(vme_dma_list_free);

int vme_dma_free(struct vme_resource *resource)
{
	struct vme_dma_resource *ctrlr;

	if (resource->type != VME_DMA) {
		printk("Not a DMA resource\n");
		return -EINVAL;
	}

	ctrlr = list_entry(resource->entry, struct vme_dma_resource, list);

	if (down_trylock(&(ctrlr->sem))) {
		printk("Resource busy, can't free\n");
		return -EBUSY;
	}

	if (!(list_empty(&(ctrlr->pending)) && list_empty(&(ctrlr->running)))) {
		printk("Resource still processing transfers\n");
		up(&(ctrlr->sem));
		return -EBUSY;
	}

	ctrlr->locked = 0;

	up(&(ctrlr->sem));

	return 0;
}
EXPORT_SYMBOL(vme_dma_free);

int vme_request_irq(struct device *dev, int level, int statid,
	void (*callback)(int level, int vector, void *priv_data),
	void *priv_data)
{
	struct vme_bridge *bridge;

	bridge = dev_to_bridge(dev);
	if (bridge == NULL) {
		printk(KERN_ERR "Can't find VME bus\n");
		return -EINVAL;
	}

	if((level < 1) || (level > 7)) {
		printk(KERN_WARNING "Invalid interrupt level\n");
		return -EINVAL;
	}

	if (bridge->request_irq == NULL) {
		printk("Registering interrupts not supported\n");
		return -EINVAL;
	}

	return bridge->request_irq(level, statid, callback, priv_data);
}
EXPORT_SYMBOL(vme_request_irq);

void vme_free_irq(struct device *dev, int level, int statid)
{
	struct vme_bridge *bridge;

	bridge = dev_to_bridge(dev);
	if (bridge == NULL) {
		printk(KERN_ERR "Can't find VME bus\n");
		return;
	}

	if((level < 1) || (level > 7)) {
		printk(KERN_WARNING "Invalid interrupt level\n");
		return;
	}

	if (bridge->free_irq == NULL) {
		printk("Freeing interrupts not supported\n");
		return;
	}

	bridge->free_irq(level, statid);
}
EXPORT_SYMBOL(vme_free_irq);

int vme_generate_irq(struct device *dev, int level, int statid)
{
	struct vme_bridge *bridge;

	bridge = dev_to_bridge(dev);
	if (bridge == NULL) {
		printk(KERN_ERR "Can't find VME bus\n");
		return -EINVAL;
	}

	if((level < 1) || (level > 7)) {
		printk(KERN_WARNING "Invalid interrupt level\n");
		return -EINVAL;
	}

	if (bridge->generate_irq == NULL) {
		printk("Interrupt generation not supported\n");
		return -EINVAL;
	}

	return bridge->generate_irq(level, statid);
}
EXPORT_SYMBOL(vme_generate_irq);

int vme_lm_set(struct device *dev, unsigned long long lm_base, vme_address_t aspace,
	vme_cycle_t cycle)
{
	struct vme_bridge *bridge;

	bridge = dev_to_bridge(dev);
	if (bridge == NULL) {
		printk(KERN_ERR "Can't find VME bus\n");
		return -EINVAL;
	}

	if (bridge->lm_set == NULL) {
		printk("vme_lm_set not supported\n");
		return -EINVAL;
	}

	return bridge->lm_set(lm_base, aspace, cycle);
}
EXPORT_SYMBOL(vme_lm_set);

int vme_lm_get(struct device *dev, unsigned long long *lm_base, vme_address_t *aspace,
	vme_cycle_t *cycle)
{
	struct vme_bridge *bridge;

	bridge = dev_to_bridge(dev);
	if (bridge == NULL) {
		printk(KERN_ERR "Can't find VME bus\n");
		return -EINVAL;
	}

	if (bridge->lm_get == NULL) {
		printk("vme_lm_get not supported\n");
		return -EINVAL;
	}

	return bridge->lm_get(lm_base, aspace, cycle);
}
EXPORT_SYMBOL(vme_lm_get);

int vme_lm_attach(struct device *dev, int monitor, void (*callback)(int))
{
	struct vme_bridge *bridge;

	bridge = dev_to_bridge(dev);
	if (bridge == NULL) {
		printk(KERN_ERR "Can't find VME bus\n");
		return -EINVAL;
	}

	if (bridge->lm_attach == NULL) {
		printk("vme_lm_attach not supported\n");
		return -EINVAL;
	}

	return bridge->lm_attach(monitor, callback);
}
EXPORT_SYMBOL(vme_lm_attach);

int vme_lm_detach(struct device *dev, int monitor)
{
	struct vme_bridge *bridge;

	bridge = dev_to_bridge(dev);
	if (bridge == NULL) {
		printk(KERN_ERR "Can't find VME bus\n");
		return -EINVAL;
	}

	if (bridge->lm_detach == NULL) {
		printk("vme_lm_detach not supported\n");
		return -EINVAL;
	}

	return bridge->lm_detach(monitor);
}
EXPORT_SYMBOL(vme_lm_detach);

int vme_slot_get(struct device *bus)
{
	struct vme_bridge *bridge;

	bridge = dev_to_bridge(bus);
	if (bridge == NULL) {
		printk(KERN_ERR "Can't find VME bus\n");
		return -EINVAL;
	}

	if (bridge->slot_get == NULL) {
		printk("vme_slot_get not supported\n");
		return -EINVAL;
	}

	return bridge->slot_get();
}
EXPORT_SYMBOL(vme_slot_get);


/* - Bridge Registration --------------------------------------------------- */

static int vme_alloc_bus_num(void)
{
	int i;

	down(&vme_bus_num_sem);
	for (i = 0; i < sizeof(vme_bus_numbers) * 8; i++) {
		if (((vme_bus_numbers >> i) & 0x1) == 0) {
			vme_bus_numbers |= (0x1 << i);
			break;
		}
	}
	up(&vme_bus_num_sem);

	return i;
}

static void vme_free_bus_num(int bus)
{
	down(&vme_bus_num_sem);
	vme_bus_numbers |= ~(0x1 << bus);
	up(&vme_bus_num_sem);
}

int vme_register_bridge (struct vme_bridge *bridge)
{
	struct device *dev;
	int retval;
	int i;

	bridge->num = vme_alloc_bus_num();

	/* This creates 32 vme "slot" devices. This equates to a slot for each
	 * ID available in a system conforming to the ANSI/VITA 1-1994
	 * specification.
	 */
	for (i = 0; i < VME_SLOTS_MAX; i++) {
		dev = &(bridge->dev[i]);
		memset(dev, 0, sizeof(struct device));

		dev->parent = bridge->parent;
		dev->bus = &(vme_bus_type);
		/*
		 * We save a pointer to the bridge in platform_data so that we
		 * can get to it later. We keep driver_data for use by the
		 * driver that binds against the slot
		 */
		dev->platform_data = bridge;
		dev_set_name(dev, "vme-%x.%x", bridge->num, i + 1);

		retval = device_register(dev);
		if(retval)
			goto err_reg;
	}

	return retval;

	i = VME_SLOTS_MAX;
err_reg:
	while (i > -1) {
		dev = &(bridge->dev[i]);
		device_unregister(dev);
	}
	vme_free_bus_num(bridge->num);
	return retval;
}
EXPORT_SYMBOL(vme_register_bridge);

void vme_unregister_bridge (struct vme_bridge *bridge)
{
	int i;
	struct device *dev;


	for (i = 0; i < VME_SLOTS_MAX; i++) {
		dev = &(bridge->dev[i]);
		device_unregister(dev);
	}
	vme_free_bus_num(bridge->num);
}
EXPORT_SYMBOL(vme_unregister_bridge);


/* - Driver Registration --------------------------------------------------- */

int vme_register_driver (struct vme_driver *drv)
{
	drv->driver.name = drv->name;
	drv->driver.bus = &vme_bus_type;

	return driver_register(&drv->driver);
}
EXPORT_SYMBOL(vme_register_driver);

void vme_unregister_driver (struct vme_driver *drv)
{
	driver_unregister(&drv->driver);
}
EXPORT_SYMBOL(vme_unregister_driver);

/* - Bus Registration ------------------------------------------------------ */

int vme_calc_slot(struct device *dev)
{
	struct vme_bridge *bridge;
	int num;

	bridge = dev_to_bridge(dev);

	/* Determine slot number */
	num = 0;
	while(num < VME_SLOTS_MAX) {
		if(&(bridge->dev[num]) == dev) {
			break;
		}
		num++;
	}
	if (num == VME_SLOTS_MAX) {
		dev_err(dev, "Failed to identify slot\n");
		num = 0;
		goto err_dev;
	}
	num++;

err_dev:
	return num;
}

static struct vme_driver *dev_to_vme_driver(struct device *dev)
{
	if(dev->driver == NULL)
		printk("Bugger dev->driver is NULL\n");

	return container_of(dev->driver, struct vme_driver, driver);
}

static int vme_bus_match(struct device *dev, struct device_driver *drv)
{
	struct vme_bridge *bridge;
	struct vme_driver *driver;
	int i, num;

	bridge = dev_to_bridge(dev);
	driver = container_of(drv, struct vme_driver, driver);

	num = vme_calc_slot(dev);
	if (!num)
		goto err_dev;

	if (driver->bind_table == NULL) {
		dev_err(dev, "Bind table NULL\n");
		goto err_table;
	}

	i = 0;
	while((driver->bind_table[i].bus != 0) ||
		(driver->bind_table[i].slot != 0)) {

		if (bridge->num == driver->bind_table[i].bus) {
			if (num == driver->bind_table[i].slot)
				return 1;

			if (driver->bind_table[i].slot == VME_SLOT_ALL)
				return 1;

			if ((driver->bind_table[i].slot == VME_SLOT_CURRENT) &&
				(num == vme_slot_get(dev)))
				return 1;
		}
		i++;
	}

err_dev:
err_table:
	return 0;
}

static int vme_bus_probe(struct device *dev)
{
	struct vme_bridge *bridge;
	struct vme_driver *driver;
	int retval = -ENODEV;

	driver = dev_to_vme_driver(dev);
	bridge = dev_to_bridge(dev);

	if(driver->probe != NULL) {
		retval = driver->probe(dev, bridge->num, vme_calc_slot(dev));
	}

	return retval;
}

static int vme_bus_remove(struct device *dev)
{
	struct vme_bridge *bridge;
	struct vme_driver *driver;
	int retval = -ENODEV;

	driver = dev_to_vme_driver(dev);
	bridge = dev_to_bridge(dev);

	if(driver->remove != NULL) {
		retval = driver->remove(dev, bridge->num, vme_calc_slot(dev));
	}

	return retval;
}

struct bus_type vme_bus_type = {
	.name = "vme",
	.match = vme_bus_match,
	.probe = vme_bus_probe,
	.remove = vme_bus_remove,
};
EXPORT_SYMBOL(vme_bus_type);

static int __init vme_init (void)
{
	return bus_register(&vme_bus_type);
}

static void __exit vme_exit (void)
{
	bus_unregister(&vme_bus_type);
}

MODULE_DESCRIPTION("VME bridge driver framework");
MODULE_AUTHOR("Martyn Welch <martyn.welch@gefanuc.com");
MODULE_LICENSE("GPL");

module_init(vme_init);
module_exit(vme_exit);
