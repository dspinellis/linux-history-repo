/*
 * Etrax specific IDE functions, like init and PIO-mode setting etc.
 * Almost the entire ide.c is used for the rest of the Etrax ATA driver.
 * Copyright (c) 2000-2005 Axis Communications AB
 *
 * Authors:    Bjorn Wesen        (initial version)
 *             Mikael Starvik     (crisv32 port)
 */

/* Regarding DMA:
 *
 * There are two forms of DMA - "DMA handshaking" between the interface and the drive,
 * and DMA between the memory and the interface. We can ALWAYS use the latter, since it's
 * something built-in in the Etrax. However only some drives support the DMA-mode handshaking
 * on the ATA-bus. The normal PC driver and Triton interface disables memory-if DMA when the
 * device can't do DMA handshaking for some stupid reason. We don't need to do that.
 */

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/timer.h>
#include <linux/mm.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/blkdev.h>
#include <linux/hdreg.h>
#include <linux/ide.h>
#include <linux/init.h>

#include <asm/io.h>
#include <asm/dma.h>

/* number of DMA descriptors */
#define MAX_DMA_DESCRS 64

/* number of times to retry busy-flags when reading/writing IDE-registers
 * this can't be too high because a hung harddisk might cause the watchdog
 * to trigger (sometimes INB and OUTB are called with irq's disabled)
 */

#define IDE_REGISTER_TIMEOUT 300

#define LOWDB(x)
#define D(x)

enum /* Transfer types */
{
	TYPE_PIO,
	TYPE_DMA,
	TYPE_UDMA
};

/* CRISv32 specifics */
#ifdef CONFIG_ETRAX_ARCH_V32
#include <asm/arch/hwregs/ata_defs.h>
#include <asm/arch/hwregs/dma_defs.h>
#include <asm/arch/hwregs/dma.h>
#include <asm/arch/pinmux.h>

#define ATA_UDMA2_CYC    2
#define ATA_UDMA2_DVS    3
#define ATA_UDMA1_CYC    2
#define ATA_UDMA1_DVS    4
#define ATA_UDMA0_CYC    4
#define ATA_UDMA0_DVS    6
#define ATA_DMA2_STROBE  7
#define ATA_DMA2_HOLD    1
#define ATA_DMA1_STROBE  8
#define ATA_DMA1_HOLD    3
#define ATA_DMA0_STROBE 25
#define ATA_DMA0_HOLD   19
#define ATA_PIO4_SETUP   3
#define ATA_PIO4_STROBE  7
#define ATA_PIO4_HOLD    1
#define ATA_PIO3_SETUP   3
#define ATA_PIO3_STROBE  9
#define ATA_PIO3_HOLD    3
#define ATA_PIO2_SETUP   3
#define ATA_PIO2_STROBE 13
#define ATA_PIO2_HOLD    5
#define ATA_PIO1_SETUP   5
#define ATA_PIO1_STROBE 23
#define ATA_PIO1_HOLD    9
#define ATA_PIO0_SETUP   9
#define ATA_PIO0_STROBE 39
#define ATA_PIO0_HOLD    9

int
cris_ide_ack_intr(ide_hwif_t* hwif)
{
	reg_ata_rw_ctrl2 ctrl2 = REG_TYPE_CONV(reg_ata_rw_ctrl2, int,
					       hwif->io_ports.data_addr);
	REG_WR_INT(ata, regi_ata, rw_ack_intr, 1 << ctrl2.sel);
	return 1;
}

static inline int
cris_ide_busy(void)
{
	reg_ata_rs_stat_data stat_data;
	stat_data = REG_RD(ata, regi_ata, rs_stat_data);
	return stat_data.busy;
}

static inline int
cris_ide_ready(void)
{
	return !cris_ide_busy();
}

static inline int
cris_ide_data_available(unsigned short* data)
{
	reg_ata_rs_stat_data stat_data;
	stat_data = REG_RD(ata, regi_ata, rs_stat_data);
	*data = stat_data.data;
	return stat_data.dav;
}

static void
cris_ide_write_command(unsigned long command)
{
	REG_WR_INT(ata, regi_ata, rw_ctrl2, command); /* write data to the drive's register */
}

static void
cris_ide_set_speed(int type, int setup, int strobe, int hold)
{
	reg_ata_rw_ctrl0 ctrl0 = REG_RD(ata, regi_ata, rw_ctrl0);
	reg_ata_rw_ctrl1 ctrl1 = REG_RD(ata, regi_ata, rw_ctrl1);

	if (type == TYPE_PIO) {
		ctrl0.pio_setup = setup;
		ctrl0.pio_strb = strobe;
		ctrl0.pio_hold = hold;
	} else if (type == TYPE_DMA) {
		ctrl0.dma_strb = strobe;
		ctrl0.dma_hold = hold;
	} else if (type == TYPE_UDMA) {
		ctrl1.udma_tcyc = setup;
		ctrl1.udma_tdvs = strobe;
	}
	REG_WR(ata, regi_ata, rw_ctrl0, ctrl0);
	REG_WR(ata, regi_ata, rw_ctrl1, ctrl1);
}

static unsigned long
cris_ide_base_address(int bus)
{
	reg_ata_rw_ctrl2 ctrl2 = {0};
	ctrl2.sel = bus;
	return REG_TYPE_CONV(int, reg_ata_rw_ctrl2, ctrl2);
}

