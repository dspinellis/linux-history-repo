/*
 *    pata_efar.c - EFAR PIIX clone controller driver
 *
 *	(C) 2005 Red Hat <alan@redhat.com>
 *
 *    Some parts based on ata_piix.c by Jeff Garzik and others.
 *
 *    The EFAR is a PIIX4 clone with UDMA66 support. Unlike the later
 *    Intel ICH controllers the EFAR widened the UDMA mode register bits
 *    and doesn't require the funky clock selection.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <scsi/scsi_host.h>
#include <linux/libata.h>
#include <linux/ata.h>

#define DRV_NAME	"pata_efar"
#define DRV_VERSION	"0.4.2"

/**
 *	efar_pre_reset	-	check for 40/80 pin
 *	@ap: Port
 *
 *	Perform cable detection for the EFAR ATA interface. This is
 *	different to the PIIX arrangement
 */

static int efar_pre_reset(struct ata_port *ap)
{
	static const struct pci_bits efar_enable_bits[] = {
		{ 0x41U, 1U, 0x80UL, 0x80UL },	/* port 0 */
		{ 0x43U, 1U, 0x80UL, 0x80UL },	/* port 1 */
	};

	struct pci_dev *pdev = to_pci_dev(ap->host->dev);
	u8 tmp;

	if (!pci_test_config_bits(pdev, &efar_enable_bits[ap->port_no]))
		return -ENOENT;

	pci_read_config_byte(pdev, 0x47, &tmp);
	if (tmp & (2 >> ap->port_no))
		ap->cbl = ATA_CBL_PATA40;
	else
		ap->cbl = ATA_CBL_PATA80;
	return ata_std_prereset(ap);
}

/**
 *	efar_probe_reset - Probe specified port on PATA host controller
 *	@ap: Port to probe
 *
 *	LOCKING:
 *	None (inherited from caller).
 */

static void efar_error_handler(struct ata_port *ap)
{
	ata_bmdma_drive_eh(ap, efar_pre_reset, ata_std_softreset, NULL, ata_std_postreset);
}

/**
 *	efar_set_piomode - Initialize host controller PATA PIO timings
 *	@ap: Port whose timings we are configuring
 *	@adev: um
 *
 *	Set PIO mode for device, in host controller PCI config space.
 *
 *	LOCKING:
 *	None (inherited from caller).
 */

static void efar_set_piomode (struct ata_port *ap, struct ata_device *adev)
{
	unsigned int pio	= adev->pio_mode - XFER_PIO_0;
	struct pci_dev *dev	= to_pci_dev(ap->host->dev);
	unsigned int idetm_port= ap->port_no ? 0x42 : 0x40;
	u16 idetm_data;
	int control = 0;

	/*
	 *	See Intel Document 298600-004 for the timing programing rules
	 *	for PIIX/ICH. The EFAR is a clone so very similar
	 */

	static const	 /* ISP  RTC */
	u8 timings[][2]	= { { 0, 0 },
			    { 0, 0 },
			    { 1, 0 },
			    { 2, 1 },
			    { 2, 3 }, };

	if (pio > 2)
		control |= 1;	/* TIME1 enable */
	if (ata_pio_need_iordy(adev))	/* PIO 3/4 require IORDY */
		control |= 2;	/* IE enable */
	/* Intel specifies that the PPE functionality is for disk only */
	if (adev->class == ATA_DEV_ATA)
		control |= 4;	/* PPE enable */

	pci_read_config_word(dev, idetm_port, &idetm_data);

	/* Enable PPE, IE and TIME as appropriate */

	if (adev->devno == 0) {
		idetm_data &= 0xCCF0;
		idetm_data |= control;
		idetm_data |= (timings[pio][0] << 12) |
			(timings[pio][1] << 8);
	} else {
		int shift = 4 * ap->port_no;
		u8 slave_data;

		idetm_data &= 0xCC0F;
		idetm_data |= (control << 4);

		/* Slave timing in seperate register */
		pci_read_config_byte(dev, 0x44, &slave_data);
		slave_data &= 0x0F << shift;
		slave_data |= ((timings[pio][0] << 2) | timings[pio][1]) << shift;
		pci_write_config_byte(dev, 0x44, slave_data);
	}

	idetm_data |= 0x4000;	/* Ensure SITRE is enabled */
	pci_write_config_word(dev, idetm_port, idetm_data);
}

