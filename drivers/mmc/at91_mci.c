/*
 *  linux/drivers/mmc/at91_mci.c - ATMEL AT91RM9200 MCI Driver
 *
 *  Copyright (C) 2005 Cougar Creek Computing Devices Ltd, All Rights Reserved
 *
 *  Copyright (C) 2006 Malcolm Noyes
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

/*
   This is the AT91RM9200 MCI driver that has been tested with both MMC cards
   and SD-cards.  Boards that support write protect are now supported.
   The CCAT91SBC001 board does not support SD cards.

   The three entry points are at91_mci_request, at91_mci_set_ios
   and at91_mci_get_ro.

   SET IOS
     This configures the device to put it into the correct mode and clock speed
     required.

   MCI REQUEST
     MCI request processes the commands sent in the mmc_request structure. This
     can consist of a processing command and a stop command in the case of
     multiple block transfers.

     There are three main types of request, commands, reads and writes.

     Commands are straight forward. The command is submitted to the controller and
     the request function returns. When the controller generates an interrupt to indicate
     the command is finished, the response to the command are read and the mmc_request_done
     function called to end the request.

     Reads and writes work in a similar manner to normal commands but involve the PDC (DMA)
     controller to manage the transfers.

     A read is done from the controller directly to the scatterlist passed in from the request.
     Due to a bug in the controller, when a read is completed, all the words are byte
     swapped in the scatterlist buffers.

     The sequence of read interrupts is: ENDRX, RXBUFF, CMDRDY

     A write is slightly different in that the bytes to write are read from the scatterlist
     into a dma memory buffer (this is in case the source buffer should be read only). The
     entire write buffer is then done from this single dma memory buffer.

     The sequence of write interrupts is: ENDTX, TXBUFE, NOTBUSY, CMDRDY

   GET RO
     Gets the status of the write protect pin, if available.
*/

#include <linux/config.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/ioport.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/blkdev.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/dma-mapping.h>
#include <linux/clk.h>

#include <linux/mmc/host.h>
#include <linux/mmc/protocol.h>

#include <asm/io.h>
#include <asm/irq.h>
#include <asm/mach/mmc.h>
#include <asm/arch/board.h>
#include <asm/arch/gpio.h>
#include <asm/arch/at91rm9200_mci.h>
#include <asm/arch/at91rm9200_pdc.h>

#define DRIVER_NAME "at91_mci"

#undef	SUPPORT_4WIRE

#ifdef CONFIG_MMC_DEBUG
#define DBG(fmt...)	\
	printk(fmt)
#else
#define DBG(fmt...)	do { } while (0)
#endif

static struct clk *mci_clk;

#define FL_SENT_COMMAND (1 << 0)
#define FL_SENT_STOP (1 << 1)



/*
 * Read from a MCI register.
 */
static inline unsigned long at91_mci_read(unsigned int reg)
{
	void __iomem *mci_base = (void __iomem *)AT91_VA_BASE_MCI;

	return __raw_readl(mci_base + reg);
}

/*
 * Write to a MCI register.
 */
static inline void at91_mci_write(unsigned int reg, unsigned long value)
{
        void __iomem *mci_base = (void __iomem *)AT91_VA_BASE_MCI;

        __raw_writel(value, mci_base + reg);
}

/*
 * Low level type for this driver
 */
struct at91mci_host
{
	struct mmc_host *mmc;
	struct mmc_command *cmd;
	struct mmc_request *request;

	struct at91_mmc_data *board;
	int present;

	/*
	 * Flag indicating when the command has been sent. This is used to
	 * work out whether or not to send the stop
	 */
	unsigned int flags;
	/* flag for current bus settings */
	u32 bus_mode;

	/* DMA buffer used for transmitting */
	unsigned int* buffer;
	dma_addr_t physical_address;
	unsigned int total_length;

	/* Latest in the scatterlist that has been enabled for transfer, but not freed */
	int in_use_index;

