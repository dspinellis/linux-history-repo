/*
 * Intel I/OAT DMA Linux driver
 * Copyright(c) 2004 - 2007 Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * The full GNU General Public License is included in this distribution in
 * the file called "COPYING".
 *
 */

/*
 * This driver supports an Intel I/OAT DMA engine, which does asynchronous
 * copy operations.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/dmaengine.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include "ioatdma.h"
#include "ioatdma_registers.h"
#include "ioatdma_hw.h"

#define INITIAL_IOAT_DESC_COUNT 128

#define to_ioat_chan(chan) container_of(chan, struct ioat_dma_chan, common)
#define to_ioatdma_device(dev) container_of(dev, struct ioatdma_device, common)
#define to_ioat_desc(lh) container_of(lh, struct ioat_desc_sw, node)
#define tx_to_ioat_desc(tx) container_of(tx, struct ioat_desc_sw, async_tx)

/* internal functions */
static void ioat_dma_start_null_desc(struct ioat_dma_chan *ioat_chan);
static void ioat_dma_memcpy_cleanup(struct ioat_dma_chan *ioat_chan);

static int ioat_dma_enumerate_channels(struct ioatdma_device *device)
{
	u8 xfercap_scale;
	u32 xfercap;
	int i;
	struct ioat_dma_chan *ioat_chan;

	device->common.chancnt = readb(device->reg_base + IOAT_CHANCNT_OFFSET);
	xfercap_scale = readb(device->reg_base + IOAT_XFERCAP_OFFSET);
	xfercap = (xfercap_scale == 0 ? -1 : (1UL << xfercap_scale));

	for (i = 0; i < device->common.chancnt; i++) {
		ioat_chan = kzalloc(sizeof(*ioat_chan), GFP_KERNEL);
		if (!ioat_chan) {
			device->common.chancnt = i;
			break;
		}

		ioat_chan->device = device;
		ioat_chan->reg_base = device->reg_base + (0x80 * (i + 1));
		ioat_chan->xfercap = xfercap;
		spin_lock_init(&ioat_chan->cleanup_lock);
		spin_lock_init(&ioat_chan->desc_lock);
		INIT_LIST_HEAD(&ioat_chan->free_desc);
		INIT_LIST_HEAD(&ioat_chan->used_desc);
		/* This should be made common somewhere in dmaengine.c */
		ioat_chan->common.device = &device->common;
		list_add_tail(&ioat_chan->common.device_node,
			      &device->common.channels);
	}
	return device->common.chancnt;
}

static void ioat_set_src(dma_addr_t addr,
			 struct dma_async_tx_descriptor *tx,
			 int index)
{
	struct ioat_desc_sw *iter, *desc = tx_to_ioat_desc(tx);
	struct ioat_dma_chan *ioat_chan = to_ioat_chan(tx->chan);

	pci_unmap_addr_set(desc, src, addr);

	list_for_each_entry(iter, &desc->async_tx.tx_list, node) {
		iter->hw->src_addr = addr;
		addr += ioat_chan->xfercap;
	}

}

static void ioat_set_dest(dma_addr_t addr,
			  struct dma_async_tx_descriptor *tx,
			  int index)
{
	struct ioat_desc_sw *iter, *desc = tx_to_ioat_desc(tx);
	struct ioat_dma_chan *ioat_chan = to_ioat_chan(tx->chan);

	pci_unmap_addr_set(desc, dst, addr);

	list_for_each_entry(iter, &desc->async_tx.tx_list, node) {
		iter->hw->dst_addr = addr;
		addr += ioat_chan->xfercap;
	}
}

static dma_cookie_t ioat_tx_submit(struct dma_async_tx_descriptor *tx)
{
	struct ioat_dma_chan *ioat_chan = to_ioat_chan(tx->chan);
	struct ioat_desc_sw *desc = tx_to_ioat_desc(tx);
	int append = 0;
	dma_cookie_t cookie;
	struct ioat_desc_sw *group_start;

	group_start = list_entry(desc->async_tx.tx_list.next,
				 struct ioat_desc_sw, node);
	spin_lock_bh(&ioat_chan->desc_lock);
	/* cookie incr and addition to used_list must be atomic */
	cookie = ioat_chan->common.cookie;
	cookie++;
	if (cookie < 0)
		cookie = 1;
	ioat_chan->common.cookie = desc->async_tx.cookie = cookie;

	/* write address into NextDescriptor field of last desc in chain */
	to_ioat_desc(ioat_chan->used_desc.prev)->hw->next =
						group_start->async_tx.phys;
	list_splice_init(&desc->async_tx.tx_list, ioat_chan->used_desc.prev);

	ioat_chan->pending += desc->tx_cnt;
	if (ioat_chan->pending >= 4) {
		append = 1;
		ioat_chan->pending = 0;
	}
	spin_unlock_bh(&ioat_chan->desc_lock);

	if (append)
		writeb(IOAT_CHANCMD_APPEND,
			ioat_chan->reg_base + IOAT_CHANCMD_OFFSET);

	return cookie;
}

