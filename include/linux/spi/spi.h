/*
 * Copyright (C) 2005 David Brownell
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
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#ifndef __LINUX_SPI_H
#define __LINUX_SPI_H

/*
 * INTERFACES between SPI master drivers and infrastructure
 * (There's no SPI slave support for Linux yet...)
 *
 * A "struct device_driver" for an spi_device uses "spi_bus_type" and
 * needs no special API wrappers (much like platform_bus).  These drivers
 * are bound to devices based on their names (much like platform_bus),
 * and are available in dev->driver.
 */
extern struct bus_type spi_bus_type;

/**
 * struct spi_device - Master side proxy for an SPI slave device
 * @dev: Driver model representation of the device.
 * @master: SPI controller used with the device.
 * @max_speed_hz: Maximum clock rate to be used with this chip
 *	(on this board); may be changed by the device's driver.
 * @chip-select: Chipselect, distinguishing chips handled by "master".
 * @mode: The spi mode defines how data is clocked out and in.
 *	This may be changed by the device's driver.
 * @bits_per_word: Data transfers involve one or more words; word sizes
 * 	like eight or 12 bits are common.  In-memory wordsizes are
 *	powers of two bytes (e.g. 20 bit samples use 32 bits).
 *	This may be changed by the device's driver.
 * @irq: Negative, or the number passed to request_irq() to receive
 * 	interrupts from this device.
 * @controller_state: Controller's runtime state
 * @controller_data: Static board-specific definitions for controller, such
 * 	as FIFO initialization parameters; from board_info.controller_data
 *
 * An spi_device is used to interchange data between an SPI slave
 * (usually a discrete chip) and CPU memory.
 *
 * In "dev", the platform_data is used to hold information about this
 * device that's meaningful to the device's protocol driver, but not
 * to its controller.  One example might be an identifier for a chip
 * variant with slightly different functionality.
 */
struct spi_device {
	struct device		dev;
	struct spi_master	*master;
	u32			max_speed_hz;
	u8			chip_select;
	u8			mode;
#define	SPI_CPHA	0x01		/* clock phase */
#define	SPI_CPOL	0x02		/* clock polarity */
#define	SPI_MODE_0	(0|0)
#define	SPI_MODE_1	(0|SPI_CPHA)
#define	SPI_MODE_2	(SPI_CPOL|0)
#define	SPI_MODE_3	(SPI_CPOL|SPI_CPHA)
#define	SPI_CS_HIGH	0x04		/* chipselect active high? */
	u8			bits_per_word;
	int			irq;
	void			*controller_state;
	const void		*controller_data;
	const char		*modalias;

	// likely need more hooks for more protocol options affecting how
	// the controller talks to its chips, like:
	//  - bit order (default is wordwise msb-first)
	//  - memory packing (12 bit samples into low bits, others zeroed)
	//  - priority
	//  - chipselect delays
	//  - ...
};

static inline struct spi_device *to_spi_device(struct device *dev)
{
	return container_of(dev, struct spi_device, dev);
}

/* most drivers won't need to care about device refcounting */
static inline struct spi_device *spi_dev_get(struct spi_device *spi)
{
	return (spi && get_device(&spi->dev)) ? spi : NULL;
}

static inline void spi_dev_put(struct spi_device *spi)
{
	if (spi)
		put_device(&spi->dev);
}

/* ctldata is for the bus_master driver's runtime state */
static inline void *spi_get_ctldata(struct spi_device *spi)
{
	return spi->controller_state;
}

static inline void spi_set_ctldata(struct spi_device *spi, void *state)
{
	spi->controller_state = state;
}


struct spi_message;


/**
 * struct spi_master - interface to SPI master controller
 * @cdev: class interface to this driver
 * @bus_num: board-specific (and often SOC-specific) identifier for a
 * 	given SPI controller.
 * @num_chipselects: chipselects are used to distinguish individual
 * 	SPI slaves, and are numbered from zero to num_chipselects.
 * 	each slave has a chipselect signal, but it's common that not
 * 	every chipselect is connected to a slave.
 * @setup: updates the device mode and clocking records used by a
 * 	device's SPI controller; protocol code may call this.
 * @transfer: adds a message to the controller's transfer queue.
 * @cleanup: frees controller-specific state
 *
 * Each SPI master controller can communicate with one or more spi_device
 * children.  These make a small bus, sharing MOSI, MISO and SCK signals
 * but not chip select signals.  Each device may be configured to use a
 * different clock rate, since those shared signals are ignored unless
 * the chip is selected.
 *
 * The driver for an SPI controller manages access to those devices through
 * a queue of spi_message transactions, copyin data between CPU memory and
 * an SPI slave device).  For each such message it queues, it calls the
 * message's completion function when the transaction completes.
 */
