/*						-*- c-basic-offset: 8 -*-
 *
 * fw-ohci.c - Driver for OHCI 1394 boards
 * Copyright (C) 2003-2006 Kristian Hoegsberg <krh@bitplanet.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/pci.h>
#include <linux/delay.h>
#include <linux/poll.h>
#include <linux/dma-mapping.h>

#include <asm/uaccess.h>
#include <asm/semaphore.h>

#include "fw-transaction.h"
#include "fw-ohci.h"

#define descriptor_output_more		0
#define descriptor_output_last		(1 << 12)
#define descriptor_input_more		(2 << 12)
#define descriptor_input_last		(3 << 12)
#define descriptor_status		(1 << 11)
#define descriptor_key_immediate	(2 << 8)
#define descriptor_ping			(1 << 7)
#define descriptor_yy			(1 << 6)
#define descriptor_no_irq		(0 << 4)
#define descriptor_irq_error		(1 << 4)
#define descriptor_irq_always		(3 << 4)
#define descriptor_branch_always	(3 << 2)
#define descriptor_wait			(3 << 0)

struct descriptor {
	__le16 req_count;
	__le16 control;
	__le32 data_address;
	__le32 branch_address;
	__le16 res_count;
	__le16 transfer_status;
} __attribute__((aligned(16)));

struct db_descriptor {
	__le16 first_size;
	__le16 control;
	__le16 second_req_count;
	__le16 first_req_count;
	__le32 branch_address;
	__le16 second_res_count;
	__le16 first_res_count;
	__le32 reserved0;
	__le32 first_buffer;
	__le32 second_buffer;
	__le32 reserved1;
} __attribute__((aligned(16)));

#define control_set(regs)	(regs)
#define control_clear(regs)	((regs) + 4)
#define command_ptr(regs)	((regs) + 12)
#define context_match(regs)	((regs) + 16)

struct ar_buffer {
	struct descriptor descriptor;
	struct ar_buffer *next;
	__le32 data[0];
};

struct ar_context {
	struct fw_ohci *ohci;
	struct ar_buffer *current_buffer;
	struct ar_buffer *last_buffer;
	void *pointer;
	u32 regs;
	struct tasklet_struct tasklet;
};

struct context;

typedef int (*descriptor_callback_t)(struct context *ctx,
				     struct descriptor *d,
				     struct descriptor *last);
struct context {
 	struct fw_ohci *ohci;
	u32 regs;
 
	struct descriptor *buffer;
	dma_addr_t buffer_bus;
	size_t buffer_size;
	struct descriptor *head_descriptor;
	struct descriptor *tail_descriptor;
	struct descriptor *tail_descriptor_last;
	struct descriptor *prev_descriptor;

	descriptor_callback_t callback;

 	struct tasklet_struct tasklet;
};
 


struct at_context {
	struct fw_ohci *ohci;
	dma_addr_t descriptor_bus;
	dma_addr_t buffer_bus;
	struct fw_packet *current_packet;

	struct list_head list;

	struct {
		struct descriptor more;
		__le32 header[4];
		struct descriptor last;
	} d;

	u32 regs;

	struct tasklet_struct tasklet;
};

#define it_header_sy(v)          ((v) <<  0)
#define it_header_tcode(v)       ((v) <<  4)
#define it_header_channel(v)     ((v) <<  8)
#define it_header_tag(v)         ((v) << 14)
#define it_header_speed(v)       ((v) << 16)
#define it_header_data_length(v) ((v) << 16)

struct iso_context {
	struct fw_iso_context base;
	struct context context;
	void *header;
	size_t header_length;
};

#define CONFIG_ROM_SIZE 1024

struct fw_ohci {
	struct fw_card card;

	__iomem char *registers;
	dma_addr_t self_id_bus;
	__le32 *self_id_cpu;
	struct tasklet_struct bus_reset_tasklet;
	int node_id;
	int generation;
	int request_generation;

	/* Spinlock for accessing fw_ohci data.  Never call out of
	 * this driver with this lock held. */
	spinlock_t lock;
	u32 self_id_buffer[512];

	/* Config rom buffers */
	__be32 *config_rom;
	dma_addr_t config_rom_bus;
	__be32 *next_config_rom;
	dma_addr_t next_config_rom_bus;
	u32 next_header;

	struct ar_context ar_request_ctx;
	struct ar_context ar_response_ctx;
	struct at_context at_request_ctx;
	struct at_context at_response_ctx;

	u32 it_context_mask;
	struct iso_context *it_context_list;
	u32 ir_context_mask;
	struct iso_context *ir_context_list;
};

static inline struct fw_ohci *fw_ohci(struct fw_card *card)
{
	return container_of(card, struct fw_ohci, card);
}

#define IT_CONTEXT_CYCLE_MATCH_ENABLE	0x80000000
#define IR_CONTEXT_BUFFER_FILL		0x80000000
#define IR_CONTEXT_ISOCH_HEADER		0x40000000
#define IR_CONTEXT_CYCLE_MATCH_ENABLE	0x20000000
#define IR_CONTEXT_MULTI_CHANNEL_MODE	0x10000000
#define IR_CONTEXT_DUAL_BUFFER_MODE	0x08000000

#define CONTEXT_RUN	0x8000
#define CONTEXT_WAKE	0x1000
#define CONTEXT_DEAD	0x0800
#define CONTEXT_ACTIVE	0x0400

#define OHCI1394_MAX_AT_REQ_RETRIES	0x2
#define OHCI1394_MAX_AT_RESP_RETRIES	0x2
#define OHCI1394_MAX_PHYS_RESP_RETRIES	0x8

#define FW_OHCI_MAJOR			240
#define OHCI1394_REGISTER_SIZE		0x800
#define OHCI_LOOP_COUNT			500
#define OHCI1394_PCI_HCI_Control	0x40
#define SELF_ID_BUF_SIZE		0x800
#define OHCI_TCODE_PHY_PACKET		0x0e

static char ohci_driver_name[] = KBUILD_MODNAME;

static inline void reg_write(const struct fw_ohci *ohci, int offset, u32 data)
{
	writel(data, ohci->registers + offset);
}

static inline u32 reg_read(const struct fw_ohci *ohci, int offset)
{
	return readl(ohci->registers + offset);
}

static inline void flush_writes(const struct fw_ohci *ohci)
{
	/* Do a dummy read to flush writes. */
	reg_read(ohci, OHCI1394_Version);
}

static int
ohci_update_phy_reg(struct fw_card *card, int addr,
		    int clear_bits, int set_bits)
{
	struct fw_ohci *ohci = fw_ohci(card);
	u32 val, old;

	reg_write(ohci, OHCI1394_PhyControl, OHCI1394_PhyControl_Read(addr));
	msleep(2);
	val = reg_read(ohci, OHCI1394_PhyControl);
	if ((val & OHCI1394_PhyControl_ReadDone) == 0) {
		fw_error("failed to set phy reg bits.\n");
		return -EBUSY;
	}

	old = OHCI1394_PhyControl_ReadData(val);
	old = (old & ~clear_bits) | set_bits;
	reg_write(ohci, OHCI1394_PhyControl,
		  OHCI1394_PhyControl_Write(addr, old));

	return 0;
}

static int ar_context_add_page(struct ar_context *ctx)
{
	struct device *dev = ctx->ohci->card.device;
	struct ar_buffer *ab;
	dma_addr_t ab_bus;
	size_t offset;

	ab = (struct ar_buffer *) __get_free_page(GFP_ATOMIC);
	if (ab == NULL)
		return -ENOMEM;

	ab_bus = dma_map_single(dev, ab, PAGE_SIZE, DMA_BIDIRECTIONAL);
	if (dma_mapping_error(ab_bus)) {
		free_page((unsigned long) ab);
		return -ENOMEM;
	}

	memset(&ab->descriptor, 0, sizeof ab->descriptor);
	ab->descriptor.control        = cpu_to_le16(descriptor_input_more |
						    descriptor_status |
						    descriptor_branch_always);
	offset = offsetof(struct ar_buffer, data);
	ab->descriptor.req_count      = cpu_to_le16(PAGE_SIZE - offset);
	ab->descriptor.data_address   = cpu_to_le32(ab_bus + offset);
	ab->descriptor.res_count      = cpu_to_le16(PAGE_SIZE - offset);
	ab->descriptor.branch_address = 0;

	dma_sync_single_for_device(dev, ab_bus, PAGE_SIZE, DMA_BIDIRECTIONAL);

	ctx->last_buffer->descriptor.branch_address = ab_bus | 1;
	ctx->last_buffer->next = ab;
	ctx->last_buffer = ab;

	reg_write(ctx->ohci, control_set(ctx->regs), CONTEXT_WAKE);
	flush_writes(ctx->ohci);

	return 0;
}