static unsigned long
cris_ide_reg_addr(unsigned long addr, int cs0, int cs1)
{
	reg_ata_rw_ctrl2 ctrl2 = {0};
	ctrl2.addr = addr;
	ctrl2.cs1 = cs1;
	ctrl2.cs0 = cs0;
	return REG_TYPE_CONV(int, reg_ata_rw_ctrl2, ctrl2);
}

static __init void
cris_ide_reset(unsigned val)
{
	reg_ata_rw_ctrl0 ctrl0 = {0};
	ctrl0.rst = val ? regk_ata_active : regk_ata_inactive;
	REG_WR(ata, regi_ata, rw_ctrl0, ctrl0);
}

static __init void
cris_ide_init(void)
{
	reg_ata_rw_ctrl0 ctrl0 = {0};
	reg_ata_rw_intr_mask intr_mask = {0};

	ctrl0.en = regk_ata_yes;
	REG_WR(ata, regi_ata, rw_ctrl0, ctrl0);

	intr_mask.bus0 = regk_ata_yes;
	intr_mask.bus1 = regk_ata_yes;
	intr_mask.bus2 = regk_ata_yes;
	intr_mask.bus3 = regk_ata_yes;

	REG_WR(ata, regi_ata, rw_intr_mask, intr_mask);

	crisv32_request_dma(2, "ETRAX FS built-in ATA", DMA_VERBOSE_ON_ERROR, 0, dma_ata);
	crisv32_request_dma(3, "ETRAX FS built-in ATA", DMA_VERBOSE_ON_ERROR, 0, dma_ata);

	crisv32_pinmux_alloc_fixed(pinmux_ata);
	crisv32_pinmux_alloc_fixed(pinmux_ata0);
	crisv32_pinmux_alloc_fixed(pinmux_ata1);
	crisv32_pinmux_alloc_fixed(pinmux_ata2);
	crisv32_pinmux_alloc_fixed(pinmux_ata3);

	DMA_RESET(regi_dma2);
	DMA_ENABLE(regi_dma2);
	DMA_RESET(regi_dma3);
	DMA_ENABLE(regi_dma3);

	DMA_WR_CMD (regi_dma2, regk_dma_set_w_size2);
	DMA_WR_CMD (regi_dma3, regk_dma_set_w_size2);
}

static dma_descr_context mycontext __attribute__ ((__aligned__(32)));

#define cris_dma_descr_type dma_descr_data
#define cris_pio_read regk_ata_rd
#define cris_ultra_mask 0x7
#define MAX_DESCR_SIZE 0xffffffffUL

static unsigned long
cris_ide_get_reg(unsigned long reg)
{
	return (reg & 0x0e000000) >> 25;
}

static void
cris_ide_fill_descriptor(cris_dma_descr_type *d, void* buf, unsigned int len, int last)
{
	d->buf = (char*)virt_to_phys(buf);
	d->after = d->buf + len;
	d->eol = last;
}

static void
cris_ide_start_dma(ide_drive_t *drive, cris_dma_descr_type *d, int dir,int type,int len)
{
	ide_hwif_t *hwif = drive->hwif;

	reg_ata_rw_ctrl2 ctrl2 = REG_TYPE_CONV(reg_ata_rw_ctrl2, int,
					       hwif->io_ports.data_addr);
	reg_ata_rw_trf_cnt trf_cnt = {0};

	mycontext.saved_data = (dma_descr_data*)virt_to_phys(d);
	mycontext.saved_data_buf = d->buf;
	/* start the dma channel */
	DMA_START_CONTEXT(dir ? regi_dma3 : regi_dma2, virt_to_phys(&mycontext));

	/* initiate a multi word dma read using PIO handshaking */
	trf_cnt.cnt = len >> 1;
	/* Due to a "feature" the transfer count has to be one extra word for UDMA. */
	if (type == TYPE_UDMA)
		trf_cnt.cnt++;
	REG_WR(ata, regi_ata, rw_trf_cnt, trf_cnt);

	ctrl2.rw = dir ? regk_ata_rd : regk_ata_wr;
	ctrl2.trf_mode = regk_ata_dma;
	ctrl2.hsh = type == TYPE_PIO ? regk_ata_pio :
	            type == TYPE_DMA ? regk_ata_dma : regk_ata_udma;
	ctrl2.multi = regk_ata_yes;
	ctrl2.dma_size = regk_ata_word;
	REG_WR(ata, regi_ata, rw_ctrl2, ctrl2);
}

static void
cris_ide_wait_dma(int dir)
{
	reg_dma_rw_stat status;
	do
	{
		status = REG_RD(dma, dir ? regi_dma3 : regi_dma2, rw_stat);
	} while(status.list_state != regk_dma_data_at_eol);
}

static int cris_dma_test_irq(ide_drive_t *drive)
{
	ide_hwif_t *hwif = drive->hwif;
	int intr = REG_RD_INT(ata, regi_ata, r_intr);

	reg_ata_rw_ctrl2 ctrl2 = REG_TYPE_CONV(reg_ata_rw_ctrl2, int,
					       hwif->io_ports.data_addr);

	return intr & (1 << ctrl2.sel) ? 1 : 0;
}

static void cris_ide_initialize_dma(int dir)
{
}

#else
/* CRISv10 specifics */
#include <asm/arch/svinto.h>
#include <asm/arch/io_interface_mux.h>