	/* Latest in the scatterlist that has been enabled for transfer */
	int transfer_index;
};

/*
 * Copy from sg to a dma block - used for transfers
 */
static inline void at91mci_sg_to_dma(struct at91mci_host *host, struct mmc_data *data)
{
	unsigned int len, i, size;
	unsigned *dmabuf = host->buffer;

	size = host->total_length;
	len = data->sg_len;

	/*
	 * Just loop through all entries. Size might not
	 * be the entire list though so make sure that
	 * we do not transfer too much.
	 */
	for (i = 0; i < len; i++) {
		struct scatterlist *sg;
		int amount;
		int index;
		unsigned int *sgbuffer;

		sg = &data->sg[i];

		sgbuffer = kmap_atomic(sg->page, KM_BIO_SRC_IRQ) + sg->offset;
		amount = min(size, sg->length);
		size -= amount;
		amount /= 4;

		for (index = 0; index < amount; index++)
			*dmabuf++ = swab32(sgbuffer[index]);

		kunmap_atomic(sgbuffer, KM_BIO_SRC_IRQ);

		if (size == 0)
			break;
	}

	/*
	 * Check that we didn't get a request to transfer
	 * more data than can fit into the SG list.
	 */
	BUG_ON(size != 0);
}

/*
 * Prepare a dma read
 */
static void at91mci_pre_dma_read(struct at91mci_host *host)
{
	int i;
	struct scatterlist *sg;
	struct mmc_command *cmd;
	struct mmc_data *data;

	DBG("pre dma read\n");

	cmd = host->cmd;
	if (!cmd) {
		DBG("no command\n");
		return;
	}

	data = cmd->data;
	if (!data) {
		DBG("no data\n");
		return;
	}

	for (i = 0; i < 2; i++) {
		/* nothing left to transfer */
		if (host->transfer_index >= data->sg_len) {
			DBG("Nothing left to transfer (index = %d)\n", host->transfer_index);
			break;
		}

		/* Check to see if this needs filling */
		if (i == 0) {
			if (at91_mci_read(AT91_PDC_RCR) != 0) {
				DBG("Transfer active in current\n");
				continue;
			}
		}
		else {
			if (at91_mci_read(AT91_PDC_RNCR) != 0) {
				DBG("Transfer active in next\n");
				continue;
			}
		}

		/* Setup the next transfer */
		DBG("Using transfer index %d\n", host->transfer_index);

		sg = &data->sg[host->transfer_index++];
		DBG("sg = %p\n", sg);

		sg->dma_address = dma_map_page(NULL, sg->page, sg->offset, sg->length, DMA_FROM_DEVICE);

		DBG("dma address = %08X, length = %d\n", sg->dma_address, sg->length);

		if (i == 0) {
			at91_mci_write(AT91_PDC_RPR, sg->dma_address);
			at91_mci_write(AT91_PDC_RCR, sg->length / 4);
		}
		else {
			at91_mci_write(AT91_PDC_RNPR, sg->dma_address);
			at91_mci_write(AT91_PDC_RNCR, sg->length / 4);
		}
	}

	DBG("pre dma read done\n");
}

/*
 * Handle after a dma read
 */