static __le32 *handle_ar_packet(struct ar_context *ctx, __le32 *buffer)
{
	struct fw_ohci *ohci = ctx->ohci;
	struct fw_packet p;
	u32 status, length, tcode;

	p.header[0] = le32_to_cpu(buffer[0]);
	p.header[1] = le32_to_cpu(buffer[1]);
	p.header[2] = le32_to_cpu(buffer[2]);

	tcode = (p.header[0] >> 4) & 0x0f;
	switch (tcode) {
	case TCODE_WRITE_QUADLET_REQUEST:
	case TCODE_READ_QUADLET_RESPONSE:
		p.header[3] = (__force __u32) buffer[3];
		p.header_length = 16;
		p.payload_length = 0;
		break;

	case TCODE_READ_BLOCK_REQUEST :
		p.header[3] = le32_to_cpu(buffer[3]);
		p.header_length = 16;
		p.payload_length = 0;
		break;

	case TCODE_WRITE_BLOCK_REQUEST:
	case TCODE_READ_BLOCK_RESPONSE:
	case TCODE_LOCK_REQUEST:
	case TCODE_LOCK_RESPONSE:
		p.header[3] = le32_to_cpu(buffer[3]);
		p.header_length = 16;
		p.payload_length = p.header[3] >> 16;
		break;

	case TCODE_WRITE_RESPONSE:
	case TCODE_READ_QUADLET_REQUEST:
	case OHCI_TCODE_PHY_PACKET:
		p.header_length = 12;
		p.payload_length = 0;
		break;
	}

	p.payload = (void *) buffer + p.header_length;

	/* FIXME: What to do about evt_* errors? */
	length = (p.header_length + p.payload_length + 3) / 4;
	status = le32_to_cpu(buffer[length]);

	p.ack        = ((status >> 16) & 0x1f) - 16;
	p.speed      = (status >> 21) & 0x7;
	p.timestamp  = status & 0xffff;
	p.generation = ohci->request_generation;

	/* The OHCI bus reset handler synthesizes a phy packet with
	 * the new generation number when a bus reset happens (see
	 * section 8.4.2.3).  This helps us determine when a request
	 * was received and make sure we send the response in the same
	 * generation.  We only need this for requests; for responses
	 * we use the unique tlabel for finding the matching
	 * request. */

	if (p.ack + 16 == 0x09)
		ohci->request_generation = (buffer[2] >> 16) & 0xff;
	else if (ctx == &ohci->ar_request_ctx)
		fw_core_handle_request(&ohci->card, &p);
	else
		fw_core_handle_response(&ohci->card, &p);

	return buffer + length + 1;
}

static void ar_context_tasklet(unsigned long data)
{
	struct ar_context *ctx = (struct ar_context *)data;
	struct fw_ohci *ohci = ctx->ohci;
	struct ar_buffer *ab;
	struct descriptor *d;
	void *buffer, *end;

	ab = ctx->current_buffer;
	d = &ab->descriptor;

	if (d->res_count == 0) {
		size_t size, rest, offset;

		/* This descriptor is finished and we may have a
		 * packet split across this and the next buffer. We
		 * reuse the page for reassembling the split packet. */

		offset = offsetof(struct ar_buffer, data);
		dma_unmap_single(ohci->card.device,
				 ab->descriptor.data_address - offset,
				 PAGE_SIZE, DMA_BIDIRECTIONAL);

		buffer = ab;
		ab = ab->next;
		d = &ab->descriptor;
		size = buffer + PAGE_SIZE - ctx->pointer;
		rest = le16_to_cpu(d->req_count) - le16_to_cpu(d->res_count);
		memmove(buffer, ctx->pointer, size);
		memcpy(buffer + size, ab->data, rest);
		ctx->current_buffer = ab;
		ctx->pointer = (void *) ab->data + rest;
		end = buffer + size + rest;

		while (buffer < end)
			buffer = handle_ar_packet(ctx, buffer);

		free_page((unsigned long)buffer);
		ar_context_add_page(ctx);
	} else {
		buffer = ctx->pointer;
		ctx->pointer = end =
			(void *) ab + PAGE_SIZE - le16_to_cpu(d->res_count);

		while (buffer < end)
			buffer = handle_ar_packet(ctx, buffer);
	}
}

static int
ar_context_init(struct ar_context *ctx, struct fw_ohci *ohci, u32 regs)
{
	struct ar_buffer ab;

	ctx->regs        = regs;
	ctx->ohci        = ohci;
	ctx->last_buffer = &ab;
	tasklet_init(&ctx->tasklet, ar_context_tasklet, (unsigned long)ctx);

	ar_context_add_page(ctx);
	ar_context_add_page(ctx);
	ctx->current_buffer = ab.next;
	ctx->pointer = ctx->current_buffer->data;

	reg_write(ctx->ohci, command_ptr(ctx->regs), ab.descriptor.branch_address);
	reg_write(ctx->ohci, control_set(ctx->regs), CONTEXT_RUN);
	flush_writes(ctx->ohci);

	return 0;
}
 
static void context_tasklet(unsigned long data)
{
	struct context *ctx = (struct context *) data;
	struct fw_ohci *ohci = ctx->ohci;
	struct descriptor *d, *last;
	u32 address;
	int z;

	dma_sync_single_for_cpu(ohci->card.device, ctx->buffer_bus,
				ctx->buffer_size, DMA_TO_DEVICE);

	d    = ctx->tail_descriptor;
	last = ctx->tail_descriptor_last;

	while (last->branch_address != 0) {
		address = le32_to_cpu(last->branch_address);
		z = address & 0xf;
		d = ctx->buffer + (address - ctx->buffer_bus) / sizeof *d;
		last = (z == 2) ? d : d + z - 1;

		if (!ctx->callback(ctx, d, last))
			break;

		ctx->tail_descriptor      = d;
		ctx->tail_descriptor_last = last;
	}
}

static int
context_init(struct context *ctx, struct fw_ohci *ohci,
	     size_t buffer_size, u32 regs,
	     descriptor_callback_t callback)
{
	ctx->ohci = ohci;
	ctx->regs = regs;
	ctx->buffer_size = buffer_size;
	ctx->buffer = kmalloc(buffer_size, GFP_KERNEL);
	if (ctx->buffer == NULL)
		return -ENOMEM;

	tasklet_init(&ctx->tasklet, context_tasklet, (unsigned long)ctx);
	ctx->callback = callback;

	ctx->buffer_bus =
		dma_map_single(ohci->card.device, ctx->buffer,
			       buffer_size, DMA_TO_DEVICE);
	if (dma_mapping_error(ctx->buffer_bus)) {
		kfree(ctx->buffer);
		return -ENOMEM;
	}

	ctx->head_descriptor      = ctx->buffer;
	ctx->prev_descriptor      = ctx->buffer;
	ctx->tail_descriptor      = ctx->buffer;
	ctx->tail_descriptor_last = ctx->buffer;

	/* We put a dummy descriptor in the buffer that has a NULL
	 * branch address and looks like it's been sent.  That way we
	 * have a descriptor to append DMA programs to.  Also, the
	 * ring buffer invariant is that it always has at least one
	 * element so that head == tail means buffer full. */

	memset(ctx->head_descriptor, 0, sizeof *ctx->head_descriptor);
	ctx->head_descriptor->control = cpu_to_le16(descriptor_output_last);
	ctx->head_descriptor->transfer_status = cpu_to_le16(0x8011);
	ctx->head_descriptor++;

	return 0;
}

static void
context_release(struct context *ctx)
{
	struct fw_card *card = &ctx->ohci->card;

	dma_unmap_single(card->device, ctx->buffer_bus,
			 ctx->buffer_size, DMA_TO_DEVICE);
	kfree(ctx->buffer);
}