/* PIO timing (in R_ATA_CONFIG)
 *
 *                        _____________________________
 * ADDRESS :     ________/
 *
 *                            _______________
 * DIOR    :     ____________/               \__________
 *
 *                               _______________
 * DATA    :     XXXXXXXXXXXXXXXX_______________XXXXXXXX
 *
 *
 * DIOR is unbuffered while address and data is buffered.
 * This creates two problems:
 * 1. The DIOR pulse is to early (because it is unbuffered)
 * 2. The rise time of DIOR is long
 *
 * There are at least three different plausible solutions
 * 1. Use a pad capable of larger currents in Etrax
 * 2. Use an external buffer
 * 3. Make the strobe pulse longer
 *
 * Some of the strobe timings below are modified to compensate
 * for this. This implies a slight performance decrease.
 *
 * THIS SHOULD NEVER BE CHANGED!
 *
 * TODO: Is this true for the latest LX boards still ?
 */

#define ATA_UDMA2_CYC    0 /* No UDMA supported, just to make it compile. */
#define ATA_UDMA2_DVS    0
#define ATA_UDMA1_CYC    0
#define ATA_UDMA1_DVS    0
#define ATA_UDMA0_CYC    0
#define ATA_UDMA0_DVS    0
#define ATA_DMA2_STROBE  4
#define ATA_DMA2_HOLD    0
#define ATA_DMA1_STROBE  4
#define ATA_DMA1_HOLD    1
#define ATA_DMA0_STROBE 12
#define ATA_DMA0_HOLD    9
#define ATA_PIO4_SETUP   1
#define ATA_PIO4_STROBE  5
#define ATA_PIO4_HOLD    0
#define ATA_PIO3_SETUP   1
#define ATA_PIO3_STROBE  5
#define ATA_PIO3_HOLD    1
#define ATA_PIO2_SETUP   1
#define ATA_PIO2_STROBE  6
#define ATA_PIO2_HOLD    2
#define ATA_PIO1_SETUP   2
#define ATA_PIO1_STROBE 11
#define ATA_PIO1_HOLD    4
#define ATA_PIO0_SETUP   4
#define ATA_PIO0_STROBE 19
#define ATA_PIO0_HOLD    4

int
cris_ide_ack_intr(ide_hwif_t* hwif)
{
	return 1;
}

static inline int
cris_ide_busy(void)
{
	return *R_ATA_STATUS_DATA & IO_MASK(R_ATA_STATUS_DATA, busy) ;
}

static inline int
cris_ide_ready(void)
{
	return *R_ATA_STATUS_DATA & IO_MASK(R_ATA_STATUS_DATA, tr_rdy) ;
}

static inline int
cris_ide_data_available(unsigned short* data)
{
	unsigned long status = *R_ATA_STATUS_DATA;
	*data = (unsigned short)status;
	return status & IO_MASK(R_ATA_STATUS_DATA, dav);
}

static void
cris_ide_write_command(unsigned long command)
{
	*R_ATA_CTRL_DATA = command;
}

static void
cris_ide_set_speed(int type, int setup, int strobe, int hold)
{
	static int pio_setup = ATA_PIO4_SETUP;
	static int pio_strobe = ATA_PIO4_STROBE;
	static int pio_hold = ATA_PIO4_HOLD;
	static int dma_strobe = ATA_DMA2_STROBE;
	static int dma_hold = ATA_DMA2_HOLD;

	if (type == TYPE_PIO) {
		pio_setup = setup;
		pio_strobe = strobe;
		pio_hold = hold;
	} else if (type == TYPE_DMA) {
		dma_strobe = strobe;
	  dma_hold = hold;
	}
	*R_ATA_CONFIG = ( IO_FIELD( R_ATA_CONFIG, enable, 1 ) |
	  IO_FIELD( R_ATA_CONFIG, dma_strobe, dma_strobe ) |
		IO_FIELD( R_ATA_CONFIG, dma_hold,   dma_hold ) |
		IO_FIELD( R_ATA_CONFIG, pio_setup,  pio_setup ) |
		IO_FIELD( R_ATA_CONFIG, pio_strobe, pio_strobe ) |
		IO_FIELD( R_ATA_CONFIG, pio_hold,   pio_hold ) );
}

static unsigned long
cris_ide_base_address(int bus)
{
	return IO_FIELD(R_ATA_CTRL_DATA, sel, bus);
}

static unsigned long
cris_ide_reg_addr(unsigned long addr, int cs0, int cs1)
{
	return IO_FIELD(R_ATA_CTRL_DATA, addr, addr) |
	       IO_FIELD(R_ATA_CTRL_DATA, cs0, cs0) |
	       IO_FIELD(R_ATA_CTRL_DATA, cs1, cs1);
}

static __init void
cris_ide_reset(unsigned val)
{
#ifdef CONFIG_ETRAX_IDE_G27_RESET
	REG_SHADOW_SET(R_PORT_G_DATA, port_g_data_shadow, 27, val);
#endif
#ifdef CONFIG_ETRAX_IDE_PB7_RESET
	port_pb_dir_shadow = port_pb_dir_shadow |
		IO_STATE(R_PORT_PB_DIR, dir7, output);
	*R_PORT_PB_DIR = port_pb_dir_shadow;
	REG_SHADOW_SET(R_PORT_PB_DATA, port_pb_data_shadow, 7, val);
#endif
}