static void at91mci_post_dma_read(struct at91mci_host *host)
{
	struct mmc_command *cmd;
	struct mmc_data *data;

	DBG("post dma read\n");

	cmd = host->cmd;
	if (!cmd) {
		DBG("no command\n");
		return;
	}

	data = cmd->data;
	if (!data) {
		DBG("no data\n");
		return;
	}

	while (host->in_use_index < host->transfer_index) {
		unsigned int *buffer;
		int index;
		int len;

		struct scatterlist *sg;

		DBG("finishing index %d\n", host->in_use_index);

		sg = &data->sg[host->in_use_index++];

		DBG("Unmapping page %08X\n", sg->dma_address);

		dma_unmap_page(NULL, sg->dma_address, sg->length, DMA_FROM_DEVICE);

		/* Swap the contents of the buffer */
		buffer = kmap_atomic(sg->page, KM_BIO_SRC_IRQ) + sg->offset;
		DBG("buffer = %p, length = %d\n", buffer, sg->length);

		data->bytes_xfered += sg->length;

		len = sg->length / 4;

		for (index = 0; index < len; index++) {
			buffer[index] = swab32(buffer[index]);
		}
		kunmap_atomic(buffer, KM_BIO_SRC_IRQ);
		flush_dcache_page(sg->page);
	}

	/* Is there another transfer to trigger? */
	if (host->transfer_index < data->sg_len)
		at91mci_pre_dma_read(host);
	else {
		at91_mci_write(AT91_MCI_IER, AT91_MCI_RXBUFF);
		at91_mci_write(AT91_PDC_PTCR, AT91_PDC_RXTDIS | AT91_PDC_TXTDIS);
	}

	DBG("post dma read done\n");
}

/*
 * Handle transmitted data
 */
static void at91_mci_handle_transmitted(struct at91mci_host *host)
{
	struct mmc_command *cmd;
	struct mmc_data *data;

	DBG("Handling the transmit\n");

	/* Disable the transfer */
	at91_mci_write(AT91_PDC_PTCR, AT91_PDC_RXTDIS | AT91_PDC_TXTDIS);

	/* Now wait for cmd ready */
	at91_mci_write(AT91_MCI_IDR, AT91_MCI_TXBUFE);
	at91_mci_write(AT91_MCI_IER, AT91_MCI_NOTBUSY);

	cmd = host->cmd;
	if (!cmd) return;

	data = cmd->data;
	if (!data) return;

	data->bytes_xfered = host->total_length;
}

/*
 * Enable the controller
 */
static void at91_mci_enable(void)
{
	at91_mci_write(AT91_MCI_CR, AT91_MCI_MCIEN);
	at91_mci_write(AT91_MCI_IDR, 0xFFFFFFFF);
	at91_mci_write(AT91_MCI_DTOR, AT91_MCI_DTOMUL_1M | AT91_MCI_DTOCYC);
	at91_mci_write(AT91_MCI_MR, 0x834A);
	at91_mci_write(AT91_MCI_SDCR, 0x0);
}

/*
 * Disable the controller
 */
static void at91_mci_disable(void)
{
	at91_mci_write(AT91_MCI_CR, AT91_MCI_MCIDIS | AT91_MCI_SWRST);
}

/*
 * Send a command
 * return the interrupts to enable
 */