/**
 *	efar_set_dmamode - Initialize host controller PATA DMA timings
 *	@ap: Port whose timings we are configuring
 *	@adev: Device to program
 *
 *	Set UDMA/MWDMA mode for device, in host controller PCI config space.
 *
 *	LOCKING:
 *	None (inherited from caller).
 */

static void efar_set_dmamode (struct ata_port *ap, struct ata_device *adev)
{
	struct pci_dev *dev	= to_pci_dev(ap->host->dev);
	u8 master_port		= ap->port_no ? 0x42 : 0x40;
	u16 master_data;
	u8 speed		= adev->dma_mode;
	int devid		= adev->devno + 2 * ap->port_no;
	u8 udma_enable;

	static const	 /* ISP  RTC */
	u8 timings[][2]	= { { 0, 0 },
			    { 0, 0 },
			    { 1, 0 },
			    { 2, 1 },
			    { 2, 3 }, };

	pci_read_config_word(dev, master_port, &master_data);
	pci_read_config_byte(dev, 0x48, &udma_enable);

	if (speed >= XFER_UDMA_0) {
		unsigned int udma	= adev->dma_mode - XFER_UDMA_0;
		u16 udma_timing;

		udma_enable |= (1 << devid);

		/* Load the UDMA mode number */
		pci_read_config_word(dev, 0x4A, &udma_timing);
		udma_timing &= ~(7 << (4 * devid));
		udma_timing |= udma << (4 * devid);
		pci_write_config_word(dev, 0x4A, udma_timing);
	} else {
		/*
		 * MWDMA is driven by the PIO timings. We must also enable
		 * IORDY unconditionally along with TIME1. PPE has already
		 * been set when the PIO timing was set.
		 */
		unsigned int mwdma	= adev->dma_mode - XFER_MW_DMA_0;
		unsigned int control;
		u8 slave_data;
		const unsigned int needed_pio[3] = {
			XFER_PIO_0, XFER_PIO_3, XFER_PIO_4
		};
		int pio = needed_pio[mwdma] - XFER_PIO_0;

		control = 3;	/* IORDY|TIME1 */

		/* If the drive MWDMA is faster than it can do PIO then
		   we must force PIO into PIO0 */

		if (adev->pio_mode < needed_pio[mwdma])
			/* Enable DMA timing only */
			control |= 8;	/* PIO cycles in PIO0 */

		if (adev->devno) {	/* Slave */
			master_data &= 0xFF4F;  /* Mask out IORDY|TIME1|DMAONLY */
			master_data |= control << 4;
			pci_read_config_byte(dev, 0x44, &slave_data);
			slave_data &= (0x0F + 0xE1 * ap->port_no);
			/* Load the matching timing */
			slave_data |= ((timings[pio][0] << 2) | timings[pio][1]) << (ap->port_no ? 4 : 0);
			pci_write_config_byte(dev, 0x44, slave_data);
		} else { 	/* Master */
			master_data &= 0xCCF4;	/* Mask out IORDY|TIME1|DMAONLY
						   and master timing bits */
			master_data |= control;
			master_data |=
				(timings[pio][0] << 12) |
				(timings[pio][1] << 8);
		}
		udma_enable &= ~(1 << devid);
		pci_write_config_word(dev, master_port, master_data);
	}
	pci_write_config_byte(dev, 0x48, udma_enable);
}