struct spi_master {
	struct class_device	cdev;

	/* other than zero (== assign one dynamically), bus_num is fully
	 * board-specific.  usually that simplifies to being SOC-specific.
	 * example:  one SOC has three SPI controllers, numbered 1..3,
	 * and one board's schematics might show it using SPI-2.  software
	 * would normally use bus_num=2 for that controller.
	 */
	u16			bus_num;

	/* chipselects will be integral to many controllers; some others
	 * might use board-specific GPIOs.
	 */
	u16			num_chipselect;

	/* setup mode and clock, etc (spi driver may call many times) */
	int			(*setup)(struct spi_device *spi);

	/* bidirectional bulk transfers
	 *
	 * + The transfer() method may not sleep; its main role is
	 *   just to add the message to the queue.
	 * + For now there's no remove-from-queue operation, or
	 *   any other request management
	 * + To a given spi_device, message queueing is pure fifo
	 *
	 * + The master's main job is to process its message queue,
	 *   selecting a chip then transferring data
	 * + If there are multiple spi_device children, the i/o queue
	 *   arbitration algorithm is unspecified (round robin, fifo,
	 *   priority, reservations, preemption, etc)
	 *
	 * + Chipselect stays active during the entire message
	 *   (unless modified by spi_transfer.cs_change != 0).
	 * + The message transfers use clock and SPI mode parameters
	 *   previously established by setup() for this device
	 */
	int			(*transfer)(struct spi_device *spi,
						struct spi_message *mesg);

	/* called on release() to free memory provided by spi_master */
	void			(*cleanup)(const struct spi_device *spi);
};

/* the spi driver core manages memory for the spi_master classdev */
extern struct spi_master *
spi_alloc_master(struct device *host, unsigned size);

extern int spi_register_master(struct spi_master *master);
extern void spi_unregister_master(struct spi_master *master);

extern struct spi_master *spi_busnum_to_master(u16 busnum);

/*---------------------------------------------------------------------------*/

/*
 * I/O INTERFACE between SPI controller and protocol drivers
 *
 * Protocol drivers use a queue of spi_messages, each transferring data
 * between the controller and memory buffers.
 *
 * The spi_messages themselves consist of a series of read+write transfer
 * segments.  Those segments always read the same number of bits as they
 * write; but one or the other is easily ignored by passing a null buffer
 * pointer.  (This is unlike most types of I/O API, because SPI hardware
 * is full duplex.)
 *
 * NOTE:  Allocation of spi_transfer and spi_message memory is entirely
 * up to the protocol driver, which guarantees the integrity of both (as
 * well as the data buffers) for as long as the message is queued.
 */

/**
 * struct spi_transfer - a read/write buffer pair
 * @tx_buf: data to be written (dma-safe address), or NULL
 * @rx_buf: data to be read (dma-safe address), or NULL
 * @tx_dma: DMA address of buffer, if spi_message.is_dma_mapped
 * @rx_dma: DMA address of buffer, if spi_message.is_dma_mapped
 * @len: size of rx and tx buffers (in bytes)
 * @cs_change: affects chipselect after this transfer completes
 * @delay_usecs: microseconds to delay after this transfer before
 * 	(optionally) changing the chipselect status, then starting
 * 	the next transfer or completing this spi_message.
 *
 * SPI transfers always write the same number of bytes as they read.
 * Protocol drivers should always provide rx_buf and/or tx_buf.
 * In some cases, they may also want to provide DMA addresses for
 * the data being transferred; that may reduce overhead, when the
 * underlying driver uses dma.
 *
 * All SPI transfers start with the relevant chipselect active.  Drivers
 * can change behavior of the chipselect after the transfer finishes
 * (including any mandatory delay).  The normal behavior is to leave it
 * selected, except for the last transfer in a message.  Setting cs_change
 * allows two additional behavior options:
 *
 * (i) If the transfer isn't the last one in the message, this flag is
 * used to make the chipselect briefly go inactive in the middle of the
 * message.  Toggling chipselect in this way may be needed to terminate
 * a chip command, letting a single spi_message perform all of group of
 * chip transactions together.
 *
 * (ii) When the transfer is the last one in the message, the chip may
 * stay selected until the next transfer.  This is purely a performance
 * hint; the controller driver may need to select a different device
 * for the next message.
 */