static __init void
cris_ide_init(void)
{
	volatile unsigned int dummy;

	*R_ATA_CTRL_DATA = 0;
	*R_ATA_TRANSFER_CNT = 0;
	*R_ATA_CONFIG = 0;

	if (cris_request_io_interface(if_ata, "ETRAX100LX IDE")) {
		printk(KERN_CRIT "ide: Failed to get IO interface\n");
		return;
	} else if (cris_request_dma(ATA_TX_DMA_NBR,
		                          "ETRAX100LX IDE TX",
		                          DMA_VERBOSE_ON_ERROR,
		                          dma_ata)) {
		cris_free_io_interface(if_ata);
		printk(KERN_CRIT "ide: Failed to get Tx DMA channel\n");
		return;
	} else if (cris_request_dma(ATA_RX_DMA_NBR,
		                          "ETRAX100LX IDE RX",
		                          DMA_VERBOSE_ON_ERROR,
		                          dma_ata)) {
		cris_free_dma(ATA_TX_DMA_NBR, "ETRAX100LX IDE Tx");
		cris_free_io_interface(if_ata);
		printk(KERN_CRIT "ide: Failed to get Rx DMA channel\n");
		return;
	}

	/* make a dummy read to set the ata controller in a proper state */
	dummy = *R_ATA_STATUS_DATA;

	*R_ATA_CONFIG = ( IO_FIELD( R_ATA_CONFIG, enable, 1 ));
	*R_ATA_CTRL_DATA = ( IO_STATE( R_ATA_CTRL_DATA, rw,   read) |
	                     IO_FIELD( R_ATA_CTRL_DATA, addr, 1   ) );

	while(*R_ATA_STATUS_DATA & IO_MASK(R_ATA_STATUS_DATA, busy)); /* wait for busy flag*/

	*R_IRQ_MASK0_SET = ( IO_STATE( R_IRQ_MASK0_SET, ata_irq0, set ) |
	                     IO_STATE( R_IRQ_MASK0_SET, ata_irq1, set ) |
	                     IO_STATE( R_IRQ_MASK0_SET, ata_irq2, set ) |
	                     IO_STATE( R_IRQ_MASK0_SET, ata_irq3, set ) );

	/* reset the dma channels we will use */

	RESET_DMA(ATA_TX_DMA_NBR);
	RESET_DMA(ATA_RX_DMA_NBR);
	WAIT_DMA(ATA_TX_DMA_NBR);
	WAIT_DMA(ATA_RX_DMA_NBR);
}

#define cris_dma_descr_type etrax_dma_descr
#define cris_pio_read IO_STATE(R_ATA_CTRL_DATA, rw, read)
#define cris_ultra_mask 0x0
#define MAX_DESCR_SIZE 0x10000UL

static unsigned long
cris_ide_get_reg(unsigned long reg)
{
	return (reg & 0x0e000000) >> 25;
}

static void
cris_ide_fill_descriptor(cris_dma_descr_type *d, void* buf, unsigned int len, int last)
{
	d->buf = virt_to_phys(buf);
	d->sw_len = len == MAX_DESCR_SIZE ? 0 : len;
	if (last)
		d->ctrl |= d_eol;
}

static void cris_ide_start_dma(ide_drive_t *drive, cris_dma_descr_type *d, int dir, int type, int len)
{
	unsigned long cmd;

	if (dir) {
		/* need to do this before RX DMA due to a chip bug
		 * it is enough to just flush the part of the cache that
		 * corresponds to the buffers we start, but since HD transfers
		 * usually are more than 8 kB, it is easier to optimize for the
		 * normal case and just flush the entire cache. its the only
		 * way to be sure! (OB movie quote)
		 */
		flush_etrax_cache();
		*R_DMA_CH3_FIRST = virt_to_phys(d);
		*R_DMA_CH3_CMD   = IO_STATE(R_DMA_CH3_CMD, cmd, start);

	} else {
		*R_DMA_CH2_FIRST = virt_to_phys(d);
		*R_DMA_CH2_CMD   = IO_STATE(R_DMA_CH2_CMD, cmd, start);
	}

	/* initiate a multi word dma read using DMA handshaking */

	*R_ATA_TRANSFER_CNT =
		IO_FIELD(R_ATA_TRANSFER_CNT, count, len >> 1);

	cmd = dir ? IO_STATE(R_ATA_CTRL_DATA, rw, read) : IO_STATE(R_ATA_CTRL_DATA, rw, write);
	cmd |= type == TYPE_PIO ? IO_STATE(R_ATA_CTRL_DATA, handsh, pio) :
	                          IO_STATE(R_ATA_CTRL_DATA, handsh, dma);
	*R_ATA_CTRL_DATA =
		cmd |
		IO_FIELD(R_ATA_CTRL_DATA, data,
			 drive->hwif->io_ports.data_addr) |
		IO_STATE(R_ATA_CTRL_DATA, src_dst,  dma)  |
		IO_STATE(R_ATA_CTRL_DATA, multi,    on)   |
		IO_STATE(R_ATA_CTRL_DATA, dma_size, word);
}

static void
cris_ide_wait_dma(int dir)
{
	if (dir)
		WAIT_DMA(ATA_RX_DMA_NBR);
	else
		WAIT_DMA(ATA_TX_DMA_NBR);
}