static struct ioat_desc_sw *ioat_dma_alloc_descriptor(
					struct ioat_dma_chan *ioat_chan,
					gfp_t flags)
{
	struct ioat_dma_descriptor *desc;
	struct ioat_desc_sw *desc_sw;
	struct ioatdma_device *ioatdma_device;
	dma_addr_t phys;

	ioatdma_device = to_ioatdma_device(ioat_chan->common.device);
	desc = pci_pool_alloc(ioatdma_device->dma_pool, flags, &phys);
	if (unlikely(!desc))
		return NULL;

	desc_sw = kzalloc(sizeof(*desc_sw), flags);
	if (unlikely(!desc_sw)) {
		pci_pool_free(ioatdma_device->dma_pool, desc, phys);
		return NULL;
	}

	memset(desc, 0, sizeof(*desc));
	dma_async_tx_descriptor_init(&desc_sw->async_tx, &ioat_chan->common);
	desc_sw->async_tx.tx_set_src = ioat_set_src;
	desc_sw->async_tx.tx_set_dest = ioat_set_dest;
	desc_sw->async_tx.tx_submit = ioat_tx_submit;
	INIT_LIST_HEAD(&desc_sw->async_tx.tx_list);
	desc_sw->hw = desc;
	desc_sw->async_tx.phys = phys;

	return desc_sw;
}

/* returns the actual number of allocated descriptors */
static int ioat_dma_alloc_chan_resources(struct dma_chan *chan)
{
	struct ioat_dma_chan *ioat_chan = to_ioat_chan(chan);
	struct ioat_desc_sw *desc = NULL;
	u16 chanctrl;
	u32 chanerr;
	int i;
	LIST_HEAD(tmp_list);

	/* have we already been set up? */
	if (!list_empty(&ioat_chan->free_desc))
		return INITIAL_IOAT_DESC_COUNT;

	/* Setup register to interrupt and write completion status on error */
	chanctrl = IOAT_CHANCTRL_ERR_INT_EN |
		IOAT_CHANCTRL_ANY_ERR_ABORT_EN |
		IOAT_CHANCTRL_ERR_COMPLETION_EN;
	writew(chanctrl, ioat_chan->reg_base + IOAT_CHANCTRL_OFFSET);

	chanerr = readl(ioat_chan->reg_base + IOAT_CHANERR_OFFSET);
	if (chanerr) {
		dev_err(&ioat_chan->device->pdev->dev,
			"ioatdma: CHANERR = %x, clearing\n", chanerr);
		writel(chanerr, ioat_chan->reg_base + IOAT_CHANERR_OFFSET);
	}

	/* Allocate descriptors */
	for (i = 0; i < INITIAL_IOAT_DESC_COUNT; i++) {
		desc = ioat_dma_alloc_descriptor(ioat_chan, GFP_KERNEL);
		if (!desc) {
			dev_err(&ioat_chan->device->pdev->dev,
				"ioatdma: Only %d initial descriptors\n", i);
			break;
		}
		list_add_tail(&desc->node, &tmp_list);
	}
	spin_lock_bh(&ioat_chan->desc_lock);
	list_splice(&tmp_list, &ioat_chan->free_desc);
	spin_unlock_bh(&ioat_chan->desc_lock);

	/* allocate a completion writeback area */
	/* doing 2 32bit writes to mmio since 1 64b write doesn't work */
	ioat_chan->completion_virt =
		pci_pool_alloc(ioat_chan->device->completion_pool,
			       GFP_KERNEL,
			       &ioat_chan->completion_addr);
	memset(ioat_chan->completion_virt, 0,
	       sizeof(*ioat_chan->completion_virt));
	writel(((u64) ioat_chan->completion_addr) & 0x00000000FFFFFFFF,
	       ioat_chan->reg_base + IOAT_CHANCMP_OFFSET_LOW);
	writel(((u64) ioat_chan->completion_addr) >> 32,
	       ioat_chan->reg_base + IOAT_CHANCMP_OFFSET_HIGH);

	ioat_dma_start_null_desc(ioat_chan);
	return i;
}