static unsigned int at91_mci_send_command(struct at91mci_host *host, struct mmc_command *cmd)
{
	unsigned int cmdr, mr;
	unsigned int block_length;
	struct mmc_data *data = cmd->data;

	unsigned int blocks;
	unsigned int ier = 0;

	host->cmd = cmd;

	/* Not sure if this is needed */
#if 0
	if ((at91_mci_read(AT91_MCI_SR) & AT91_MCI_RTOE) && (cmd->opcode == 1)) {
		DBG("Clearing timeout\n");
		at91_mci_write(AT91_MCI_ARGR, 0);
		at91_mci_write(AT91_MCI_CMDR, AT91_MCI_OPDCMD);
		while (!(at91_mci_read(AT91_MCI_SR) & AT91_MCI_CMDRDY)) {
			/* spin */
			DBG("Clearing: SR = %08X\n", at91_mci_read(AT91_MCI_SR));
		}
	}
#endif
	cmdr = cmd->opcode;

	if (mmc_resp_type(cmd) == MMC_RSP_NONE)
		cmdr |= AT91_MCI_RSPTYP_NONE;
	else {
		/* if a response is expected then allow maximum response latancy */
		cmdr |= AT91_MCI_MAXLAT;
		/* set 136 bit response for R2, 48 bit response otherwise */
		if (mmc_resp_type(cmd) == MMC_RSP_R2)
			cmdr |= AT91_MCI_RSPTYP_136;
		else
			cmdr |= AT91_MCI_RSPTYP_48;
	}

	if (data) {
		block_length = data->blksz;
		blocks = data->blocks;

		/* always set data start - also set direction flag for read */
		if (data->flags & MMC_DATA_READ)
			cmdr |= (AT91_MCI_TRDIR | AT91_MCI_TRCMD_START);
		else if (data->flags & MMC_DATA_WRITE)
			cmdr |= AT91_MCI_TRCMD_START;

		if (data->flags & MMC_DATA_STREAM)
			cmdr |= AT91_MCI_TRTYP_STREAM;
		if (data->flags & MMC_DATA_MULTI)
			cmdr |= AT91_MCI_TRTYP_MULTIPLE;
	}
	else {
		block_length = 0;
		blocks = 0;
	}

	if (cmd->opcode == MMC_STOP_TRANSMISSION)
		cmdr |= AT91_MCI_TRCMD_STOP;

	if (host->bus_mode == MMC_BUSMODE_OPENDRAIN)
		cmdr |= AT91_MCI_OPDCMD;

	/*
	 * Set the arguments and send the command
	 */
	DBG("Sending command %d as %08X, arg = %08X, blocks = %d, length = %d (MR = %08lX)\n",
		cmd->opcode, cmdr, cmd->arg, blocks, block_length, at91_mci_read(AT91_MCI_MR));

	if (!data) {
		at91_mci_write(AT91_PDC_PTCR, AT91_PDC_TXTDIS | AT91_PDC_RXTDIS);
		at91_mci_write(AT91_PDC_RPR, 0);
		at91_mci_write(AT91_PDC_RCR, 0);
		at91_mci_write(AT91_PDC_RNPR, 0);
		at91_mci_write(AT91_PDC_RNCR, 0);
		at91_mci_write(AT91_PDC_TPR, 0);
		at91_mci_write(AT91_PDC_TCR, 0);
		at91_mci_write(AT91_PDC_TNPR, 0);
		at91_mci_write(AT91_PDC_TNCR, 0);

		at91_mci_write(AT91_MCI_ARGR, cmd->arg);
		at91_mci_write(AT91_MCI_CMDR, cmdr);
		return AT91_MCI_CMDRDY;
	}

	mr = at91_mci_read(AT91_MCI_MR) & 0x7fff;	/* zero block length and PDC mode */
	at91_mci_write(AT91_MCI_MR, mr | (block_length << 16) | AT91_MCI_PDCMODE);

	/*
	 * Disable the PDC controller
	 */
	at91_mci_write(AT91_PDC_PTCR, AT91_PDC_RXTDIS | AT91_PDC_TXTDIS);

	if (cmdr & AT91_MCI_TRCMD_START) {
		data->bytes_xfered = 0;
		host->transfer_index = 0;
		host->in_use_index = 0;
		if (cmdr & AT91_MCI_TRDIR) {
			/*
			 * Handle a read
			 */
			host->buffer = NULL;
			host->total_length = 0;

			at91mci_pre_dma_read(host);
			ier = AT91_MCI_ENDRX /* | AT91_MCI_RXBUFF */;
		}
		else {
			/*
			 * Handle a write
			 */
			host->total_length = block_length * blocks;
			host->buffer = dma_alloc_coherent(NULL,
						  host->total_length,
						  &host->physical_address, GFP_KERNEL);

			at91mci_sg_to_dma(host, data);

			DBG("Transmitting %d bytes\n", host->total_length);

			at91_mci_write(AT91_PDC_TPR, host->physical_address);
			at91_mci_write(AT91_PDC_TCR, host->total_length / 4);
			ier = AT91_MCI_TXBUFE;
		}
	}

	/*
	 * Send the command and then enable the PDC - not the other way round as
	 * the data sheet says
	 */

	at91_mci_write(AT91_MCI_ARGR, cmd->arg);
	at91_mci_write(AT91_MCI_CMDR, cmdr);

	if (cmdr & AT91_MCI_TRCMD_START) {
		if (cmdr & AT91_MCI_TRDIR)
			at91_mci_write(AT91_PDC_PTCR, AT91_PDC_RXTEN);
		else
			at91_mci_write(AT91_PDC_PTCR, AT91_PDC_TXTEN);
	}
	return ier;
}