static int cris_dma_test_irq(ide_drive_t *drive)
{
	int intr = *R_IRQ_MASK0_RD;
	int bus = IO_EXTRACT(R_ATA_CTRL_DATA, sel,
			     drive->hwif->io_ports.data_addr);

	return intr & (1 << (bus + IO_BITNR(R_IRQ_MASK0_RD, ata_irq0))) ? 1 : 0;
}


static void cris_ide_initialize_dma(int dir)
{
	if (dir)
	{
		RESET_DMA(ATA_RX_DMA_NBR); /* sometimes the DMA channel get stuck so we need to do this */
		WAIT_DMA(ATA_RX_DMA_NBR);
	}
	else
	{
		RESET_DMA(ATA_TX_DMA_NBR); /* sometimes the DMA channel get stuck so we need to do this */
		WAIT_DMA(ATA_TX_DMA_NBR);
	}
}

#endif

void
cris_ide_outw(unsigned short data, unsigned long reg) {
	int timeleft;

	LOWDB(printk("ow: data 0x%x, reg 0x%x\n", data, reg));

	/* note the lack of handling any timeouts. we stop waiting, but we don't
	 * really notify anybody.
	 */

	timeleft = IDE_REGISTER_TIMEOUT;
	/* wait for busy flag */
	do {
		timeleft--;
	} while(timeleft && cris_ide_busy());

	/*
	 * Fall through at a timeout, so the ongoing command will be
	 * aborted by the write below, which is expected to be a dummy
	 * command to the command register.  This happens when a faulty
	 * drive times out on a command.  See comment on timeout in
	 * INB.
	 */
	if(!timeleft)
		printk("ATA timeout reg 0x%lx := 0x%x\n", reg, data);

	cris_ide_write_command(reg|data); /* write data to the drive's register */

	timeleft = IDE_REGISTER_TIMEOUT;
	/* wait for transmitter ready */
	do {
		timeleft--;
	} while(timeleft && !cris_ide_ready());
}

void
cris_ide_outb(unsigned char data, unsigned long reg)
{
	cris_ide_outw(data, reg);
}

void
cris_ide_outbsync(ide_drive_t *drive, u8 addr, unsigned long port)
{
	cris_ide_outw(addr, port);
}

unsigned short
cris_ide_inw(unsigned long reg) {
	int timeleft;
	unsigned short val;

	timeleft = IDE_REGISTER_TIMEOUT;
	/* wait for busy flag */
	do {
		timeleft--;
	} while(timeleft && cris_ide_busy());

	if(!timeleft) {
		/*
		 * If we're asked to read the status register, like for
		 * example when a command does not complete for an
		 * extended time, but the ATA interface is stuck in a
		 * busy state at the *ETRAX* ATA interface level (as has
		 * happened repeatedly with at least one bad disk), then
		 * the best thing to do is to pretend that we read
		 * "busy" in the status register, so the IDE driver will
		 * time-out, abort the ongoing command and perform a
		 * reset sequence.  Note that the subsequent OUT_BYTE
		 * call will also timeout on busy, but as long as the
		 * write is still performed, everything will be fine.
		 */
		if (cris_ide_get_reg(reg) == 7)
			return BUSY_STAT;
		else
			/* For other rare cases we assume 0 is good enough.  */
			return 0;
	}

	cris_ide_write_command(reg | cris_pio_read);

	timeleft = IDE_REGISTER_TIMEOUT;
	/* wait for available */
	do {
		timeleft--;
	} while(timeleft && !cris_ide_data_available(&val));

	if(!timeleft)
		return 0;

	LOWDB(printk("inb: 0x%x from reg 0x%x\n", val & 0xff, reg));

	return val;
}

unsigned char
cris_ide_inb(unsigned long reg)
{
	return (unsigned char)cris_ide_inw(reg);
}

static void cris_input_data(ide_drive_t *, struct request *, void *, unsigned);
static void cris_output_data(ide_drive_t *, struct request *, void *, unsigned);

static void cris_dma_host_set(ide_drive_t *drive, int on)
{
}

static void cris_set_pio_mode(ide_drive_t *drive, const u8 pio)
{
	int setup, strobe, hold;

	switch(pio)
	{
		case 0:
			setup = ATA_PIO0_SETUP;
			strobe = ATA_PIO0_STROBE;
			hold = ATA_PIO0_HOLD;
			break;
		case 1:
			setup = ATA_PIO1_SETUP;
			strobe = ATA_PIO1_STROBE;
			hold = ATA_PIO1_HOLD;
			break;
		case 2:
			setup = ATA_PIO2_SETUP;
			strobe = ATA_PIO2_STROBE;
			hold = ATA_PIO2_HOLD;
			break;
		case 3:
			setup = ATA_PIO3_SETUP;
			strobe = ATA_PIO3_STROBE;
			hold = ATA_PIO3_HOLD;
			break;
		case 4:
			setup = ATA_PIO4_SETUP;
			strobe = ATA_PIO4_STROBE;
			hold = ATA_PIO4_HOLD;
			break;
		default:
			return;
	}

	cris_ide_set_speed(TYPE_PIO, setup, strobe, hold);
}