struct spi_transfer {
	/* it's ok if tx_buf == rx_buf (right?)
	 * for MicroWire, one buffer must be null
	 * buffers must work with dma_*map_single() calls
	 */
	const void	*tx_buf;
	void		*rx_buf;
	unsigned	len;

	dma_addr_t	tx_dma;
	dma_addr_t	rx_dma;

	unsigned	cs_change:1;
	u16		delay_usecs;
};

/**
 * struct spi_message - one multi-segment SPI transaction
 * @transfers: the segements of the transaction
 * @n_transfer: how many segments
 * @spi: SPI device to which the transaction is queued
 * @is_dma_mapped: if true, the caller provided both dma and cpu virtual
 *	addresses for each transfer buffer
 * @complete: called to report transaction completions
 * @context: the argument to complete() when it's called
 * @actual_length: how many bytes were transferd
 * @status: zero for success, else negative errno
 * @queue: for use by whichever driver currently owns the message
 * @state: for use by whichever driver currently owns the message
 */
struct spi_message {
	struct spi_transfer	*transfers;
	unsigned		n_transfer;

	struct spi_device	*spi;

	unsigned		is_dma_mapped:1;

	/* REVISIT:  we might want a flag affecting the behavior of the
	 * last transfer ... allowing things like "read 16 bit length L"
	 * immediately followed by "read L bytes".  Basically imposing
	 * a specific message scheduling algorithm.
	 *
	 * Some controller drivers (message-at-a-time queue processing)
	 * could provide that as their default scheduling algorithm.  But
	 * others (with multi-message pipelines) would need a flag to
	 * tell them about such special cases.
	 */

	/* completion is reported through a callback */
	void 			FASTCALL((*complete)(void *context));
	void			*context;
	unsigned		actual_length;
	int			status;

	/* for optional use by whatever driver currently owns the
	 * spi_message ...  between calls to spi_async and then later
	 * complete(), that's the spi_master controller driver.
	 */
	struct list_head	queue;
	void			*state;
};

/**
 * spi_setup -- setup SPI mode and clock rate
 * @spi: the device whose settings are being modified
 *
 * SPI protocol drivers may need to update the transfer mode if the
 * device doesn't work with the mode 0 default.  They may likewise need
 * to update clock rates or word sizes from initial values.  This function
 * changes those settings, and must be called from a context that can sleep.
 */
static inline int
spi_setup(struct spi_device *spi)
{
	return spi->master->setup(spi);
}


/**
 * spi_async -- asynchronous SPI transfer
 * @spi: device with which data will be exchanged
 * @message: describes the data transfers, including completion callback
 *
 * This call may be used in_irq and other contexts which can't sleep,
 * as well as from task contexts which can sleep.
 *
 * The completion callback is invoked in a context which can't sleep.
 * Before that invocation, the value of message->status is undefined.
 * When the callback is issued, message->status holds either zero (to
 * indicate complete success) or a negative error code.
 *
 * Note that although all messages to a spi_device are handled in
 * FIFO order, messages may go to different devices in other orders.
 * Some device might be higher priority, or have various "hard" access
 * time requirements, for example.
 */
static inline int
spi_async(struct spi_device *spi, struct spi_message *message)
{
	message->spi = spi;
	return spi->master->transfer(spi, message);
}

/*---------------------------------------------------------------------------*/

/* All these synchronous SPI transfer routines are utilities layered
 * over the core async transfer primitive.  Here, "synchronous" means
 * they will sleep uninterruptibly until the async transfer completes.
 */

extern int spi_sync(struct spi_device *spi, struct spi_message *message);

/**
 * spi_write - SPI synchronous write
 * @spi: device to which data will be written
 * @buf: data buffer
 * @len: data buffer size
 *
 * This writes the buffer and returns zero or a negative error code.
 * Callable only from contexts that can sleep.
 */
static inline int
spi_write(struct spi_device *spi, const u8 *buf, size_t len)
{
	struct spi_transfer	t = {
			.tx_buf		= buf,
			.rx_buf		= NULL,
			.len		= len,
			.cs_change	= 0,
		};
	struct spi_message	m = {
			.transfers	= &t,
			.n_transfer	= 1,
		};

	return spi_sync(spi, &m);
}