/*
 * Wait for a command to complete
 */
static void at91mci_process_command(struct at91mci_host *host, struct mmc_command *cmd)
{
	unsigned int ier;

	ier = at91_mci_send_command(host, cmd);

	DBG("setting ier to %08X\n", ier);

	/* Stop on errors or the required value */
	at91_mci_write(AT91_MCI_IER, 0xffff0000 | ier);
}

/*
 * Process the next step in the request
 */
static void at91mci_process_next(struct at91mci_host *host)
{
	if (!(host->flags & FL_SENT_COMMAND)) {
		host->flags |= FL_SENT_COMMAND;
		at91mci_process_command(host, host->request->cmd);
	}
	else if ((!(host->flags & FL_SENT_STOP)) && host->request->stop) {
		host->flags |= FL_SENT_STOP;
		at91mci_process_command(host, host->request->stop);
	}
	else
		mmc_request_done(host->mmc, host->request);
}

/*
 * Handle a command that has been completed
 */
static void at91mci_completed_command(struct at91mci_host *host)
{
	struct mmc_command *cmd = host->cmd;
	unsigned int status;

	at91_mci_write(AT91_MCI_IDR, 0xffffffff);

	cmd->resp[0] = at91_mci_read(AT91_MCI_RSPR(0));
	cmd->resp[1] = at91_mci_read(AT91_MCI_RSPR(1));
	cmd->resp[2] = at91_mci_read(AT91_MCI_RSPR(2));
	cmd->resp[3] = at91_mci_read(AT91_MCI_RSPR(3));

	if (host->buffer) {
		dma_free_coherent(NULL, host->total_length, host->buffer, host->physical_address);
		host->buffer = NULL;
	}

	status = at91_mci_read(AT91_MCI_SR);

	DBG("Status = %08X [%08X %08X %08X %08X]\n",
		 status, cmd->resp[0], cmd->resp[1], cmd->resp[2], cmd->resp[3]);

	if (status & (AT91_MCI_RINDE | AT91_MCI_RDIRE | AT91_MCI_RCRCE |
			AT91_MCI_RENDE | AT91_MCI_RTOE | AT91_MCI_DCRCE |
			AT91_MCI_DTOE | AT91_MCI_OVRE | AT91_MCI_UNRE)) {
		if ((status & AT91_MCI_RCRCE) &&
			((cmd->opcode == MMC_SEND_OP_COND) || (cmd->opcode == SD_APP_OP_COND))) {
			cmd->error = MMC_ERR_NONE;
		}
		else {
			if (status & (AT91_MCI_RTOE | AT91_MCI_DTOE))
				cmd->error = MMC_ERR_TIMEOUT;
			else if (status & (AT91_MCI_RCRCE | AT91_MCI_DCRCE))
				cmd->error = MMC_ERR_BADCRC;
			else if (status & (AT91_MCI_OVRE | AT91_MCI_UNRE))
				cmd->error = MMC_ERR_FIFO;
			else
				cmd->error = MMC_ERR_FAILED;

			DBG("Error detected and set to %d (cmd = %d, retries = %d)\n",
				 cmd->error, cmd->opcode, cmd->retries);
		}
	}
	else
		cmd->error = MMC_ERR_NONE;

	at91mci_process_next(host);
}

/*
 * Handle an MMC request
 */
static void at91_mci_request(struct mmc_host *mmc, struct mmc_request *mrq)
{
	struct at91mci_host *host = mmc_priv(mmc);
	host->request = mrq;
	host->flags = 0;

	at91mci_process_next(host);
}