static struct scsi_host_template efar_sht = {
	.module			= THIS_MODULE,
	.name			= DRV_NAME,
	.ioctl			= ata_scsi_ioctl,
	.queuecommand		= ata_scsi_queuecmd,
	.can_queue		= ATA_DEF_QUEUE,
	.this_id		= ATA_SHT_THIS_ID,
	.sg_tablesize		= LIBATA_MAX_PRD,
	.max_sectors		= ATA_MAX_SECTORS,
	.cmd_per_lun		= ATA_SHT_CMD_PER_LUN,
	.emulated		= ATA_SHT_EMULATED,
	.use_clustering		= ATA_SHT_USE_CLUSTERING,
	.proc_name		= DRV_NAME,
	.dma_boundary		= ATA_DMA_BOUNDARY,
	.slave_configure	= ata_scsi_slave_config,
	.bios_param		= ata_std_bios_param,
};

static const struct ata_port_operations efar_ops = {
	.port_disable		= ata_port_disable,
	.set_piomode		= efar_set_piomode,
	.set_dmamode		= efar_set_dmamode,
	.mode_filter		= ata_pci_default_filter,

	.tf_load		= ata_tf_load,
	.tf_read		= ata_tf_read,
	.check_status		= ata_check_status,
	.exec_command		= ata_exec_command,
	.dev_select		= ata_std_dev_select,

	.freeze			= ata_bmdma_freeze,
	.thaw			= ata_bmdma_thaw,
	.error_handler		= efar_error_handler,
	.post_internal_cmd	= ata_bmdma_post_internal_cmd,

	.bmdma_setup		= ata_bmdma_setup,
	.bmdma_start		= ata_bmdma_start,
	.bmdma_stop		= ata_bmdma_stop,
	.bmdma_status		= ata_bmdma_status,
	.qc_prep		= ata_qc_prep,
	.qc_issue		= ata_qc_issue_prot,
	.data_xfer		= ata_pio_data_xfer,

	.eng_timeout		= ata_eng_timeout,

	.irq_handler		= ata_interrupt,
	.irq_clear		= ata_bmdma_irq_clear,

	.port_start		= ata_port_start,
	.port_stop		= ata_port_stop,
	.host_stop		= ata_host_stop,
};


/**
 *	efar_init_one - Register EFAR ATA PCI device with kernel services
 *	@pdev: PCI device to register
 *	@ent: Entry in efar_pci_tbl matching with @pdev
 *
 *	Called from kernel PCI layer.
 *
 *	LOCKING:
 *	Inherited from PCI layer (may sleep).
 *
 *	RETURNS:
 *	Zero on success, or -ERRNO value.
 */

static int efar_init_one (struct pci_dev *pdev, const struct pci_device_id *ent)
{
	static int printed_version;
	static struct ata_port_info info = {
		.sht		= &efar_sht,
		.flags		= ATA_FLAG_SLAVE_POSS | ATA_FLAG_SRST,
		.pio_mask	= 0x1f,	/* pio0-4 */
		.mwdma_mask	= 0x07, /* mwdma1-2 */
		.udma_mask 	= 0x0f, /* UDMA 66 */
		.port_ops	= &efar_ops,
	};
	static struct ata_port_info *port_info[2] = { &info, &info };

	if (!printed_version++)
		dev_printk(KERN_DEBUG, &pdev->dev,
			   "version " DRV_VERSION "\n");

	return ata_pci_init_one(pdev, port_info, 2);
}

static const struct pci_device_id efar_pci_tbl[] = {
	{ 0x1055, 0x9130, PCI_ANY_ID, PCI_ANY_ID, },
	{ }	/* terminate list */
};

static struct pci_driver efar_pci_driver = {
	.name			= DRV_NAME,
	.id_table		= efar_pci_tbl,
	.probe			= efar_init_one,
	.remove			= ata_pci_remove_one,
};

static int __init efar_init(void)
{
	return pci_register_driver(&efar_pci_driver);
}

static void __exit efar_exit(void)
{
	pci_unregister_driver(&efar_pci_driver);
}


module_init(efar_init);
module_exit(efar_exit);

MODULE_AUTHOR("Alan Cox");
MODULE_DESCRIPTION("SCSI low-level driver for EFAR PIIX clones");
MODULE_LICENSE("GPL");
MODULE_DEVICE_TABLE(pci, efar_pci_tbl);
MODULE_VERSION(DRV_VERSION);