static struct descriptor *
context_get_descriptors(struct context *ctx, int z, dma_addr_t *d_bus)
{
	struct descriptor *d, *tail, *end;

	d = ctx->head_descriptor;
	tail = ctx->tail_descriptor;
	end = ctx->buffer + ctx->buffer_size / sizeof(struct descriptor);

	if (d + z <= tail) {
		goto has_space;
	} else if (d > tail && d + z <= end) {
		goto has_space;
	} else if (d > tail && ctx->buffer + z <= tail) {
		d = ctx->buffer;
		goto has_space;
	}

	return NULL;

 has_space:
	memset(d, 0, z * sizeof *d);
	*d_bus = ctx->buffer_bus + (d - ctx->buffer) * sizeof *d;

	return d;
}

static void context_run(struct context *ctx, u32 extra)
{
	struct fw_ohci *ohci = ctx->ohci;

	reg_write(ohci, command_ptr(ctx->regs),
		  le32_to_cpu(ctx->tail_descriptor_last->branch_address));
	reg_write(ohci, control_clear(ctx->regs), ~0);
	reg_write(ohci, control_set(ctx->regs), CONTEXT_RUN | extra);
	flush_writes(ohci);
}

static void context_append(struct context *ctx,
			   struct descriptor *d, int z, int extra)
{
	dma_addr_t d_bus;

	d_bus = ctx->buffer_bus + (d - ctx->buffer) * sizeof *d;

	ctx->head_descriptor = d + z + extra;
	ctx->prev_descriptor->branch_address = cpu_to_le32(d_bus | z);
	ctx->prev_descriptor = z == 2 ? d : d + z - 1;

	dma_sync_single_for_device(ctx->ohci->card.device, ctx->buffer_bus,
				   ctx->buffer_size, DMA_TO_DEVICE);

	reg_write(ctx->ohci, control_set(ctx->regs), CONTEXT_WAKE);
	flush_writes(ctx->ohci);
}

static void context_stop(struct context *ctx)
{
	u32 reg;
	int i;

	reg_write(ctx->ohci, control_clear(ctx->regs), CONTEXT_RUN);
	flush_writes(ctx->ohci);

	for (i = 0; i < 10; i++) {
		reg = reg_read(ctx->ohci, control_set(ctx->regs));
		if ((reg & CONTEXT_ACTIVE) == 0)
			break;

		fw_notify("context_stop: still active (0x%08x)\n", reg);
		msleep(1);
	}
}

static void
do_packet_callbacks(struct fw_ohci *ohci, struct list_head *list)
{
	struct fw_packet *p, *next;

	list_for_each_entry_safe(p, next, list, link)
		p->callback(p, &ohci->card, p->ack);
}

static void
complete_transmission(struct fw_packet *packet,
		      int ack, struct list_head *list)
{
	list_move_tail(&packet->link, list);
	packet->ack = ack;
}

/* This function prepares the first packet in the context queue for
 * transmission.  Must always be called with the ochi->lock held to
 * ensure proper generation handling and locking around packet queue
 * manipulation. */
static void
at_context_setup_packet(struct at_context *ctx, struct list_head *list)
{
	struct fw_packet *packet;
	struct fw_ohci *ohci = ctx->ohci;
	int z, tcode;

	packet = fw_packet(ctx->list.next);

	memset(&ctx->d, 0, sizeof ctx->d);
	if (packet->payload_length > 0) {
		packet->payload_bus = dma_map_single(ohci->card.device,
						     packet->payload,
						     packet->payload_length,
						     DMA_TO_DEVICE);
		if (dma_mapping_error(packet->payload_bus)) {
			complete_transmission(packet, RCODE_SEND_ERROR, list);
			return;
		}

		ctx->d.more.control      =
			cpu_to_le16(descriptor_output_more |
				    descriptor_key_immediate);
		ctx->d.more.req_count    = cpu_to_le16(packet->header_length);
		ctx->d.more.res_count    = cpu_to_le16(packet->timestamp);
		ctx->d.last.control      =
			cpu_to_le16(descriptor_output_last |
				    descriptor_irq_always |
				    descriptor_branch_always);
		ctx->d.last.req_count    = cpu_to_le16(packet->payload_length);
		ctx->d.last.data_address = cpu_to_le32(packet->payload_bus);
		z = 3;
	} else {
		ctx->d.more.control   =
			cpu_to_le16(descriptor_output_last |
				    descriptor_key_immediate |
				    descriptor_irq_always |
				    descriptor_branch_always);
		ctx->d.more.req_count = cpu_to_le16(packet->header_length);
		ctx->d.more.res_count = cpu_to_le16(packet->timestamp);
		z = 2;
	}

	/* The DMA format for asyncronous link packets is different
	 * from the IEEE1394 layout, so shift the fields around
	 * accordingly.  If header_length is 8, it's a PHY packet, to
	 * which we need to prepend an extra quadlet. */
	if (packet->header_length > 8) {
		ctx->d.header[0] = cpu_to_le32((packet->header[0] & 0xffff) |
					       (packet->speed << 16));
		ctx->d.header[1] = cpu_to_le32((packet->header[1] & 0xffff) |
					       (packet->header[0] & 0xffff0000));
		ctx->d.header[2] = cpu_to_le32(packet->header[2]);

		tcode = (packet->header[0] >> 4) & 0x0f;
		if (TCODE_IS_BLOCK_PACKET(tcode))
			ctx->d.header[3] = cpu_to_le32(packet->header[3]);
		else
			ctx->d.header[3] = packet->header[3];
	} else {
		ctx->d.header[0] =
			cpu_to_le32((OHCI1394_phy_tcode << 4) |
				    (packet->speed << 16));
		ctx->d.header[1] = cpu_to_le32(packet->header[0]);
		ctx->d.header[2] = cpu_to_le32(packet->header[1]);
		ctx->d.more.req_count = cpu_to_le16(12);
	}

	/* FIXME: Document how the locking works. */
	if (ohci->generation == packet->generation) {
		reg_write(ctx->ohci, command_ptr(ctx->regs),
			  ctx->descriptor_bus | z);
		reg_write(ctx->ohci, control_set(ctx->regs),
			  CONTEXT_RUN | CONTEXT_WAKE);
		ctx->current_packet = packet;
	} else {
		/* We dont return error codes from this function; all
		 * transmission errors are reported through the
		 * callback. */
		complete_transmission(packet, RCODE_GENERATION, list);
	}
}

static void at_context_stop(struct at_context *ctx)
{
	u32 reg;

	reg_write(ctx->ohci, control_clear(ctx->regs), CONTEXT_RUN);

	reg = reg_read(ctx->ohci, control_set(ctx->regs));
	if (reg & CONTEXT_ACTIVE)
		fw_notify("Tried to stop context, but it is still active "
			  "(0x%08x).\n", reg);
}

static void at_context_tasklet(unsigned long data)
{
	struct at_context *ctx = (struct at_context *)data;
	struct fw_ohci *ohci = ctx->ohci;
	struct fw_packet *packet;
	LIST_HEAD(list);
	unsigned long flags;
	int evt;

	spin_lock_irqsave(&ohci->lock, flags);

	packet = fw_packet(ctx->list.next);

	at_context_stop(ctx);

	/* If the head of the list isn't the packet that just got
	 * transmitted, the packet got cancelled before we finished
	 * transmitting it. */
	if (ctx->current_packet != packet)
		goto skip_to_next;

	if (packet->payload_length > 0) {
		dma_unmap_single(ohci->card.device, packet->payload_bus,
				 packet->payload_length, DMA_TO_DEVICE);
		evt = le16_to_cpu(ctx->d.last.transfer_status) & 0x1f;
		packet->timestamp = le16_to_cpu(ctx->d.last.res_count);
	}
	else {
		evt = le16_to_cpu(ctx->d.more.transfer_status) & 0x1f;
		packet->timestamp = le16_to_cpu(ctx->d.more.res_count);
	}

	if (evt < 16) {
		switch (evt) {
		case OHCI1394_evt_timeout:
			/* Async response transmit timed out. */
			complete_transmission(packet, RCODE_CANCELLED, &list);
			break;

		case OHCI1394_evt_flushed:
			/* The packet was flushed should give same
			 * error as when we try to use a stale
			 * generation count. */
			complete_transmission(packet,
					      RCODE_GENERATION, &list);
			break;

		case OHCI1394_evt_missing_ack:
			/* Using a valid (current) generation count,
			 * but the node is not on the bus or not
			 * sending acks. */
			complete_transmission(packet, RCODE_NO_ACK, &list);
			break;

		default:
			complete_transmission(packet, RCODE_SEND_ERROR, &list);
			break;
		}
	} else
		complete_transmission(packet, evt - 16, &list);

 skip_to_next:
	/* If more packets are queued, set up the next one. */
	if (!list_empty(&ctx->list))
		at_context_setup_packet(ctx, &list);

	spin_unlock_irqrestore(&ohci->lock, flags);

	do_packet_callbacks(ohci, &list);
}