/*
 * Set the IOS
 */
static void at91_mci_set_ios(struct mmc_host *mmc, struct mmc_ios *ios)
{
	int clkdiv;
	struct at91mci_host *host = mmc_priv(mmc);
	unsigned long at91_master_clock = clk_get_rate(mci_clk);

	if (host)
		host->bus_mode = ios->bus_mode;
	else
		printk("MMC: No host for bus_mode\n");

	if (ios->clock == 0) {
		/* Disable the MCI controller */
		at91_mci_write(AT91_MCI_CR, AT91_MCI_MCIDIS);
		clkdiv = 0;
	}
	else {
		/* Enable the MCI controller */
		at91_mci_write(AT91_MCI_CR, AT91_MCI_MCIEN);

		if ((at91_master_clock % (ios->clock * 2)) == 0)
			clkdiv = ((at91_master_clock / ios->clock) / 2) - 1;
		else
			clkdiv = (at91_master_clock / ios->clock) / 2;

		DBG("clkdiv = %d. mcck = %ld\n", clkdiv,
			at91_master_clock / (2 * (clkdiv + 1)));
	}
	if (ios->bus_width == MMC_BUS_WIDTH_4 && host->board->wire4) {
		DBG("MMC: Setting controller bus width to 4\n");
		at91_mci_write(AT91_MCI_SDCR, at91_mci_read(AT91_MCI_SDCR) | AT91_MCI_SDCBUS);
	}
	else {
		DBG("MMC: Setting controller bus width to 1\n");
		at91_mci_write(AT91_MCI_SDCR, at91_mci_read(AT91_MCI_SDCR) & ~AT91_MCI_SDCBUS);
	}

	/* Set the clock divider */
	at91_mci_write(AT91_MCI_MR, (at91_mci_read(AT91_MCI_MR) & ~AT91_MCI_CLKDIV) | clkdiv);

	/* maybe switch power to the card */
	if (host && host->board->vcc_pin) {
		switch (ios->power_mode) {
			case MMC_POWER_OFF:
				at91_set_gpio_output(host->board->vcc_pin, 0);
				break;
			case MMC_POWER_UP:
			case MMC_POWER_ON:
				at91_set_gpio_output(host->board->vcc_pin, 1);
				break;
		}
	}
}

/*
 * Handle an interrupt
 */