static void ioat_dma_free_chan_resources(struct dma_chan *chan)
{
	struct ioat_dma_chan *ioat_chan = to_ioat_chan(chan);
	struct ioatdma_device *ioatdma_device = to_ioatdma_device(chan->device);
	struct ioat_desc_sw *desc, *_desc;
	int in_use_descs = 0;

	ioat_dma_memcpy_cleanup(ioat_chan);

	writeb(IOAT_CHANCMD_RESET, ioat_chan->reg_base + IOAT_CHANCMD_OFFSET);

	spin_lock_bh(&ioat_chan->desc_lock);
	list_for_each_entry_safe(desc, _desc, &ioat_chan->used_desc, node) {
		in_use_descs++;
		list_del(&desc->node);
		pci_pool_free(ioatdma_device->dma_pool, desc->hw,
			      desc->async_tx.phys);
		kfree(desc);
	}
	list_for_each_entry_safe(desc, _desc, &ioat_chan->free_desc, node) {
		list_del(&desc->node);
		pci_pool_free(ioatdma_device->dma_pool, desc->hw,
			      desc->async_tx.phys);
		kfree(desc);
	}
	spin_unlock_bh(&ioat_chan->desc_lock);

	pci_pool_free(ioatdma_device->completion_pool,
		      ioat_chan->completion_virt,
		      ioat_chan->completion_addr);

	/* one is ok since we left it on there on purpose */
	if (in_use_descs > 1)
		dev_err(&ioat_chan->device->pdev->dev,
			"ioatdma: Freeing %d in use descriptors!\n",
			in_use_descs - 1);

	ioat_chan->last_completion = ioat_chan->completion_addr = 0;
}

static struct dma_async_tx_descriptor *ioat_dma_prep_memcpy(
						struct dma_chan *chan,
						size_t len,
						int int_en)
{
	struct ioat_dma_chan *ioat_chan = to_ioat_chan(chan);
	struct ioat_desc_sw *first, *prev, *new;
	LIST_HEAD(new_chain);
	u32 copy;
	size_t orig_len;
	int desc_count = 0;

	if (!len)
		return NULL;

	orig_len = len;

	first = NULL;
	prev = NULL;

	spin_lock_bh(&ioat_chan->desc_lock);
	while (len) {
		if (!list_empty(&ioat_chan->free_desc)) {
			new = to_ioat_desc(ioat_chan->free_desc.next);
			list_del(&new->node);
		} else {
			/* try to get another desc */
			new = ioat_dma_alloc_descriptor(ioat_chan, GFP_ATOMIC);
			/* will this ever happen? */
			/* TODO add upper limit on these */
			BUG_ON(!new);
		}

		copy = min((u32) len, ioat_chan->xfercap);

		new->hw->size = copy;
		new->hw->ctl = 0;
		new->async_tx.cookie = 0;
		new->async_tx.ack = 1;

		/* chain together the physical address list for the HW */
		if (!first)
			first = new;
		else
			prev->hw->next = (u64) new->async_tx.phys;

		prev = new;
		len  -= copy;
		list_add_tail(&new->node, &new_chain);
		desc_count++;
	}

	list_splice(&new_chain, &new->async_tx.tx_list);

	new->hw->ctl = IOAT_DMA_DESCRIPTOR_CTL_CP_STS;
	new->hw->next = 0;
	new->tx_cnt = desc_count;
	new->async_tx.ack = 0; /* client is in control of this ack */
	new->async_tx.cookie = -EBUSY;

	pci_unmap_len_set(new, len, orig_len);
	spin_unlock_bh(&ioat_chan->desc_lock);

	return new ? &new->async_tx : NULL;
}

/**
 * ioat_dma_memcpy_issue_pending - push potentially unrecognized appended
 *                                 descriptors to hw
 * @chan: DMA channel handle
 */
static void ioat_dma_memcpy_issue_pending(struct dma_chan *chan)
{
	struct ioat_dma_chan *ioat_chan = to_ioat_chan(chan);

	if (ioat_chan->pending != 0) {
		ioat_chan->pending = 0;
		writeb(IOAT_CHANCMD_APPEND,
		       ioat_chan->reg_base + IOAT_CHANCMD_OFFSET);
	}
}