static int
at_context_init(struct at_context *ctx, struct fw_ohci *ohci, u32 regs)
{
	INIT_LIST_HEAD(&ctx->list);

	ctx->descriptor_bus =
		dma_map_single(ohci->card.device, &ctx->d,
			       sizeof ctx->d, DMA_TO_DEVICE);
	if (dma_mapping_error(ctx->descriptor_bus))
		return -ENOMEM;

	ctx->regs = regs;
	ctx->ohci = ohci;

	tasklet_init(&ctx->tasklet, at_context_tasklet, (unsigned long)ctx);

	return 0;
}

#define header_get_destination(q)	(((q) >> 16) & 0xffff)
#define header_get_tcode(q)		(((q) >> 4) & 0x0f)
#define header_get_offset_high(q)	(((q) >> 0) & 0xffff)
#define header_get_data_length(q)	(((q) >> 16) & 0xffff)
#define header_get_extended_tcode(q)	(((q) >> 0) & 0xffff)

static void
handle_local_rom(struct fw_ohci *ohci, struct fw_packet *packet, u32 csr)
{
	struct fw_packet response;
	int tcode, length, i;

	tcode = header_get_tcode(packet->header[0]);
	if (TCODE_IS_BLOCK_PACKET(tcode))
		length = header_get_data_length(packet->header[3]);
	else
		length = 4;

	i = csr - CSR_CONFIG_ROM;
	if (i + length > CONFIG_ROM_SIZE) {
		fw_fill_response(&response, packet->header,
				 RCODE_ADDRESS_ERROR, NULL, 0);
	} else if (!TCODE_IS_READ_REQUEST(tcode)) {
		fw_fill_response(&response, packet->header,
				 RCODE_TYPE_ERROR, NULL, 0);
	} else {
		fw_fill_response(&response, packet->header, RCODE_COMPLETE,
				 (void *) ohci->config_rom + i, length);
	}

	fw_core_handle_response(&ohci->card, &response);
}

static void
handle_local_lock(struct fw_ohci *ohci, struct fw_packet *packet, u32 csr)
{
	struct fw_packet response;
	int tcode, length, ext_tcode, sel;
	__be32 *payload, lock_old;
	u32 lock_arg, lock_data;

	tcode = header_get_tcode(packet->header[0]);
	length = header_get_data_length(packet->header[3]);
	payload = packet->payload;
	ext_tcode = header_get_extended_tcode(packet->header[3]);

	if (tcode == TCODE_LOCK_REQUEST &&
	    ext_tcode == EXTCODE_COMPARE_SWAP && length == 8) {
		lock_arg = be32_to_cpu(payload[0]);
		lock_data = be32_to_cpu(payload[1]);
	} else if (tcode == TCODE_READ_QUADLET_REQUEST) {
		lock_arg = 0;
		lock_data = 0;
	} else {
		fw_fill_response(&response, packet->header,
				 RCODE_TYPE_ERROR, NULL, 0);
		goto out;
	}

	sel = (csr - CSR_BUS_MANAGER_ID) / 4;
	reg_write(ohci, OHCI1394_CSRData, lock_data);
	reg_write(ohci, OHCI1394_CSRCompareData, lock_arg);
	reg_write(ohci, OHCI1394_CSRControl, sel);

	if (reg_read(ohci, OHCI1394_CSRControl) & 0x80000000)
		lock_old = cpu_to_be32(reg_read(ohci, OHCI1394_CSRData));
	else
		fw_notify("swap not done yet\n");

	fw_fill_response(&response, packet->header,
			 RCODE_COMPLETE, &lock_old, sizeof lock_old);
 out:
	fw_core_handle_response(&ohci->card, &response);
}

static void
handle_local_request(struct at_context *ctx, struct fw_packet *packet)
{
	u64 offset;
	u32 csr;

	packet->ack = ACK_PENDING;
	packet->callback(packet, &ctx->ohci->card, packet->ack);

	offset =
		((unsigned long long)
		 header_get_offset_high(packet->header[1]) << 32) |
		packet->header[2];
	csr = offset - CSR_REGISTER_BASE;

	/* Handle config rom reads. */
	if (csr >= CSR_CONFIG_ROM && csr < CSR_CONFIG_ROM_END)
		handle_local_rom(ctx->ohci, packet, csr);
	else switch (csr) {
	case CSR_BUS_MANAGER_ID:
	case CSR_BANDWIDTH_AVAILABLE:
	case CSR_CHANNELS_AVAILABLE_HI:
	case CSR_CHANNELS_AVAILABLE_LO:
		handle_local_lock(ctx->ohci, packet, csr);
		break;
	default:
		if (ctx == &ctx->ohci->at_request_ctx)
			fw_core_handle_request(&ctx->ohci->card, packet);
		else
			fw_core_handle_response(&ctx->ohci->card, packet);
		break;
	}
}

static void
at_context_transmit(struct at_context *ctx, struct fw_packet *packet)
{
	LIST_HEAD(list);
	unsigned long flags;

	spin_lock_irqsave(&ctx->ohci->lock, flags);

	if (header_get_destination(packet->header[0]) == ctx->ohci->node_id &&
	    ctx->ohci->generation == packet->generation) {
		spin_unlock_irqrestore(&ctx->ohci->lock, flags);
		handle_local_request(ctx, packet);
		return;
	}

	list_add_tail(&packet->link, &ctx->list);
	if (ctx->list.next == &packet->link)
		at_context_setup_packet(ctx, &list);

	spin_unlock_irqrestore(&ctx->ohci->lock, flags);

	do_packet_callbacks(ctx->ohci, &list);
}

static void bus_reset_tasklet(unsigned long data)
{
	struct fw_ohci *ohci = (struct fw_ohci *)data;
	int self_id_count, i, j, reg;
	int generation, new_generation;
	unsigned long flags;

	reg = reg_read(ohci, OHCI1394_NodeID);
	if (!(reg & OHCI1394_NodeID_idValid)) {
		fw_error("node ID not valid, new bus reset in progress\n");
		return;
	}
	ohci->node_id = reg & 0xffff;

	/* The count in the SelfIDCount register is the number of
	 * bytes in the self ID receive buffer.  Since we also receive
	 * the inverted quadlets and a header quadlet, we shift one
	 * bit extra to get the actual number of self IDs. */

	self_id_count = (reg_read(ohci, OHCI1394_SelfIDCount) >> 3) & 0x3ff;
	generation = (le32_to_cpu(ohci->self_id_cpu[0]) >> 16) & 0xff;

	for (i = 1, j = 0; j < self_id_count; i += 2, j++) {
		if (ohci->self_id_cpu[i] != ~ohci->self_id_cpu[i + 1])
			fw_error("inconsistent self IDs\n");
		ohci->self_id_buffer[j] = le32_to_cpu(ohci->self_id_cpu[i]);
	}

	/* Check the consistency of the self IDs we just read.  The
	 * problem we face is that a new bus reset can start while we
	 * read out the self IDs from the DMA buffer. If this happens,
	 * the DMA buffer will be overwritten with new self IDs and we
	 * will read out inconsistent data.  The OHCI specification
	 * (section 11.2) recommends a technique similar to
	 * linux/seqlock.h, where we remember the generation of the
	 * self IDs in the buffer before reading them out and compare
	 * it to the current generation after reading them out.  If
	 * the two generations match we know we have a consistent set
	 * of self IDs. */

	new_generation = (reg_read(ohci, OHCI1394_SelfIDCount) >> 16) & 0xff;
	if (new_generation != generation) {
		fw_notify("recursive bus reset detected, "
			  "discarding self ids\n");
		return;
	}

	/* FIXME: Document how the locking works. */
	spin_lock_irqsave(&ohci->lock, flags);

	ohci->generation = generation;
	at_context_stop(&ohci->at_request_ctx);
	at_context_stop(&ohci->at_response_ctx);
	reg_write(ohci, OHCI1394_IntEventClear, OHCI1394_busReset);

	/* This next bit is unrelated to the AT context stuff but we
	 * have to do it under the spinlock also.  If a new config rom
	 * was set up before this reset, the old one is now no longer
	 * in use and we can free it. Update the config rom pointers
	 * to point to the current config rom and clear the
	 * next_config_rom pointer so a new udpate can take place. */

	if (ohci->next_config_rom != NULL) {
		dma_free_coherent(ohci->card.device, CONFIG_ROM_SIZE,
				  ohci->config_rom, ohci->config_rom_bus);
		ohci->config_rom      = ohci->next_config_rom;
		ohci->config_rom_bus  = ohci->next_config_rom_bus;
		ohci->next_config_rom = NULL;

		/* Restore config_rom image and manually update
		 * config_rom registers.  Writing the header quadlet
		 * will indicate that the config rom is ready, so we
		 * do that last. */
		reg_write(ohci, OHCI1394_BusOptions,
			  be32_to_cpu(ohci->config_rom[2]));
		ohci->config_rom[0] = cpu_to_be32(ohci->next_header);
		reg_write(ohci, OHCI1394_ConfigROMhdr, ohci->next_header);
	}

	spin_unlock_irqrestore(&ohci->lock, flags);

	fw_core_handle_bus_reset(&ohci->card, ohci->node_id, generation,
				 self_id_count, ohci->self_id_buffer);
}