static irqreturn_t at91_mci_irq(int irq, void *devid, struct pt_regs *regs)
{
	struct at91mci_host *host = devid;
	int completed = 0;

	unsigned int int_status;

	if (host == NULL)
		return IRQ_HANDLED;

	int_status = at91_mci_read(AT91_MCI_SR);
	DBG("MCI irq: status = %08X, %08lX, %08lX\n", int_status, at91_mci_read(AT91_MCI_IMR),
		int_status & at91_mci_read(AT91_MCI_IMR));

	if ((int_status & at91_mci_read(AT91_MCI_IMR)) & 0xffff0000)
		completed = 1;

	int_status &= at91_mci_read(AT91_MCI_IMR);

	if (int_status & AT91_MCI_UNRE)
		DBG("MMC: Underrun error\n");
	if (int_status & AT91_MCI_OVRE)
		DBG("MMC: Overrun error\n");
	if (int_status & AT91_MCI_DTOE)
		DBG("MMC: Data timeout\n");
	if (int_status & AT91_MCI_DCRCE)
		DBG("MMC: CRC error in data\n");
	if (int_status & AT91_MCI_RTOE)
		DBG("MMC: Response timeout\n");
	if (int_status & AT91_MCI_RENDE)
		DBG("MMC: Response end bit error\n");
	if (int_status & AT91_MCI_RCRCE)
		DBG("MMC: Response CRC error\n");
	if (int_status & AT91_MCI_RDIRE)
		DBG("MMC: Response direction error\n");
	if (int_status & AT91_MCI_RINDE)
		DBG("MMC: Response index error\n");

	/* Only continue processing if no errors */
	if (!completed) {
		if (int_status & AT91_MCI_TXBUFE) {
			DBG("TX buffer empty\n");
			at91_mci_handle_transmitted(host);
		}

		if (int_status & AT91_MCI_RXBUFF) {
			DBG("RX buffer full\n");
			at91_mci_write(AT91_MCI_IER, AT91_MCI_CMDRDY);
		}

		if (int_status & AT91_MCI_ENDTX) {
			DBG("Transmit has ended\n");
		}

		if (int_status & AT91_MCI_ENDRX) {
			DBG("Receive has ended\n");
			at91mci_post_dma_read(host);
		}

		if (int_status & AT91_MCI_NOTBUSY) {
			DBG("Card is ready\n");
			at91_mci_write(AT91_MCI_IER, AT91_MCI_CMDRDY);
		}

		if (int_status & AT91_MCI_DTIP) {
			DBG("Data transfer in progress\n");
		}

		if (int_status & AT91_MCI_BLKE) {
			DBG("Block transfer has ended\n");
		}

		if (int_status & AT91_MCI_TXRDY) {
			DBG("Ready to transmit\n");
		}

		if (int_status & AT91_MCI_RXRDY) {
			DBG("Ready to receive\n");
		}

		if (int_status & AT91_MCI_CMDRDY) {
			DBG("Command ready\n");
			completed = 1;
		}
	}
	at91_mci_write(AT91_MCI_IDR, int_status);

	if (completed) {
		DBG("Completed command\n");
		at91_mci_write(AT91_MCI_IDR, 0xffffffff);
		at91mci_completed_command(host);
	}

	return IRQ_HANDLED;
}

static irqreturn_t at91_mmc_det_irq(int irq, void *_host, struct pt_regs *regs)
{
	struct at91mci_host *host = _host;
	int present = !at91_get_gpio_value(irq);

	/*
	 * we expect this irq on both insert and remove,
	 * and use a short delay to debounce.
	 */
	if (present != host->present) {
		host->present = present;
		DBG("%s: card %s\n", mmc_hostname(host->mmc),
			present ? "insert" : "remove");
		if (!present) {
			DBG("****** Resetting SD-card bus width ******\n");
			at91_mci_write(AT91_MCI_SDCR, 0);
		}
		mmc_detect_change(host->mmc, msecs_to_jiffies(100));
	}
	return IRQ_HANDLED;
}

int at91_mci_get_ro(struct mmc_host *mmc)
{
	int read_only = 0;
	struct at91mci_host *host = mmc_priv(mmc);

	if (host->board->wp_pin) {
		read_only = at91_get_gpio_value(host->board->wp_pin);
		printk(KERN_WARNING "%s: card is %s\n", mmc_hostname(mmc),
				(read_only ? "read-only" : "read-write") );
	}
	else {
		printk(KERN_WARNING "%s: host does not support reading read-only "
				"switch.  Assuming write-enable.\n", mmc_hostname(mmc));
	}
	return read_only;
}

static struct mmc_host_ops at91_mci_ops = {
	.request	= at91_mci_request,
	.set_ios	= at91_mci_set_ios,
	.get_ro		= at91_mci_get_ro,
};

/*
 * Probe for the device
 */