static void ioat_dma_memcpy_cleanup(struct ioat_dma_chan *ioat_chan)
{
	unsigned long phys_complete;
	struct ioat_desc_sw *desc, *_desc;
	dma_cookie_t cookie = 0;

	prefetch(ioat_chan->completion_virt);

	if (!spin_trylock(&ioat_chan->cleanup_lock))
		return;

	/* The completion writeback can happen at any time,
	   so reads by the driver need to be atomic operations
	   The descriptor physical addresses are limited to 32-bits
	   when the CPU can only do a 32-bit mov */

#if (BITS_PER_LONG == 64)
	phys_complete =
	ioat_chan->completion_virt->full & IOAT_CHANSTS_COMPLETED_DESCRIPTOR_ADDR;
#else
	phys_complete = ioat_chan->completion_virt->low & IOAT_LOW_COMPLETION_MASK;
#endif

	if ((ioat_chan->completion_virt->full & IOAT_CHANSTS_DMA_TRANSFER_STATUS) ==
				IOAT_CHANSTS_DMA_TRANSFER_STATUS_HALTED) {
		dev_err(&ioat_chan->device->pdev->dev,
			"ioatdma: Channel halted, chanerr = %x\n",
			readl(ioat_chan->reg_base + IOAT_CHANERR_OFFSET));

		/* TODO do something to salvage the situation */
	}

	if (phys_complete == ioat_chan->last_completion) {
		spin_unlock(&ioat_chan->cleanup_lock);
		return;
	}

	spin_lock_bh(&ioat_chan->desc_lock);
	list_for_each_entry_safe(desc, _desc, &ioat_chan->used_desc, node) {

		/*
		 * Incoming DMA requests may use multiple descriptors, due to
		 * exceeding xfercap, perhaps. If so, only the last one will
		 * have a cookie, and require unmapping.
		 */
		if (desc->async_tx.cookie) {
			cookie = desc->async_tx.cookie;

			/*
			 * yes we are unmapping both _page and _single alloc'd
			 * regions with unmap_page. Is this *really* that bad?
			 */
			pci_unmap_page(ioat_chan->device->pdev,
					pci_unmap_addr(desc, dst),
					pci_unmap_len(desc, len),
					PCI_DMA_FROMDEVICE);
			pci_unmap_page(ioat_chan->device->pdev,
					pci_unmap_addr(desc, src),
					pci_unmap_len(desc, len),
					PCI_DMA_TODEVICE);
		}

		if (desc->async_tx.phys != phys_complete) {
			/*
			 * a completed entry, but not the last, so cleanup
			 * if the client is done with the descriptor
			 */
			if (desc->async_tx.ack) {
				list_del(&desc->node);
				list_add_tail(&desc->node,
					      &ioat_chan->free_desc);
			} else
				desc->async_tx.cookie = 0;
		} else {
			/*
			 * last used desc. Do not remove, so we can append from
			 * it, but don't look at it next time, either
			 */
			desc->async_tx.cookie = 0;

			/* TODO check status bits? */
			break;
		}
	}

	spin_unlock_bh(&ioat_chan->desc_lock);

	ioat_chan->last_completion = phys_complete;
	if (cookie != 0)
		ioat_chan->completed_cookie = cookie;

	spin_unlock(&ioat_chan->cleanup_lock);
}

static void ioat_dma_dependency_added(struct dma_chan *chan)
{
	struct ioat_dma_chan *ioat_chan = to_ioat_chan(chan);
	spin_lock_bh(&ioat_chan->desc_lock);
	if (ioat_chan->pending == 0) {
		spin_unlock_bh(&ioat_chan->desc_lock);
		ioat_dma_memcpy_cleanup(ioat_chan);
	} else
		spin_unlock_bh(&ioat_chan->desc_lock);
}

/**
 * ioat_dma_is_complete - poll the status of a IOAT DMA transaction
 * @chan: IOAT DMA channel handle
 * @cookie: DMA transaction identifier
 * @done: if not %NULL, updated with last completed transaction
 * @used: if not %NULL, updated with last used transaction
 */
static enum dma_status ioat_dma_is_complete(struct dma_chan *chan,
					    dma_cookie_t cookie,
					    dma_cookie_t *done,
					    dma_cookie_t *used)
{
	struct ioat_dma_chan *ioat_chan = to_ioat_chan(chan);
	dma_cookie_t last_used;
	dma_cookie_t last_complete;
	enum dma_status ret;

	last_used = chan->cookie;
	last_complete = ioat_chan->completed_cookie;

	if (done)
		*done = last_complete;
	if (used)
		*used = last_used;

	ret = dma_async_is_complete(cookie, last_complete, last_used);
	if (ret == DMA_SUCCESS)
		return ret;

	ioat_dma_memcpy_cleanup(ioat_chan);

	last_used = chan->cookie;
	last_complete = ioat_chan->completed_cookie;

	if (done)
		*done = last_complete;
	if (used)
		*used = last_used;

	return dma_async_is_complete(cookie, last_complete, last_used);
}