static irqreturn_t irq_handler(int irq, void *data)
{
	struct fw_ohci *ohci = data;
	u32 event, iso_event;
	int i;

	event = reg_read(ohci, OHCI1394_IntEventClear);

	if (!event)
		return IRQ_NONE;

	reg_write(ohci, OHCI1394_IntEventClear, event);

	if (event & OHCI1394_selfIDComplete)
		tasklet_schedule(&ohci->bus_reset_tasklet);

	if (event & OHCI1394_RQPkt)
		tasklet_schedule(&ohci->ar_request_ctx.tasklet);

	if (event & OHCI1394_RSPkt)
		tasklet_schedule(&ohci->ar_response_ctx.tasklet);

	if (event & OHCI1394_reqTxComplete)
		tasklet_schedule(&ohci->at_request_ctx.tasklet);

	if (event & OHCI1394_respTxComplete)
		tasklet_schedule(&ohci->at_response_ctx.tasklet);

	iso_event = reg_read(ohci, OHCI1394_IsoRecvIntEventClear);
	reg_write(ohci, OHCI1394_IsoRecvIntEventClear, iso_event);

	while (iso_event) {
		i = ffs(iso_event) - 1;
		tasklet_schedule(&ohci->ir_context_list[i].context.tasklet);
		iso_event &= ~(1 << i);
	}

	iso_event = reg_read(ohci, OHCI1394_IsoXmitIntEventClear);
	reg_write(ohci, OHCI1394_IsoXmitIntEventClear, iso_event);

	while (iso_event) {
		i = ffs(iso_event) - 1;
		tasklet_schedule(&ohci->it_context_list[i].context.tasklet);
		iso_event &= ~(1 << i);
	}

	return IRQ_HANDLED;
}

static int ohci_enable(struct fw_card *card, u32 *config_rom, size_t length)
{
	struct fw_ohci *ohci = fw_ohci(card);
	struct pci_dev *dev = to_pci_dev(card->device);

	/* When the link is not yet enabled, the atomic config rom
	 * update mechanism described below in ohci_set_config_rom()
	 * is not active.  We have to update ConfigRomHeader and
	 * BusOptions manually, and the write to ConfigROMmap takes
	 * effect immediately.  We tie this to the enabling of the
	 * link, so we have a valid config rom before enabling - the
	 * OHCI requires that ConfigROMhdr and BusOptions have valid
	 * values before enabling.
	 *
	 * However, when the ConfigROMmap is written, some controllers
	 * always read back quadlets 0 and 2 from the config rom to
	 * the ConfigRomHeader and BusOptions registers on bus reset.
	 * They shouldn't do that in this initial case where the link
	 * isn't enabled.  This means we have to use the same
	 * workaround here, setting the bus header to 0 and then write
	 * the right values in the bus reset tasklet.
	 */

	ohci->next_config_rom =
		dma_alloc_coherent(ohci->card.device, CONFIG_ROM_SIZE,
				   &ohci->next_config_rom_bus, GFP_KERNEL);
	if (ohci->next_config_rom == NULL)
		return -ENOMEM;

	memset(ohci->next_config_rom, 0, CONFIG_ROM_SIZE);
	fw_memcpy_to_be32(ohci->next_config_rom, config_rom, length * 4);

	ohci->next_header = config_rom[0];
	ohci->next_config_rom[0] = 0;
	reg_write(ohci, OHCI1394_ConfigROMhdr, 0);
	reg_write(ohci, OHCI1394_BusOptions, config_rom[2]);
	reg_write(ohci, OHCI1394_ConfigROMmap, ohci->next_config_rom_bus);

	reg_write(ohci, OHCI1394_AsReqFilterHiSet, 0x80000000);

	if (request_irq(dev->irq, irq_handler,
			SA_SHIRQ, ohci_driver_name, ohci)) {
		fw_error("Failed to allocate shared interrupt %d.\n",
			 dev->irq);
		dma_free_coherent(ohci->card.device, CONFIG_ROM_SIZE,
				  ohci->config_rom, ohci->config_rom_bus);
		return -EIO;
	}

	reg_write(ohci, OHCI1394_HCControlSet,
		  OHCI1394_HCControl_linkEnable |
		  OHCI1394_HCControl_BIBimageValid);
	flush_writes(ohci);

	/* We are ready to go, initiate bus reset to finish the
	 * initialization. */

	fw_core_initiate_bus_reset(&ohci->card, 1);

	return 0;
}

static int
ohci_set_config_rom(struct fw_card *card, u32 *config_rom, size_t length)
{
	struct fw_ohci *ohci;
	unsigned long flags;
	int retval = 0;
	__be32 *next_config_rom;
	dma_addr_t next_config_rom_bus;

	ohci = fw_ohci(card);

	/* When the OHCI controller is enabled, the config rom update
	 * mechanism is a bit tricky, but easy enough to use.  See
	 * section 5.5.6 in the OHCI specification.
	 *
	 * The OHCI controller caches the new config rom address in a
	 * shadow register (ConfigROMmapNext) and needs a bus reset
	 * for the changes to take place.  When the bus reset is
	 * detected, the controller loads the new values for the
	 * ConfigRomHeader and BusOptions registers from the specified
	 * config rom and loads ConfigROMmap from the ConfigROMmapNext
	 * shadow register. All automatically and atomically.
	 *
	 * Now, there's a twist to this story.  The automatic load of
	 * ConfigRomHeader and BusOptions doesn't honor the
	 * noByteSwapData bit, so with a be32 config rom, the
	 * controller will load be32 values in to these registers
	 * during the atomic update, even on litte endian
	 * architectures.  The workaround we use is to put a 0 in the
	 * header quadlet; 0 is endian agnostic and means that the
	 * config rom isn't ready yet.  In the bus reset tasklet we
	 * then set up the real values for the two registers.
	 *
	 * We use ohci->lock to avoid racing with the code that sets
	 * ohci->next_config_rom to NULL (see bus_reset_tasklet).
	 */

	next_config_rom =
		dma_alloc_coherent(ohci->card.device, CONFIG_ROM_SIZE,
				   &next_config_rom_bus, GFP_KERNEL);
	if (next_config_rom == NULL)
		return -ENOMEM;

	spin_lock_irqsave(&ohci->lock, flags);

	if (ohci->next_config_rom == NULL) {
		ohci->next_config_rom = next_config_rom;
		ohci->next_config_rom_bus = next_config_rom_bus;

		memset(ohci->next_config_rom, 0, CONFIG_ROM_SIZE);
		fw_memcpy_to_be32(ohci->next_config_rom, config_rom,
				  length * 4);

		ohci->next_header = config_rom[0];
		ohci->next_config_rom[0] = 0;

		reg_write(ohci, OHCI1394_ConfigROMmap,
			  ohci->next_config_rom_bus);
	} else {
		dma_free_coherent(ohci->card.device, CONFIG_ROM_SIZE,
				  next_config_rom, next_config_rom_bus);
		retval = -EBUSY;
	}

	spin_unlock_irqrestore(&ohci->lock, flags);

	/* Now initiate a bus reset to have the changes take
	 * effect. We clean up the old config rom memory and DMA
	 * mappings in the bus reset tasklet, since the OHCI
	 * controller could need to access it before the bus reset
	 * takes effect. */
	if (retval == 0)
		fw_core_initiate_bus_reset(&ohci->card, 1);

	return retval;
}