static void cris_set_dma_mode(ide_drive_t *drive, const u8 speed)
{
	int cyc = 0, dvs = 0, strobe = 0, hold = 0;

	switch(speed)
	{
		case XFER_UDMA_0:
			cyc = ATA_UDMA0_CYC;
			dvs = ATA_UDMA0_DVS;
			break;
		case XFER_UDMA_1:
			cyc = ATA_UDMA1_CYC;
			dvs = ATA_UDMA1_DVS;
			break;
		case XFER_UDMA_2:
			cyc = ATA_UDMA2_CYC;
			dvs = ATA_UDMA2_DVS;
			break;
		case XFER_MW_DMA_0:
			strobe = ATA_DMA0_STROBE;
			hold = ATA_DMA0_HOLD;
			break;
		case XFER_MW_DMA_1:
			strobe = ATA_DMA1_STROBE;
			hold = ATA_DMA1_HOLD;
			break;
		case XFER_MW_DMA_2:
			strobe = ATA_DMA2_STROBE;
			hold = ATA_DMA2_HOLD;
			break;
	}

	if (speed >= XFER_UDMA_0)
		cris_ide_set_speed(TYPE_UDMA, cyc, dvs, 0);
	else
		cris_ide_set_speed(TYPE_DMA, 0, strobe, hold);
}

static void __init cris_setup_ports(hw_regs_t *hw, unsigned long base)
{
	int i;

	memset(hw, 0, sizeof(*hw));

	for (i = 0; i <= 7; i++)
		hw->io_ports_array[i] = base + cris_ide_reg_addr(i, 0, 1);

	/*
	 * the IDE control register is at ATA address 6,
	 * with CS1 active instead of CS0
	 */
	hw->io_ports.ctl_addr = base + cris_ide_reg_addr(6, 1, 0);

	hw->irq = ide_default_irq(0);
	hw->ack_intr = cris_ide_ack_intr;
}

static void cris_tf_load(ide_drive_t *drive, ide_task_t *task)
{
	struct ide_io_ports *io_ports = &drive->hwif->io_ports;
	struct ide_taskfile *tf = &task->tf;
	u8 HIHI = (task->tf_flags & IDE_TFLAG_LBA48) ? 0xE0 : 0xEF;

	if (task->tf_flags & IDE_TFLAG_FLAGGED)
		HIHI = 0xFF;

	ide_set_irq(drive, 1);

	if (task->tf_flags & IDE_TFLAG_OUT_DATA)
		cris_ide_outw((tf->hob_data << 8) | tf->data,
			      io_ports->data_addr);

	if (task->tf_flags & IDE_TFLAG_OUT_HOB_FEATURE)
		cris_ide_outb(tf->hob_feature, io_ports->feature_addr);
	if (task->tf_flags & IDE_TFLAG_OUT_HOB_NSECT)
		cris_ide_outb(tf->hob_nsect, io_ports->nsect_addr);
	if (task->tf_flags & IDE_TFLAG_OUT_HOB_LBAL)
		cris_ide_outb(tf->hob_lbal, io_ports->lbal_addr);
	if (task->tf_flags & IDE_TFLAG_OUT_HOB_LBAM)
		cris_ide_outb(tf->hob_lbam, io_ports->lbam_addr);
	if (task->tf_flags & IDE_TFLAG_OUT_HOB_LBAH)
		cris_ide_outb(tf->hob_lbah, io_ports->lbah_addr);

	if (task->tf_flags & IDE_TFLAG_OUT_FEATURE)
		cris_ide_outb(tf->feature, io_ports->feature_addr);
	if (task->tf_flags & IDE_TFLAG_OUT_NSECT)
		cris_ide_outb(tf->nsect, io_ports->nsect_addr);
	if (task->tf_flags & IDE_TFLAG_OUT_LBAL)
		cris_ide_outb(tf->lbal, io_ports->lbal_addr);
	if (task->tf_flags & IDE_TFLAG_OUT_LBAM)
		cris_ide_outb(tf->lbam, io_ports->lbam_addr);
	if (task->tf_flags & IDE_TFLAG_OUT_LBAH)
		cris_ide_outb(tf->lbah, io_ports->lbah_addr);

	if (task->tf_flags & IDE_TFLAG_OUT_DEVICE)
		cris_ide_outb((tf->device & HIHI) | drive->select.all,
			      io_ports->device_addr);
}

static void cris_tf_read(ide_drive_t *drive, ide_task_t *task)
{
	struct ide_io_ports *io_ports = &drive->hwif->io_ports;
	struct ide_taskfile *tf = &task->tf;

	if (task->tf_flags & IDE_TFLAG_IN_DATA) {
		u16 data = cris_ide_inw(io_ports->data_addr);

		tf->data = data & 0xff;
		tf->hob_data = (data >> 8) & 0xff;
	}

	/* be sure we're looking at the low order bits */
	cris_ide_outb(drive->ctl & ~0x80, io_ports->ctl_addr);

	if (task->tf_flags & IDE_TFLAG_IN_NSECT)
		tf->nsect  = cris_ide_inb(io_ports->nsect_addr);
	if (task->tf_flags & IDE_TFLAG_IN_LBAL)
		tf->lbal   = cris_ide_inb(io_ports->lbal_addr);
	if (task->tf_flags & IDE_TFLAG_IN_LBAM)
		tf->lbam   = cris_ide_inb(io_ports->lbam_addr);
	if (task->tf_flags & IDE_TFLAG_IN_LBAH)
		tf->lbah   = cris_ide_inb(io_ports->lbah_addr);
	if (task->tf_flags & IDE_TFLAG_IN_DEVICE)
		tf->device = cris_ide_inb(io_ports->device_addr);

	if (task->tf_flags & IDE_TFLAG_LBA48) {
		cris_ide_outb(drive->ctl | 0x80, io_ports->ctl_addr);

		if (task->tf_flags & IDE_TFLAG_IN_HOB_FEATURE)
			tf->hob_feature = cris_ide_inb(io_ports->feature_addr);
		if (task->tf_flags & IDE_TFLAG_IN_HOB_NSECT)
			tf->hob_nsect   = cris_ide_inb(io_ports->nsect_addr);
		if (task->tf_flags & IDE_TFLAG_IN_HOB_LBAL)
			tf->hob_lbal    = cris_ide_inb(io_ports->lbal_addr);
		if (task->tf_flags & IDE_TFLAG_IN_HOB_LBAM)
			tf->hob_lbam    = cris_ide_inb(io_ports->lbam_addr);
		if (task->tf_flags & IDE_TFLAG_IN_HOB_LBAH)
			tf->hob_lbah    = cris_ide_inb(io_ports->lbah_addr);
	}
}