/**
 * spi_read - SPI synchronous read
 * @spi: device from which data will be read
 * @buf: data buffer
 * @len: data buffer size
 *
 * This writes the buffer and returns zero or a negative error code.
 * Callable only from contexts that can sleep.
 */
static inline int
spi_read(struct spi_device *spi, u8 *buf, size_t len)
{
	struct spi_transfer	t = {
			.tx_buf		= NULL,
			.rx_buf		= buf,
			.len		= len,
			.cs_change	= 0,
		};
	struct spi_message	m = {
			.transfers	= &t,
			.n_transfer	= 1,
		};

	return spi_sync(spi, &m);
}

extern int spi_write_then_read(struct spi_device *spi,
		const u8 *txbuf, unsigned n_tx,
		u8 *rxbuf, unsigned n_rx);

/**
 * spi_w8r8 - SPI synchronous 8 bit write followed by 8 bit read
 * @spi: device with which data will be exchanged
 * @cmd: command to be written before data is read back
 *
 * This returns the (unsigned) eight bit number returned by the
 * device, or else a negative error code.  Callable only from
 * contexts that can sleep.
 */
static inline ssize_t spi_w8r8(struct spi_device *spi, u8 cmd)
{
	ssize_t			status;
	u8			result;

	status = spi_write_then_read(spi, &cmd, 1, &result, 1);

	/* return negative errno or unsigned value */
	return (status < 0) ? status : result;
}

/**
 * spi_w8r16 - SPI synchronous 8 bit write followed by 16 bit read
 * @spi: device with which data will be exchanged
 * @cmd: command to be written before data is read back
 *
 * This returns the (unsigned) sixteen bit number returned by the
 * device, or else a negative error code.  Callable only from
 * contexts that can sleep.
 *
 * The number is returned in wire-order, which is at least sometimes
 * big-endian.
 */
static inline ssize_t spi_w8r16(struct spi_device *spi, u8 cmd)
{
	ssize_t			status;
	u16			result;

	status = spi_write_then_read(spi, &cmd, 1, (u8 *) &result, 2);

	/* return negative errno or unsigned value */
	return (status < 0) ? status : result;
}

/*---------------------------------------------------------------------------*/

/*
 * INTERFACE between board init code and SPI infrastructure.
 *
 * No SPI driver ever sees these SPI device table segments, but
 * it's how the SPI core (or adapters that get hotplugged) grows
 * the driver model tree.
 *
 * As a rule, SPI devices can't be probed.  Instead, board init code
 * provides a table listing the devices which are present, with enough
 * information to bind and set up the device's driver.  There's basic
 * support for nonstatic configurations too; enough to handle adding
 * parport adapters, or microcontrollers acting as USB-to-SPI bridges.
 */

/* board-specific information about each SPI device */
struct spi_board_info {
	/* the device name and module name are coupled, like platform_bus;
	 * "modalias" is normally the driver name.
	 *
	 * platform_data goes to spi_device.dev.platform_data,
	 * controller_data goes to spi_device.platform_data,
	 * irq is copied too
	 */
	char		modalias[KOBJ_NAME_LEN];
	const void	*platform_data;
	const void	*controller_data;
	int		irq;

	/* slower signaling on noisy or low voltage boards */
	u32		max_speed_hz;


	/* bus_num is board specific and matches the bus_num of some
	 * spi_master that will probably be registered later.
	 *
	 * chip_select reflects how this chip is wired to that master;
	 * it's less than num_chipselect.
	 */
	u16		bus_num;
	u16		chip_select;

	/* ... may need additional spi_device chip config data here.
	 * avoid stuff protocol drivers can set; but include stuff
	 * needed to behave without being bound to a driver:
	 *  - chipselect polarity
	 *  - quirks like clock rate mattering when not selected
	 */
};

#ifdef	CONFIG_SPI
extern int
spi_register_board_info(struct spi_board_info const *info, unsigned n);
#else
/* board init code may ignore whether SPI is configured or not */
static inline int
spi_register_board_info(struct spi_board_info const *info, unsigned n)
	{ return 0; }
#endif


/* If you're hotplugging an adapter with devices (parport, usb, etc)
 * use spi_new_device() to describe each device.  You can also call
 * spi_unregister_device() to get start making that device vanish,
 * but normally that would be handled by spi_unregister_master().
 */
extern struct spi_device *
spi_new_device(struct spi_master *, struct spi_board_info *);

static inline void
spi_unregister_device(struct spi_device *spi)
{
	if (spi)
		device_unregister(&spi->dev);
}

#endif /* __LINUX_SPI_H */