/* PCI API */

static irqreturn_t ioat_do_interrupt(int irq, void *data)
{
	struct ioatdma_device *instance = data;
	unsigned long attnstatus;
	u8 intrctrl;

	intrctrl = readb(instance->reg_base + IOAT_INTRCTRL_OFFSET);

	if (!(intrctrl & IOAT_INTRCTRL_MASTER_INT_EN))
		return IRQ_NONE;

	if (!(intrctrl & IOAT_INTRCTRL_INT_STATUS)) {
		writeb(intrctrl, instance->reg_base + IOAT_INTRCTRL_OFFSET);
		return IRQ_NONE;
	}

	attnstatus = readl(instance->reg_base + IOAT_ATTNSTATUS_OFFSET);

	printk(KERN_ERR "ioatdma: interrupt! status %lx\n", attnstatus);

	writeb(intrctrl, instance->reg_base + IOAT_INTRCTRL_OFFSET);
	return IRQ_HANDLED;
}

static void ioat_dma_start_null_desc(struct ioat_dma_chan *ioat_chan)
{
	struct ioat_desc_sw *desc;

	spin_lock_bh(&ioat_chan->desc_lock);

	if (!list_empty(&ioat_chan->free_desc)) {
		desc = to_ioat_desc(ioat_chan->free_desc.next);
		list_del(&desc->node);
	} else {
		/* try to get another desc */
		spin_unlock_bh(&ioat_chan->desc_lock);
		desc = ioat_dma_alloc_descriptor(ioat_chan, GFP_KERNEL);
		spin_lock_bh(&ioat_chan->desc_lock);
		/* will this ever happen? */
		BUG_ON(!desc);
	}

	desc->hw->ctl = IOAT_DMA_DESCRIPTOR_NUL;
	desc->hw->next = 0;
	desc->async_tx.ack = 1;

	list_add_tail(&desc->node, &ioat_chan->used_desc);
	spin_unlock_bh(&ioat_chan->desc_lock);

	writel(((u64) desc->async_tx.phys) & 0x00000000FFFFFFFF,
	       ioat_chan->reg_base + IOAT_CHAINADDR_OFFSET_LOW);
	writel(((u64) desc->async_tx.phys) >> 32,
	       ioat_chan->reg_base + IOAT_CHAINADDR_OFFSET_HIGH);

	writeb(IOAT_CHANCMD_START, ioat_chan->reg_base + IOAT_CHANCMD_OFFSET);
}

/*
 * Perform a IOAT transaction to verify the HW works.
 */
#define IOAT_TEST_SIZE 2000

static int ioat_self_test(struct ioatdma_device *device)
{
	int i;
	u8 *src;
	u8 *dest;
	struct dma_chan *dma_chan;
	struct dma_async_tx_descriptor *tx;
	dma_addr_t addr;
	dma_cookie_t cookie;
	int err = 0;

	src = kzalloc(sizeof(u8) * IOAT_TEST_SIZE, GFP_KERNEL);
	if (!src)
		return -ENOMEM;
	dest = kzalloc(sizeof(u8) * IOAT_TEST_SIZE, GFP_KERNEL);
	if (!dest) {
		kfree(src);
		return -ENOMEM;
	}

	/* Fill in src buffer */
	for (i = 0; i < IOAT_TEST_SIZE; i++)
		src[i] = (u8)i;

	/* Start copy, using first DMA channel */
	dma_chan = container_of(device->common.channels.next,
				struct dma_chan,
				device_node);
	if (ioat_dma_alloc_chan_resources(dma_chan) < 1) {
		dev_err(&device->pdev->dev,
			"selftest cannot allocate chan resource\n");
		err = -ENODEV;
		goto out;
	}

	tx = ioat_dma_prep_memcpy(dma_chan, IOAT_TEST_SIZE, 0);
	async_tx_ack(tx);
	addr = dma_map_single(dma_chan->device->dev, src, IOAT_TEST_SIZE,
			DMA_TO_DEVICE);
	ioat_set_src(addr, tx, 0);
	addr = dma_map_single(dma_chan->device->dev, dest, IOAT_TEST_SIZE,
			DMA_FROM_DEVICE);
	ioat_set_dest(addr, tx, 0);
	cookie = ioat_tx_submit(tx);
	ioat_dma_memcpy_issue_pending(dma_chan);
	msleep(1);

	if (ioat_dma_is_complete(dma_chan, cookie, NULL, NULL) != DMA_SUCCESS) {
		dev_err(&device->pdev->dev,
			"ioatdma: Self-test copy timed out, disabling\n");
		err = -ENODEV;
		goto free_resources;
	}
	if (memcmp(src, dest, IOAT_TEST_SIZE)) {
		dev_err(&device->pdev->dev,
			"ioatdma: Self-test copy failed compare, disabling\n");
		err = -ENODEV;
		goto free_resources;
	}

free_resources:
	ioat_dma_free_chan_resources(dma_chan);
out:
	kfree(src);
	kfree(dest);
	return err;
}