static const struct ide_port_ops cris_port_ops = {
	.set_pio_mode		= cris_set_pio_mode,
	.set_dma_mode		= cris_set_dma_mode,
};

static const struct ide_dma_ops cris_dma_ops;

static const struct ide_port_info cris_port_info __initdata = {
	.chipset		= ide_etrax100,
	.port_ops		= &cris_port_ops,
	.dma_ops		= &cris_dma_ops,
	.host_flags		= IDE_HFLAG_NO_ATAPI_DMA |
				  IDE_HFLAG_NO_DMA, /* no SFF-style DMA */
	.pio_mask		= ATA_PIO4,
	.udma_mask		= cris_ultra_mask,
	.mwdma_mask		= ATA_MWDMA2,
};

static int __init init_e100_ide(void)
{
	hw_regs_t hw;
	int h;
	u8 idx[4] = { 0xff, 0xff, 0xff, 0xff };

	printk("ide: ETRAX FS built-in ATA DMA controller\n");

	for (h = 0; h < 4; h++) {
		ide_hwif_t *hwif = NULL;

		cris_setup_ports(&hw, cris_ide_base_address(h));

		hwif = ide_find_port();
		if (hwif == NULL)
			continue;
		ide_init_port_data(hwif, hwif->index);
		ide_init_port_hw(hwif, &hw);

		hwif->tf_load = cris_tf_load;
		hwif->tf_read = cris_tf_read;

		hwif->input_data  = cris_input_data;
		hwif->output_data = cris_output_data;

		hwif->OUTB = &cris_ide_outb;
		hwif->OUTBSYNC = &cris_ide_outbsync;
		hwif->INB = &cris_ide_inb;
		hwif->cbl = ATA_CBL_PATA40;

		idx[h] = hwif->index;
	}

	/* Reset pulse */
	cris_ide_reset(0);
	udelay(25);
	cris_ide_reset(1);

	cris_ide_init();

	cris_ide_set_speed(TYPE_PIO, ATA_PIO4_SETUP, ATA_PIO4_STROBE, ATA_PIO4_HOLD);
	cris_ide_set_speed(TYPE_DMA, 0, ATA_DMA2_STROBE, ATA_DMA2_HOLD);
	cris_ide_set_speed(TYPE_UDMA, ATA_UDMA2_CYC, ATA_UDMA2_DVS, 0);

	ide_device_add(idx, &cris_port_info);

	return 0;
}

static cris_dma_descr_type mydescr __attribute__ ((__aligned__(16)));

/*
 * This is used for most PIO data transfers *from* the IDE interface
 *
 * These routines will round up any request for an odd number of bytes,
 * so if an odd bytecount is specified, be sure that there's at least one
 * extra byte allocated for the buffer.
 */
static void cris_input_data(ide_drive_t *drive, struct request *rq,
			    void *buffer, unsigned int bytecount)
{
	D(printk("input_data, buffer 0x%x, count %d\n", buffer, bytecount));

	if(bytecount & 1) {
		printk("warning, odd bytecount in cdrom_in_bytes = %d.\n", bytecount);
		bytecount++; /* to round off */
	}

	/* setup DMA and start transfer */

	cris_ide_fill_descriptor(&mydescr, buffer, bytecount, 1);
	cris_ide_start_dma(drive, &mydescr, 1, TYPE_PIO, bytecount);

	/* wait for completion */
	LED_DISK_READ(1);
	cris_ide_wait_dma(1);
	LED_DISK_READ(0);
}

/*
 * This is used for most PIO data transfers *to* the IDE interface
 */
static void cris_output_data(ide_drive_t *drive,  struct request *rq,
			     void *buffer, unsigned int bytecount)
{
	D(printk("output_data, buffer 0x%x, count %d\n", buffer, bytecount));

	if(bytecount & 1) {
		printk("odd bytecount %d in atapi_out_bytes!\n", bytecount);
		bytecount++;
	}

	cris_ide_fill_descriptor(&mydescr, buffer, bytecount, 1);
	cris_ide_start_dma(drive, &mydescr, 0, TYPE_PIO, bytecount);

	/* wait for completion */

	LED_DISK_WRITE(1);
	LED_DISK_READ(1);
	cris_ide_wait_dma(0);
	LED_DISK_WRITE(0);
}

/* we only have one DMA channel on the chip for ATA, so we can keep these statically */
static cris_dma_descr_type ata_descrs[MAX_DMA_DESCRS] __attribute__ ((__aligned__(16)));
static unsigned int ata_tot_size;