static void ohci_send_request(struct fw_card *card, struct fw_packet *packet)
{
	struct fw_ohci *ohci = fw_ohci(card);

	at_context_transmit(&ohci->at_request_ctx, packet);
}

static void ohci_send_response(struct fw_card *card, struct fw_packet *packet)
{
	struct fw_ohci *ohci = fw_ohci(card);

	at_context_transmit(&ohci->at_response_ctx, packet);
}

static int ohci_cancel_packet(struct fw_card *card, struct fw_packet *packet)
{
	struct fw_ohci *ohci = fw_ohci(card);
	LIST_HEAD(list);
	unsigned long flags;

	spin_lock_irqsave(&ohci->lock, flags);

	if (packet->ack == 0) {
		fw_notify("cancelling packet %p (header[0]=%08x)\n",
			  packet, packet->header[0]);

		complete_transmission(packet, RCODE_CANCELLED, &list);
	}

	spin_unlock_irqrestore(&ohci->lock, flags);

	do_packet_callbacks(ohci, &list);

	/* Return success if we actually cancelled something. */
	return list_empty(&list) ? -ENOENT : 0;
}

static int
ohci_enable_phys_dma(struct fw_card *card, int node_id, int generation)
{
	struct fw_ohci *ohci = fw_ohci(card);
	unsigned long flags;
	int n, retval = 0;

	/* FIXME:  Make sure this bitmask is cleared when we clear the busReset
	 * interrupt bit.  Clear physReqResourceAllBuses on bus reset. */

	spin_lock_irqsave(&ohci->lock, flags);

	if (ohci->generation != generation) {
		retval = -ESTALE;
		goto out;
	}

	/* NOTE, if the node ID contains a non-local bus ID, physical DMA is
	 * enabled for _all_ nodes on remote buses. */

	n = (node_id & 0xffc0) == LOCAL_BUS ? node_id & 0x3f : 63;
	if (n < 32)
		reg_write(ohci, OHCI1394_PhyReqFilterLoSet, 1 << n);
	else
		reg_write(ohci, OHCI1394_PhyReqFilterHiSet, 1 << (n - 32));

	flush_writes(ohci);
 out:
	spin_unlock_irqrestore(&ohci->lock, flags);
	return retval;
}

static int handle_ir_packet(struct context *context,
			    struct descriptor *d,
			    struct descriptor *last)
{
	struct iso_context *ctx =
		container_of(context, struct iso_context, context);
	struct db_descriptor *db = (struct db_descriptor *) d;
	size_t header_length;
 
	if (db->first_res_count > 0 && db->second_res_count > 0)
		/* This descriptor isn't done yet, stop iteration. */
		return 0;

	header_length = db->first_req_count - db->first_res_count;
	if (ctx->header_length + header_length <= PAGE_SIZE)
		memcpy(ctx->header + ctx->header_length, db + 1, header_length);
	ctx->header_length += header_length;

	if (le16_to_cpu(db->control) & descriptor_irq_always) {
		ctx->base.callback(&ctx->base, 0,
				   ctx->header_length, ctx->header,
				   ctx->base.callback_data);
		ctx->header_length = 0;
	}

	return 1;
}

#define ISO_BUFFER_SIZE (64 * 1024)

static int handle_it_packet(struct context *context,
			    struct descriptor *d,
			    struct descriptor *last)
{
	struct iso_context *ctx =
		container_of(context, struct iso_context, context);
 
	if (last->transfer_status == 0)
		/* This descriptor isn't done yet, stop iteration. */
		return 0;

	if (le16_to_cpu(last->control) & descriptor_irq_always)
		ctx->base.callback(&ctx->base, le16_to_cpu(last->res_count),
				   0, NULL, ctx->base.callback_data);

	return 1;
}

static struct fw_iso_context *
ohci_allocate_iso_context(struct fw_card *card, int type)
{
	struct fw_ohci *ohci = fw_ohci(card);
	struct iso_context *ctx, *list;
	descriptor_callback_t callback;
	u32 *mask, regs;
	unsigned long flags;
	int index, retval = -ENOMEM;

	if (type == FW_ISO_CONTEXT_TRANSMIT) {
		mask = &ohci->it_context_mask;
		list = ohci->it_context_list;
		callback = handle_it_packet;
	} else {
 		mask = &ohci->ir_context_mask;
 		list = ohci->ir_context_list;
		callback = handle_ir_packet;
	}

	spin_lock_irqsave(&ohci->lock, flags);
	index = ffs(*mask) - 1;
	if (index >= 0)
		*mask &= ~(1 << index);
	spin_unlock_irqrestore(&ohci->lock, flags);

	if (index < 0)
		return ERR_PTR(-EBUSY);

 	if (type == FW_ISO_CONTEXT_TRANSMIT)
 		regs = OHCI1394_IsoXmitContextBase(index);
 	else
 		regs = OHCI1394_IsoRcvContextBase(index);
 
	ctx = &list[index];
	memset(ctx, 0, sizeof *ctx);
	ctx->header_length = 0;
	ctx->header = (void *) __get_free_page(GFP_KERNEL);
	if (ctx->header == NULL)
		goto out;

	retval = context_init(&ctx->context, ohci, ISO_BUFFER_SIZE,
			      regs, callback);
	if (retval < 0)
		goto out_with_header;

	return &ctx->base;

 out_with_header:
	free_page((unsigned long)ctx->header);
 out:
	spin_lock_irqsave(&ohci->lock, flags);
	*mask |= 1 << index;
	spin_unlock_irqrestore(&ohci->lock, flags);

	return ERR_PTR(retval);
}

static int ohci_start_iso(struct fw_iso_context *base, s32 cycle)
{
 	struct iso_context *ctx = container_of(base, struct iso_context, base);
	struct fw_ohci *ohci = ctx->context.ohci;
	u32 cycle_match = 0;
	int index;

	if (ctx->base.type == FW_ISO_CONTEXT_TRANSMIT) {
		index = ctx - ohci->it_context_list;
		if (cycle > 0)
			cycle_match = IT_CONTEXT_CYCLE_MATCH_ENABLE |
				(cycle & 0x7fff) << 16;
		
		reg_write(ohci, OHCI1394_IsoXmitIntEventClear, 1 << index);
		reg_write(ohci, OHCI1394_IsoXmitIntMaskSet, 1 << index);
		context_run(&ctx->context, cycle_match);
	} else {
		index = ctx - ohci->ir_context_list;

		reg_write(ohci, OHCI1394_IsoRecvIntEventClear, 1 << index);
		reg_write(ohci, OHCI1394_IsoRecvIntMaskSet, 1 << index);
		reg_write(ohci, context_match(ctx->context.regs),
			  0xf0000000 | ctx->base.channel);
		context_run(&ctx->context, IR_CONTEXT_DUAL_BUFFER_MODE);
	}

	return 0;
}

static int ohci_stop_iso(struct fw_iso_context *base)
{
	struct fw_ohci *ohci = fw_ohci(base->card);
 	struct iso_context *ctx = container_of(base, struct iso_context, base);
	int index;

	if (ctx->base.type == FW_ISO_CONTEXT_TRANSMIT) {
		index = ctx - ohci->it_context_list;
		reg_write(ohci, OHCI1394_IsoXmitIntMaskClear, 1 << index);
	} else {
		index = ctx - ohci->ir_context_list;
		reg_write(ohci, OHCI1394_IsoRecvIntMaskClear, 1 << index);
	}
	flush_writes(ohci);
	context_stop(&ctx->context);

	return 0;
}