static int at91_mci_probe(struct platform_device *pdev)
{
	struct mmc_host *mmc;
	struct at91mci_host *host;
	int ret;

	DBG("Probe MCI devices\n");
	at91_mci_disable();
	at91_mci_enable();

	mmc = mmc_alloc_host(sizeof(struct at91mci_host), &pdev->dev);
	if (!mmc) {
		DBG("Failed to allocate mmc host\n");
		return -ENOMEM;
	}

	mmc->ops = &at91_mci_ops;
	mmc->f_min = 375000;
	mmc->f_max = 25000000;
	mmc->ocr_avail = MMC_VDD_32_33 | MMC_VDD_33_34;

	host = mmc_priv(mmc);
	host->mmc = mmc;
	host->buffer = NULL;
	host->bus_mode = 0;
	host->board = pdev->dev.platform_data;
	if (host->board->wire4) {
#ifdef SUPPORT_4WIRE
		mmc->caps |= MMC_CAP_4_BIT_DATA;
#else
		printk("MMC: 4 wire bus mode not supported by this driver - using 1 wire\n");
#endif
	}

	/*
	 * Get Clock
	 */
	mci_clk = clk_get(&pdev->dev, "mci_clk");
	if (!mci_clk) {
		printk(KERN_ERR "AT91 MMC: no clock defined.\n");
		return -ENODEV;
	}
	clk_enable(mci_clk);			/* Enable the peripheral clock */

	/*
	 * Allocate the MCI interrupt
	 */
	ret = request_irq(AT91_ID_MCI, at91_mci_irq, SA_SHIRQ, DRIVER_NAME, host);
	if (ret) {
		DBG("Failed to request MCI interrupt\n");
		return ret;
	}

	platform_set_drvdata(pdev, mmc);

	/*
	 * Add host to MMC layer
	 */
	if (host->board->det_pin)
		host->present = !at91_get_gpio_value(host->board->det_pin);
	else
		host->present = -1;

	mmc_add_host(mmc);

	/*
	 * monitor card insertion/removal if we can
	 */
	if (host->board->det_pin) {
		ret = request_irq(host->board->det_pin, at91_mmc_det_irq,
				SA_SAMPLE_RANDOM, DRIVER_NAME, host);
		if (ret)
			DBG("couldn't allocate MMC detect irq\n");
	}

	DBG(KERN_INFO "Added MCI driver\n");

	return 0;
}

/*
 * Remove a device
 */
static int at91_mci_remove(struct platform_device *pdev)
{
	struct mmc_host *mmc = platform_get_drvdata(pdev);
	struct at91mci_host *host;

	if (!mmc)
		return -1;

	host = mmc_priv(mmc);

	if (host->present != -1) {
		free_irq(host->board->det_pin, host);
		cancel_delayed_work(&host->mmc->detect);
	}

	mmc_remove_host(mmc);
	at91_mci_disable();
	free_irq(AT91_ID_MCI, host);
	mmc_free_host(mmc);

	clk_disable(mci_clk);				/* Disable the peripheral clock */
	clk_put(mci_clk);

	platform_set_drvdata(pdev, NULL);

	DBG("Removed\n");

	return 0;
}

#ifdef CONFIG_PM
static int at91_mci_suspend(struct platform_device *pdev, pm_message_t state)
{
	struct mmc_host *mmc = platform_get_drvdata(pdev);
	int ret = 0;

	if (mmc)
		ret = mmc_suspend_host(mmc, state);

	return ret;
}

static int at91_mci_resume(struct platform_device *pdev)
{
	struct mmc_host *mmc = platform_get_drvdata(pdev);
	int ret = 0;

	if (mmc)
		ret = mmc_resume_host(mmc);

	return ret;
}
#else
#define at91_mci_suspend	NULL
#define at91_mci_resume		NULL
#endif

static struct platform_driver at91_mci_driver = {
	.probe		= at91_mci_probe,
	.remove		= at91_mci_remove,
	.suspend	= at91_mci_suspend,
	.resume		= at91_mci_resume,
	.driver		= {
		.name	= DRIVER_NAME,
		.owner	= THIS_MODULE,
	},
};

static int __init at91_mci_init(void)
{
	return platform_driver_register(&at91_mci_driver);
}

static void __exit at91_mci_exit(void)
{
	platform_driver_unregister(&at91_mci_driver);
}

module_init(at91_mci_init);
module_exit(at91_mci_exit);

MODULE_DESCRIPTION("AT91 Multimedia Card Interface driver");
MODULE_AUTHOR("Nick Randell");
MODULE_LICENSE("GPL");