/*
 * cris_ide_build_dmatable() prepares a dma request.
 * Returns 0 if all went okay, returns 1 otherwise.
 */
static int cris_ide_build_dmatable (ide_drive_t *drive)
{
	ide_hwif_t *hwif = drive->hwif;
	struct scatterlist* sg;
	struct request *rq  = drive->hwif->hwgroup->rq;
	unsigned long size, addr;
	unsigned int count = 0;
	int i = 0;

	sg = hwif->sg_table;

	ata_tot_size = 0;

	ide_map_sg(drive, rq);
	i = hwif->sg_nents;

	while(i) {
		/*
		 * Determine addr and size of next buffer area.  We assume that
		 * individual virtual buffers are always composed linearly in
		 * physical memory.  For example, we assume that any 8kB buffer
		 * is always composed of two adjacent physical 4kB pages rather
		 * than two possibly non-adjacent physical 4kB pages.
		 */
		/* group sequential buffers into one large buffer */
		addr = sg_phys(sg);
		size = sg_dma_len(sg);
		while (--i) {
			sg = sg_next(sg);
			if ((addr + size) != sg_phys(sg))
				break;
			size += sg_dma_len(sg);
		}

		/* did we run out of descriptors? */

		if(count >= MAX_DMA_DESCRS) {
			printk("%s: too few DMA descriptors\n", drive->name);
			return 1;
		}

		/* however, this case is more difficult - rw_trf_cnt cannot be more
		   than 65536 words per transfer, so in that case we need to either
		   1) use a DMA interrupt to re-trigger rw_trf_cnt and continue with
		      the descriptors, or
		   2) simply do the request here, and get dma_intr to only ide_end_request on
		      those blocks that were actually set-up for transfer.
		*/

		if(ata_tot_size + size > 131072) {
			printk("too large total ATA DMA request, %d + %d!\n", ata_tot_size, (int)size);
			return 1;
		}

		/* If size > MAX_DESCR_SIZE it has to be splitted into new descriptors. Since we
                   don't handle size > 131072 only one split is necessary */

		if(size > MAX_DESCR_SIZE) {
			cris_ide_fill_descriptor(&ata_descrs[count], (void*)addr, MAX_DESCR_SIZE, 0);
			count++;
			ata_tot_size += MAX_DESCR_SIZE;
			size -= MAX_DESCR_SIZE;
			addr += MAX_DESCR_SIZE;
		}

		cris_ide_fill_descriptor(&ata_descrs[count], (void*)addr, size,i ? 0 : 1);
		count++;
		ata_tot_size += size;
	}

	if (count) {
		/* return and say all is ok */
		return 0;
	}

	printk("%s: empty DMA table?\n", drive->name);
	return 1;	/* let the PIO routines handle this weirdness */
}

/*
 * cris_dma_intr() is the handler for disk read/write DMA interrupts
 */
static ide_startstop_t cris_dma_intr (ide_drive_t *drive)
{
	LED_DISK_READ(0);
	LED_DISK_WRITE(0);

	return ide_dma_intr(drive);
}

/*
 * Functions below initiates/aborts DMA read/write operations on a drive.
 *
 * The caller is assumed to have selected the drive and programmed the drive's
 * sector address using CHS or LBA.  All that remains is to prepare for DMA
 * and then issue the actual read/write DMA/PIO command to the drive.
 *
 * For ATAPI devices, we just prepare for DMA and return. The caller should
 * then issue the packet command to the drive and call us again with
 * cris_dma_start afterwards.
 *
 * Returns 0 if all went well.
 * Returns 1 if DMA read/write could not be started, in which case
 * the caller should revert to PIO for the current request.
 */

static int cris_dma_end(ide_drive_t *drive)
{
	drive->waiting_for_dma = 0;
	return 0;
}

static int cris_dma_setup(ide_drive_t *drive)
{
	struct request *rq = drive->hwif->hwgroup->rq;

	cris_ide_initialize_dma(!rq_data_dir(rq));
	if (cris_ide_build_dmatable (drive)) {
		ide_map_sg(drive, rq);
		return 1;
	}

	drive->waiting_for_dma = 1;
	return 0;
}

static void cris_dma_exec_cmd(ide_drive_t *drive, u8 command)
{
	ide_execute_command(drive, command, &cris_dma_intr, WAIT_CMD, NULL);
}

static void cris_dma_start(ide_drive_t *drive)
{
	struct request *rq = drive->hwif->hwgroup->rq;
	int writing = rq_data_dir(rq);
	int type = TYPE_DMA;

	if (drive->current_speed >= XFER_UDMA_0)
		type = TYPE_UDMA;

	cris_ide_start_dma(drive, &ata_descrs[0], writing ? 0 : 1, type, ata_tot_size);

	if (writing) {
		LED_DISK_WRITE(1);
	} else {
		LED_DISK_READ(1);
	}
}

static const struct ide_dma_ops cris_dma_ops = {
	.dma_host_set		= cris_dma_host_set,
	.dma_setup		= cris_dma_setup,
	.dma_exec_cmd		= cris_dma_exec_cmd,
	.dma_start		= cris_dma_start,
	.dma_end		= cris_dma_end,
	.dma_test_irq		= cris_dma_test_irq,
};

module_init(init_e100_ide);

MODULE_LICENSE("GPL");