static void ohci_free_iso_context(struct fw_iso_context *base)
{
	struct fw_ohci *ohci = fw_ohci(base->card);
 	struct iso_context *ctx = container_of(base, struct iso_context, base);
	unsigned long flags;
	int index;

	ohci_stop_iso(base);
	context_release(&ctx->context);
	free_page((unsigned long)ctx->header);

	spin_lock_irqsave(&ohci->lock, flags);

	if (ctx->base.type == FW_ISO_CONTEXT_TRANSMIT) {
		index = ctx - ohci->it_context_list;
		ohci->it_context_mask |= 1 << index;
	} else {
		index = ctx - ohci->ir_context_list;
		ohci->ir_context_mask |= 1 << index;
	}

	spin_unlock_irqrestore(&ohci->lock, flags);
}

static int
ohci_queue_iso_transmit(struct fw_iso_context *base,
			struct fw_iso_packet *packet,
			struct fw_iso_buffer *buffer,
			unsigned long payload)
{
 	struct iso_context *ctx = container_of(base, struct iso_context, base);
	struct descriptor *d, *last, *pd;
	struct fw_iso_packet *p;
	__le32 *header;
	dma_addr_t d_bus, page_bus;
	u32 z, header_z, payload_z, irq;
	u32 payload_index, payload_end_index, next_page_index;
	int page, end_page, i, length, offset;

	/* FIXME: Cycle lost behavior should be configurable: lose
	 * packet, retransmit or terminate.. */

	p = packet;
	payload_index = payload;

	if (p->skip)
		z = 1;
	else
		z = 2;
	if (p->header_length > 0)
		z++;

	/* Determine the first page the payload isn't contained in. */
	end_page = PAGE_ALIGN(payload_index + p->payload_length) >> PAGE_SHIFT;
	if (p->payload_length > 0)
		payload_z = end_page - (payload_index >> PAGE_SHIFT);
	else
		payload_z = 0;

	z += payload_z;

	/* Get header size in number of descriptors. */
	header_z = DIV_ROUND_UP(p->header_length, sizeof *d);

	d = context_get_descriptors(&ctx->context, z + header_z, &d_bus);
	if (d == NULL)
		return -ENOMEM;

	if (!p->skip) {
		d[0].control   = cpu_to_le16(descriptor_key_immediate);
		d[0].req_count = cpu_to_le16(8);

		header = (__le32 *) &d[1];
		header[0] = cpu_to_le32(it_header_sy(p->sy) |
					it_header_tag(p->tag) |
					it_header_tcode(TCODE_STREAM_DATA) |
					it_header_channel(ctx->base.channel) |
					it_header_speed(ctx->base.speed));
		header[1] =
			cpu_to_le32(it_header_data_length(p->header_length +
							  p->payload_length));
	}

	if (p->header_length > 0) {
		d[2].req_count    = cpu_to_le16(p->header_length);
		d[2].data_address = cpu_to_le32(d_bus + z * sizeof *d);
		memcpy(&d[z], p->header, p->header_length);
	}

	pd = d + z - payload_z;
	payload_end_index = payload_index + p->payload_length;
	for (i = 0; i < payload_z; i++) {
		page               = payload_index >> PAGE_SHIFT;
		offset             = payload_index & ~PAGE_MASK;
		next_page_index    = (page + 1) << PAGE_SHIFT;
		length             =
			min(next_page_index, payload_end_index) - payload_index;
		pd[i].req_count    = cpu_to_le16(length);

		page_bus = page_private(buffer->pages[page]);
		pd[i].data_address = cpu_to_le32(page_bus + offset);

		payload_index += length;
	}

	if (p->interrupt)
		irq = descriptor_irq_always;
	else
		irq = descriptor_no_irq;

	last = z == 2 ? d : d + z - 1;
	last->control |= cpu_to_le16(descriptor_output_last |
				     descriptor_status |
				     descriptor_branch_always |
				     irq);

	context_append(&ctx->context, d, z, header_z);

	return 0;
}

static int
ohci_queue_iso_receive(struct fw_iso_context *base,
		       struct fw_iso_packet *packet,
		       struct fw_iso_buffer *buffer,
		       unsigned long payload)
{
	struct iso_context *ctx = container_of(base, struct iso_context, base);
	struct db_descriptor *db = NULL;
	struct descriptor *d;
	struct fw_iso_packet *p;
	dma_addr_t d_bus, page_bus;
	u32 z, header_z, length, rest;
	int page, offset;
 
	/* FIXME: Cycle lost behavior should be configurable: lose
	 * packet, retransmit or terminate.. */

	p = packet;
	z = 2;

	/* Get header size in number of descriptors. */
	header_z = DIV_ROUND_UP(p->header_length, sizeof *d);
	page     = payload >> PAGE_SHIFT;
	offset   = payload & ~PAGE_MASK;
	rest     = p->payload_length;

	/* FIXME: OHCI 1.0 doesn't support dual buffer receive */
	/* FIXME: handle descriptor_wait */
	/* FIXME: make packet-per-buffer/dual-buffer a context option */
	while (rest > 0) {
		d = context_get_descriptors(&ctx->context,
					    z + header_z, &d_bus);
		if (d == NULL)
			return -ENOMEM;

		db = (struct db_descriptor *) d;
		db->control = cpu_to_le16(descriptor_status |
					  descriptor_branch_always);
		db->first_size = cpu_to_le16(ctx->base.header_size);
		db->first_req_count = cpu_to_le16(p->header_length);
		db->first_res_count = db->first_req_count;
		db->first_buffer = cpu_to_le32(d_bus + sizeof *db);
		
		if (offset + rest < PAGE_SIZE)
			length = rest;
		else
			length = PAGE_SIZE - offset;

		db->second_req_count = cpu_to_le16(length);
		db->second_res_count = db->second_req_count;
		page_bus = page_private(buffer->pages[page]);
		db->second_buffer = cpu_to_le32(page_bus + offset);

		context_append(&ctx->context, d, z, header_z);
		offset = (offset + length) & ~PAGE_MASK;
		rest -= length;
		page++;
	}

	if (p->interrupt)
		db->control |= cpu_to_le16(descriptor_irq_always);
 
 	return 0;
 }
 
static int
ohci_queue_iso(struct fw_iso_context *base,
	       struct fw_iso_packet *packet,
	       struct fw_iso_buffer *buffer,
	       unsigned long payload)
{
	if (base->type == FW_ISO_CONTEXT_TRANSMIT)
		return ohci_queue_iso_transmit(base, packet, buffer, payload);
	else
		return ohci_queue_iso_receive(base, packet, buffer, payload);
}

static const struct fw_card_driver ohci_driver = {
	.name			= ohci_driver_name,
	.enable			= ohci_enable,
	.update_phy_reg		= ohci_update_phy_reg,
	.set_config_rom		= ohci_set_config_rom,
	.send_request		= ohci_send_request,
	.send_response		= ohci_send_response,
	.cancel_packet		= ohci_cancel_packet,
	.enable_phys_dma	= ohci_enable_phys_dma,

	.allocate_iso_context	= ohci_allocate_iso_context,
	.free_iso_context	= ohci_free_iso_context,
	.queue_iso		= ohci_queue_iso,
	.start_iso		= ohci_start_iso,
	.stop_iso		= ohci_stop_iso,
};

static int software_reset(struct fw_ohci *ohci)
{
	int i;

	reg_write(ohci, OHCI1394_HCControlSet, OHCI1394_HCControl_softReset);

	for (i = 0; i < OHCI_LOOP_COUNT; i++) {
		if ((reg_read(ohci, OHCI1394_HCControlSet) &
		     OHCI1394_HCControl_softReset) == 0)
			return 0;
		msleep(1);
	}

	return -EBUSY;
}

/* ---------- pci subsystem interface ---------- */

enum {
	CLEANUP_SELF_ID,
	CLEANUP_REGISTERS,
	CLEANUP_IOMEM,
	CLEANUP_DISABLE,
	CLEANUP_PUT_CARD,
};

static int cleanup(struct fw_ohci *ohci, int stage, int code)
{
	struct pci_dev *dev = to_pci_dev(ohci->card.device);

	switch (stage) {
	case CLEANUP_SELF_ID:
		dma_free_coherent(ohci->card.device, SELF_ID_BUF_SIZE,
				  ohci->self_id_cpu, ohci->self_id_bus);
	case CLEANUP_REGISTERS:
		kfree(ohci->it_context_list);
		kfree(ohci->ir_context_list);
		pci_iounmap(dev, ohci->registers);
	case CLEANUP_IOMEM:
		pci_release_region(dev, 0);
	case CLEANUP_DISABLE:
		pci_disable_device(dev);
	case CLEANUP_PUT_CARD:
		fw_card_put(&ohci->card);
	}

	return code;
}