struct ioatdma_device *ioat_dma_probe(struct pci_dev *pdev,
				      void __iomem *iobase)
{
	int err;
	struct ioatdma_device *device;

	device = kzalloc(sizeof(*device), GFP_KERNEL);
	if (!device) {
		err = -ENOMEM;
		goto err_kzalloc;
	}
	device->pdev = pdev;
	device->reg_base = iobase;
	device->version = readb(device->reg_base + IOAT_VER_OFFSET);

	/* DMA coherent memory pool for DMA descriptor allocations */
	device->dma_pool = pci_pool_create("dma_desc_pool", pdev,
					   sizeof(struct ioat_dma_descriptor),
					   64, 0);
	if (!device->dma_pool) {
		err = -ENOMEM;
		goto err_dma_pool;
	}

	device->completion_pool = pci_pool_create("completion_pool", pdev,
						  sizeof(u64), SMP_CACHE_BYTES,
						  SMP_CACHE_BYTES);
	if (!device->completion_pool) {
		err = -ENOMEM;
		goto err_completion_pool;
	}

	INIT_LIST_HEAD(&device->common.channels);
	ioat_dma_enumerate_channels(device);

	dma_cap_set(DMA_MEMCPY, device->common.cap_mask);
	device->common.device_alloc_chan_resources =
						ioat_dma_alloc_chan_resources;
	device->common.device_free_chan_resources =
						ioat_dma_free_chan_resources;
	device->common.device_prep_dma_memcpy = ioat_dma_prep_memcpy;
	device->common.device_is_tx_complete = ioat_dma_is_complete;
	device->common.device_issue_pending = ioat_dma_memcpy_issue_pending;
	device->common.device_dependency_added = ioat_dma_dependency_added;
	device->common.dev = &pdev->dev;
	printk(KERN_INFO "ioatdma: Intel(R) I/OAT DMA Engine found,"
	       " %d channels, device version 0x%02x\n",
	       device->common.chancnt, device->version);

	pci_set_drvdata(pdev, device);
	err = request_irq(pdev->irq, &ioat_do_interrupt, IRQF_SHARED, "ioat",
		device);
	if (err)
		goto err_irq;

	writeb(IOAT_INTRCTRL_MASTER_INT_EN,
	       device->reg_base + IOAT_INTRCTRL_OFFSET);
	pci_set_master(pdev);

	err = ioat_self_test(device);
	if (err)
		goto err_self_test;

	dma_async_device_register(&device->common);

	return device;

err_self_test:
	free_irq(device->pdev->irq, device);
err_irq:
	pci_pool_destroy(device->completion_pool);
err_completion_pool:
	pci_pool_destroy(device->dma_pool);
err_dma_pool:
	kfree(device);
err_kzalloc:
	iounmap(iobase);
	printk(KERN_ERR
	       "ioatdma: Intel(R) I/OAT DMA Engine initialization failed\n");
	return NULL;
}

void ioat_dma_remove(struct ioatdma_device *device)
{
	struct dma_chan *chan, *_chan;
	struct ioat_dma_chan *ioat_chan;

	dma_async_device_unregister(&device->common);

	free_irq(device->pdev->irq, device);

	pci_pool_destroy(device->dma_pool);
	pci_pool_destroy(device->completion_pool);

	list_for_each_entry_safe(chan, _chan,
				 &device->common.channels, device_node) {
		ioat_chan = to_ioat_chan(chan);
		list_del(&chan->device_node);
		kfree(ioat_chan);
	}
	kfree(device);
}