static int __devinit
pci_probe(struct pci_dev *dev, const struct pci_device_id *ent)
{
	struct fw_ohci *ohci;
	u32 bus_options, max_receive, link_speed, version;
	u64 guid;
	int error_code;
	size_t size;

	ohci = kzalloc(sizeof *ohci, GFP_KERNEL);
	if (ohci == NULL) {
		fw_error("Could not malloc fw_ohci data.\n");
		return -ENOMEM;
	}

	fw_card_initialize(&ohci->card, &ohci_driver, &dev->dev);

	if (pci_enable_device(dev)) {
		fw_error("Failed to enable OHCI hardware.\n");
		return cleanup(ohci, CLEANUP_PUT_CARD, -ENODEV);
	}

	pci_set_master(dev);
	pci_write_config_dword(dev, OHCI1394_PCI_HCI_Control, 0);
	pci_set_drvdata(dev, ohci);

	spin_lock_init(&ohci->lock);

	tasklet_init(&ohci->bus_reset_tasklet,
		     bus_reset_tasklet, (unsigned long)ohci);

	if (pci_request_region(dev, 0, ohci_driver_name)) {
		fw_error("MMIO resource unavailable\n");
		return cleanup(ohci, CLEANUP_DISABLE, -EBUSY);
	}

	ohci->registers = pci_iomap(dev, 0, OHCI1394_REGISTER_SIZE);
	if (ohci->registers == NULL) {
		fw_error("Failed to remap registers\n");
		return cleanup(ohci, CLEANUP_IOMEM, -ENXIO);
	}

	if (software_reset(ohci)) {
		fw_error("Failed to reset ohci card.\n");
		return cleanup(ohci, CLEANUP_REGISTERS, -EBUSY);
	}

	/* Now enable LPS, which we need in order to start accessing
	 * most of the registers.  In fact, on some cards (ALI M5251),
	 * accessing registers in the SClk domain without LPS enabled
	 * will lock up the machine.  Wait 50msec to make sure we have
	 * full link enabled.  */
	reg_write(ohci, OHCI1394_HCControlSet,
		  OHCI1394_HCControl_LPS |
		  OHCI1394_HCControl_postedWriteEnable);
	flush_writes(ohci);
	msleep(50);

	reg_write(ohci, OHCI1394_HCControlClear,
		  OHCI1394_HCControl_noByteSwapData);

	reg_write(ohci, OHCI1394_LinkControlSet,
		  OHCI1394_LinkControl_rcvSelfID |
		  OHCI1394_LinkControl_cycleTimerEnable |
		  OHCI1394_LinkControl_cycleMaster);

	ar_context_init(&ohci->ar_request_ctx, ohci,
			OHCI1394_AsReqRcvContextControlSet);

	ar_context_init(&ohci->ar_response_ctx, ohci,
			OHCI1394_AsRspRcvContextControlSet);

	at_context_init(&ohci->at_request_ctx, ohci,
			OHCI1394_AsReqTrContextControlSet);

	at_context_init(&ohci->at_response_ctx, ohci,
			OHCI1394_AsRspTrContextControlSet);

	reg_write(ohci, OHCI1394_ATRetries,
		  OHCI1394_MAX_AT_REQ_RETRIES |
		  (OHCI1394_MAX_AT_RESP_RETRIES << 4) |
		  (OHCI1394_MAX_PHYS_RESP_RETRIES << 8));

	reg_write(ohci, OHCI1394_IsoRecvIntMaskSet, ~0);
	ohci->it_context_mask = reg_read(ohci, OHCI1394_IsoRecvIntMaskSet);
	reg_write(ohci, OHCI1394_IsoRecvIntMaskClear, ~0);
	size = sizeof(struct iso_context) * hweight32(ohci->it_context_mask);
	ohci->it_context_list = kzalloc(size, GFP_KERNEL);

	reg_write(ohci, OHCI1394_IsoXmitIntMaskSet, ~0);
	ohci->ir_context_mask = reg_read(ohci, OHCI1394_IsoXmitIntMaskSet);
	reg_write(ohci, OHCI1394_IsoXmitIntMaskClear, ~0);
	size = sizeof(struct iso_context) * hweight32(ohci->ir_context_mask);
	ohci->ir_context_list = kzalloc(size, GFP_KERNEL);

	if (ohci->it_context_list == NULL || ohci->ir_context_list == NULL) {
		fw_error("Out of memory for it/ir contexts.\n");
		return cleanup(ohci, CLEANUP_REGISTERS, -ENOMEM);
	}

	/* self-id dma buffer allocation */
	ohci->self_id_cpu = dma_alloc_coherent(ohci->card.device,
					       SELF_ID_BUF_SIZE,
					       &ohci->self_id_bus,
					       GFP_KERNEL);
	if (ohci->self_id_cpu == NULL) {
		fw_error("Out of memory for self ID buffer.\n");
		return cleanup(ohci, CLEANUP_REGISTERS, -ENOMEM);
	}

	reg_write(ohci, OHCI1394_SelfIDBuffer, ohci->self_id_bus);
	reg_write(ohci, OHCI1394_PhyUpperBound, 0x00010000);
	reg_write(ohci, OHCI1394_IntEventClear, ~0);
	reg_write(ohci, OHCI1394_IntMaskClear, ~0);
	reg_write(ohci, OHCI1394_IntMaskSet,
		  OHCI1394_selfIDComplete |
		  OHCI1394_RQPkt | OHCI1394_RSPkt |
		  OHCI1394_reqTxComplete | OHCI1394_respTxComplete |
		  OHCI1394_isochRx | OHCI1394_isochTx |
		  OHCI1394_masterIntEnable);

	bus_options = reg_read(ohci, OHCI1394_BusOptions);
	max_receive = (bus_options >> 12) & 0xf;
	link_speed = bus_options & 0x7;
	guid = ((u64) reg_read(ohci, OHCI1394_GUIDHi) << 32) |
		reg_read(ohci, OHCI1394_GUIDLo);

	error_code = fw_card_add(&ohci->card, max_receive, link_speed, guid);
	if (error_code < 0)
		return cleanup(ohci, CLEANUP_SELF_ID, error_code);

	version = reg_read(ohci, OHCI1394_Version);
	fw_notify("Added fw-ohci device %s, OHCI version %x.%x\n",
		  dev->dev.bus_id, (version >> 16) & 0xff, version & 0xff);

	return 0;
}

static void pci_remove(struct pci_dev *dev)
{
	struct fw_ohci *ohci;

	ohci = pci_get_drvdata(dev);
	reg_write(ohci, OHCI1394_IntMaskClear, OHCI1394_masterIntEnable);
	fw_core_remove_card(&ohci->card);

	/* FIXME: Fail all pending packets here, now that the upper
	 * layers can't queue any more. */

	software_reset(ohci);
	free_irq(dev->irq, ohci);
	cleanup(ohci, CLEANUP_SELF_ID, 0);

	fw_notify("Removed fw-ohci device.\n");
}

static struct pci_device_id pci_table[] = {
	{ PCI_DEVICE_CLASS(PCI_CLASS_SERIAL_FIREWIRE_OHCI, ~0) },
	{ }
};

MODULE_DEVICE_TABLE(pci, pci_table);

static struct pci_driver fw_ohci_pci_driver = {
	.name		= ohci_driver_name,
	.id_table	= pci_table,
	.probe		= pci_probe,
	.remove		= pci_remove,
};

MODULE_AUTHOR("Kristian Hoegsberg <krh@bitplanet.net>");
MODULE_DESCRIPTION("Driver for PCI OHCI IEEE1394 controllers");
MODULE_LICENSE("GPL");

static int __init fw_ohci_init(void)
{
	return pci_register_driver(&fw_ohci_pci_driver);
}

static void __exit fw_ohci_cleanup(void)
{
	pci_unregister_driver(&fw_ohci_pci_driver);
}

module_init(fw_ohci_init);
module_exit(fw_ohci_cleanup);
