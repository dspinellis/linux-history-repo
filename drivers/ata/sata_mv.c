/*
 * sata_mv.c - Marvell SATA support
 *
 * Copyright 2008: Marvell Corporation, all rights reserved.
 * Copyright 2005: EMC Corporation, all rights reserved.
 * Copyright 2005 Red Hat, Inc.  All rights reserved.
 *
 * Please ALWAYS copy linux-ide@vger.kernel.org on emails.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

/*
  sata_mv TODO list:

  1) Needs a full errata audit for all chipsets.  I implemented most
  of the errata workarounds found in the Marvell vendor driver, but
  I distinctly remember a couple workarounds (one related to PCI-X)
  are still needed.

  2) Improve/fix IRQ and error handling sequences.

  3) ATAPI support (Marvell claims the 60xx/70xx chips can do it).

  4) Think about TCQ support here, and for libata in general
  with controllers that suppport it via host-queuing hardware
  (a software-only implementation could be a nightmare).

  5) Investigate problems with PCI Message Signalled Interrupts (MSI).

  6) Cache frequently-accessed registers in mv_port_priv to reduce overhead.

  7) Fix/reenable hot plug/unplug (should happen as a side-effect of (2) above).

  8) Develop a low-power-consumption strategy, and implement it.

  9) [Experiment, low priority] See if ATAPI can be supported using
  "unknown FIS" or "vendor-specific FIS" support, or something creative
  like that.

  10) [Experiment, low priority] Investigate interrupt coalescing.
  Quite often, especially with PCI Message Signalled Interrupts (MSI),
  the overhead reduced by interrupt mitigation is quite often not
  worth the latency cost.

  11) [Experiment, Marvell value added] Is it possible to use target
  mode to cross-connect two Linux boxes with Marvell cards?  If so,
  creating LibATA target mode support would be very interesting.

  Target mode, for those without docs, is the ability to directly
  connect two SATA controllers.

*/

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/dmapool.h>
#include <linux/dma-mapping.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/ata_platform.h>
#include <linux/mbus.h>
#include <scsi/scsi_host.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_device.h>
#include <linux/libata.h>

#define DRV_NAME	"sata_mv"
#define DRV_VERSION	"1.20"

enum {
	/* BAR's are enumerated in terms of pci_resource_start() terms */
	MV_PRIMARY_BAR		= 0,	/* offset 0x10: memory space */
	MV_IO_BAR		= 2,	/* offset 0x18: IO space */
	MV_MISC_BAR		= 3,	/* offset 0x1c: FLASH, NVRAM, SRAM */

	MV_MAJOR_REG_AREA_SZ	= 0x10000,	/* 64KB */
	MV_MINOR_REG_AREA_SZ	= 0x2000,	/* 8KB */

	MV_PCI_REG_BASE		= 0,
	MV_IRQ_COAL_REG_BASE	= 0x18000,	/* 6xxx part only */
	MV_IRQ_COAL_CAUSE		= (MV_IRQ_COAL_REG_BASE + 0x08),
	MV_IRQ_COAL_CAUSE_LO		= (MV_IRQ_COAL_REG_BASE + 0x88),
	MV_IRQ_COAL_CAUSE_HI		= (MV_IRQ_COAL_REG_BASE + 0x8c),
	MV_IRQ_COAL_THRESHOLD		= (MV_IRQ_COAL_REG_BASE + 0xcc),
	MV_IRQ_COAL_TIME_THRESHOLD	= (MV_IRQ_COAL_REG_BASE + 0xd0),

	MV_SATAHC0_REG_BASE	= 0x20000,
	MV_FLASH_CTL		= 0x1046c,
	MV_GPIO_PORT_CTL	= 0x104f0,
	MV_RESET_CFG		= 0x180d8,

	MV_PCI_REG_SZ		= MV_MAJOR_REG_AREA_SZ,
	MV_SATAHC_REG_SZ	= MV_MAJOR_REG_AREA_SZ,
	MV_SATAHC_ARBTR_REG_SZ	= MV_MINOR_REG_AREA_SZ,		/* arbiter */
	MV_PORT_REG_SZ		= MV_MINOR_REG_AREA_SZ,

	MV_MAX_Q_DEPTH		= 32,
	MV_MAX_Q_DEPTH_MASK	= MV_MAX_Q_DEPTH - 1,

	/* CRQB needs alignment on a 1KB boundary. Size == 1KB
	 * CRPB needs alignment on a 256B boundary. Size == 256B
	 * ePRD (SG) entries need alignment on a 16B boundary. Size == 16B
	 */
	MV_CRQB_Q_SZ		= (32 * MV_MAX_Q_DEPTH),
	MV_CRPB_Q_SZ		= (8 * MV_MAX_Q_DEPTH),
	MV_MAX_SG_CT		= 256,
	MV_SG_TBL_SZ		= (16 * MV_MAX_SG_CT),

	/* Determine hc from 0-7 port: hc = port >> MV_PORT_HC_SHIFT */
	MV_PORT_HC_SHIFT	= 2,
	MV_PORTS_PER_HC		= (1 << MV_PORT_HC_SHIFT), /* 4 */
	/* Determine hc port from 0-7 port: hardport = port & MV_PORT_MASK */
	MV_PORT_MASK		= (MV_PORTS_PER_HC - 1),   /* 3 */

	/* Host Flags */
	MV_FLAG_DUAL_HC		= (1 << 30),  /* two SATA Host Controllers */
	MV_FLAG_IRQ_COALESCE	= (1 << 29),  /* IRQ coalescing capability */
	/* SoC integrated controllers, no PCI interface */
	MV_FLAG_SOC		= (1 << 28),

	MV_COMMON_FLAGS		= ATA_FLAG_SATA | ATA_FLAG_NO_LEGACY |
				  ATA_FLAG_MMIO | ATA_FLAG_NO_ATAPI |
				  ATA_FLAG_PIO_POLLING,
	MV_6XXX_FLAGS		= MV_FLAG_IRQ_COALESCE,

	CRQB_FLAG_READ		= (1 << 0),
	CRQB_TAG_SHIFT		= 1,
	CRQB_IOID_SHIFT		= 6,	/* CRQB Gen-II/IIE IO Id shift */
	CRQB_PMP_SHIFT		= 12,	/* CRQB Gen-II/IIE PMP shift */
	CRQB_HOSTQ_SHIFT	= 17,	/* CRQB Gen-II/IIE HostQueTag shift */
	CRQB_CMD_ADDR_SHIFT	= 8,
	CRQB_CMD_CS		= (0x2 << 11),
	CRQB_CMD_LAST		= (1 << 15),

	CRPB_FLAG_STATUS_SHIFT	= 8,
	CRPB_IOID_SHIFT_6	= 5,	/* CRPB Gen-II IO Id shift */
	CRPB_IOID_SHIFT_7	= 7,	/* CRPB Gen-IIE IO Id shift */

	EPRD_FLAG_END_OF_TBL	= (1 << 31),

	/* PCI interface registers */

	PCI_COMMAND_OFS		= 0xc00,

	PCI_MAIN_CMD_STS_OFS	= 0xd30,
	STOP_PCI_MASTER		= (1 << 2),
	PCI_MASTER_EMPTY	= (1 << 3),
	GLOB_SFT_RST		= (1 << 4),

	MV_PCI_MODE		= 0xd00,
	MV_PCI_EXP_ROM_BAR_CTL	= 0xd2c,
	MV_PCI_DISC_TIMER	= 0xd04,
	MV_PCI_MSI_TRIGGER	= 0xc38,
	MV_PCI_SERR_MASK	= 0xc28,
	MV_PCI_XBAR_TMOUT	= 0x1d04,
	MV_PCI_ERR_LOW_ADDRESS	= 0x1d40,
	MV_PCI_ERR_HIGH_ADDRESS	= 0x1d44,
	MV_PCI_ERR_ATTRIBUTE	= 0x1d48,
	MV_PCI_ERR_COMMAND	= 0x1d50,

	PCI_IRQ_CAUSE_OFS	= 0x1d58,
	PCI_IRQ_MASK_OFS	= 0x1d5c,
	PCI_UNMASK_ALL_IRQS	= 0x7fffff,	/* bits 22-0 */

	PCIE_IRQ_CAUSE_OFS	= 0x1900,
	PCIE_IRQ_MASK_OFS	= 0x1910,
	PCIE_UNMASK_ALL_IRQS	= 0x40a,	/* assorted bits */

	HC_MAIN_IRQ_CAUSE_OFS	= 0x1d60,
	HC_MAIN_IRQ_MASK_OFS	= 0x1d64,
	HC_SOC_MAIN_IRQ_CAUSE_OFS = 0x20020,
	HC_SOC_MAIN_IRQ_MASK_OFS = 0x20024,
	ERR_IRQ			= (1 << 0),	/* shift by port # */
	DONE_IRQ		= (1 << 1),	/* shift by port # */
	HC0_IRQ_PEND		= 0x1ff,	/* bits 0-8 = HC0's ports */
	HC_SHIFT		= 9,		/* bits 9-17 = HC1's ports */
	PCI_ERR			= (1 << 18),
	TRAN_LO_DONE		= (1 << 19),	/* 6xxx: IRQ coalescing */
	TRAN_HI_DONE		= (1 << 20),	/* 6xxx: IRQ coalescing */
	PORTS_0_3_COAL_DONE	= (1 << 8),
	PORTS_4_7_COAL_DONE	= (1 << 17),
	PORTS_0_7_COAL_DONE	= (1 << 21),	/* 6xxx: IRQ coalescing */
	GPIO_INT		= (1 << 22),
	SELF_INT		= (1 << 23),
	TWSI_INT		= (1 << 24),
	HC_MAIN_RSVD		= (0x7f << 25),	/* bits 31-25 */
	HC_MAIN_RSVD_5		= (0x1fff << 19), /* bits 31-19 */
	HC_MAIN_RSVD_SOC	= (0x3fffffb << 6),     /* bits 31-9, 7-6 */
	HC_MAIN_MASKED_IRQS	= (TRAN_LO_DONE | TRAN_HI_DONE |
				   PORTS_0_3_COAL_DONE | PORTS_4_7_COAL_DONE |
				   PORTS_0_7_COAL_DONE | GPIO_INT | TWSI_INT |
				   HC_MAIN_RSVD),
	HC_MAIN_MASKED_IRQS_5	= (PORTS_0_3_COAL_DONE | PORTS_4_7_COAL_DONE |
				   HC_MAIN_RSVD_5),
	HC_MAIN_MASKED_IRQS_SOC = (PORTS_0_3_COAL_DONE | HC_MAIN_RSVD_SOC),

	/* SATAHC registers */
	HC_CFG_OFS		= 0,

	HC_IRQ_CAUSE_OFS	= 0x14,
	DMA_IRQ			= (1 << 0),	/* shift by port # */
	HC_COAL_IRQ		= (1 << 4),	/* IRQ coalescing */
	DEV_IRQ			= (1 << 8),	/* shift by port # */

	/* Shadow block registers */
	SHD_BLK_OFS		= 0x100,
	SHD_CTL_AST_OFS		= 0x20,		/* ofs from SHD_BLK_OFS */

	/* SATA registers */
	SATA_STATUS_OFS		= 0x300,  /* ctrl, err regs follow status */
	SATA_ACTIVE_OFS		= 0x350,
	SATA_FIS_IRQ_CAUSE_OFS	= 0x364,

	LTMODE_OFS		= 0x30c,
	LTMODE_BIT8		= (1 << 8),	/* unknown, but necessary */

	PHY_MODE3		= 0x310,
	PHY_MODE4		= 0x314,
	PHY_MODE2		= 0x330,
	SATA_IFCTL_OFS		= 0x344,
	SATA_IFSTAT_OFS		= 0x34c,
	VENDOR_UNIQUE_FIS_OFS	= 0x35c,

	FIS_CFG_OFS		= 0x360,
	FIS_CFG_SINGLE_SYNC	= (1 << 16),	/* SYNC on DMA activation */

	MV5_PHY_MODE		= 0x74,
	MV5_LT_MODE		= 0x30,
	MV5_PHY_CTL		= 0x0C,
	SATA_INTERFACE_CFG	= 0x050,

	MV_M2_PREAMP_MASK	= 0x7e0,

	/* Port registers */
	EDMA_CFG_OFS		= 0,
	EDMA_CFG_Q_DEPTH	= 0x1f,		/* max device queue depth */
	EDMA_CFG_NCQ		= (1 << 5),	/* for R/W FPDMA queued */
	EDMA_CFG_NCQ_GO_ON_ERR	= (1 << 14),	/* continue on error */
	EDMA_CFG_RD_BRST_EXT	= (1 << 11),	/* read burst 512B */
	EDMA_CFG_WR_BUFF_LEN	= (1 << 13),	/* write buffer 512B */
	EDMA_CFG_EDMA_FBS	= (1 << 16),	/* EDMA FIS-Based Switching */
	EDMA_CFG_FBS		= (1 << 26),	/* FIS-Based Switching */

	EDMA_ERR_IRQ_CAUSE_OFS	= 0x8,
	EDMA_ERR_IRQ_MASK_OFS	= 0xc,
	EDMA_ERR_D_PAR		= (1 << 0),	/* UDMA data parity err */
	EDMA_ERR_PRD_PAR	= (1 << 1),	/* UDMA PRD parity err */
	EDMA_ERR_DEV		= (1 << 2),	/* device error */
	EDMA_ERR_DEV_DCON	= (1 << 3),	/* device disconnect */
	EDMA_ERR_DEV_CON	= (1 << 4),	/* device connected */
	EDMA_ERR_SERR		= (1 << 5),	/* SError bits [WBDST] raised */
	EDMA_ERR_SELF_DIS	= (1 << 7),	/* Gen II/IIE self-disable */
	EDMA_ERR_SELF_DIS_5	= (1 << 8),	/* Gen I self-disable */
	EDMA_ERR_BIST_ASYNC	= (1 << 8),	/* BIST FIS or Async Notify */
	EDMA_ERR_TRANS_IRQ_7	= (1 << 8),	/* Gen IIE transprt layer irq */
	EDMA_ERR_CRQB_PAR	= (1 << 9),	/* CRQB parity error */
	EDMA_ERR_CRPB_PAR	= (1 << 10),	/* CRPB parity error */
	EDMA_ERR_INTRL_PAR	= (1 << 11),	/* internal parity error */
	EDMA_ERR_IORDY		= (1 << 12),	/* IORdy timeout */

	EDMA_ERR_LNK_CTRL_RX	= (0xf << 13),	/* link ctrl rx error */
	EDMA_ERR_LNK_CTRL_RX_0	= (1 << 13),	/* transient: CRC err */
	EDMA_ERR_LNK_CTRL_RX_1	= (1 << 14),	/* transient: FIFO err */
	EDMA_ERR_LNK_CTRL_RX_2	= (1 << 15),	/* fatal: caught SYNC */
	EDMA_ERR_LNK_CTRL_RX_3	= (1 << 16),	/* transient: FIS rx err */

	EDMA_ERR_LNK_DATA_RX	= (0xf << 17),	/* link data rx error */

	EDMA_ERR_LNK_CTRL_TX	= (0x1f << 21),	/* link ctrl tx error */
	EDMA_ERR_LNK_CTRL_TX_0	= (1 << 21),	/* transient: CRC err */
	EDMA_ERR_LNK_CTRL_TX_1	= (1 << 22),	/* transient: FIFO err */
	EDMA_ERR_LNK_CTRL_TX_2	= (1 << 23),	/* transient: caught SYNC */
	EDMA_ERR_LNK_CTRL_TX_3	= (1 << 24),	/* transient: caught DMAT */
	EDMA_ERR_LNK_CTRL_TX_4	= (1 << 25),	/* transient: FIS collision */

	EDMA_ERR_LNK_DATA_TX	= (0x1f << 26),	/* link data tx error */

	EDMA_ERR_TRANS_PROTO	= (1 << 31),	/* transport protocol error */
	EDMA_ERR_OVERRUN_5	= (1 << 5),
	EDMA_ERR_UNDERRUN_5	= (1 << 6),

	EDMA_ERR_IRQ_TRANSIENT  = EDMA_ERR_LNK_CTRL_RX_0 |
				  EDMA_ERR_LNK_CTRL_RX_1 |
				  EDMA_ERR_LNK_CTRL_RX_3 |
				  EDMA_ERR_LNK_CTRL_TX |
				 /* temporary, until we fix hotplug: */
				 (EDMA_ERR_DEV_DCON | EDMA_ERR_DEV_CON),

	EDMA_EH_FREEZE		= EDMA_ERR_D_PAR |
				  EDMA_ERR_PRD_PAR |
				  EDMA_ERR_DEV_DCON |
				  EDMA_ERR_DEV_CON |
				  EDMA_ERR_SERR |
				  EDMA_ERR_SELF_DIS |
				  EDMA_ERR_CRQB_PAR |
				  EDMA_ERR_CRPB_PAR |
				  EDMA_ERR_INTRL_PAR |
				  EDMA_ERR_IORDY |
				  EDMA_ERR_LNK_CTRL_RX_2 |
				  EDMA_ERR_LNK_DATA_RX |
				  EDMA_ERR_LNK_DATA_TX |
				  EDMA_ERR_TRANS_PROTO,

	EDMA_EH_FREEZE_5	= EDMA_ERR_D_PAR |
				  EDMA_ERR_PRD_PAR |
				  EDMA_ERR_DEV_DCON |
				  EDMA_ERR_DEV_CON |
				  EDMA_ERR_OVERRUN_5 |
				  EDMA_ERR_UNDERRUN_5 |
				  EDMA_ERR_SELF_DIS_5 |
				  EDMA_ERR_CRQB_PAR |
				  EDMA_ERR_CRPB_PAR |
				  EDMA_ERR_INTRL_PAR |
				  EDMA_ERR_IORDY,

	EDMA_REQ_Q_BASE_HI_OFS	= 0x10,
	EDMA_REQ_Q_IN_PTR_OFS	= 0x14,		/* also contains BASE_LO */

	EDMA_REQ_Q_OUT_PTR_OFS	= 0x18,
	EDMA_REQ_Q_PTR_SHIFT	= 5,

	EDMA_RSP_Q_BASE_HI_OFS	= 0x1c,
	EDMA_RSP_Q_IN_PTR_OFS	= 0x20,
	EDMA_RSP_Q_OUT_PTR_OFS	= 0x24,		/* also contains BASE_LO */
	EDMA_RSP_Q_PTR_SHIFT	= 3,

	EDMA_CMD_OFS		= 0x28,		/* EDMA command register */
	EDMA_EN			= (1 << 0),	/* enable EDMA */
	EDMA_DS			= (1 << 1),	/* disable EDMA; self-negated */
	ATA_RST			= (1 << 2),	/* reset trans/link/phy */

	EDMA_IORDY_TMOUT	= 0x34,
	EDMA_ARB_CFG		= 0x38,

	GEN_II_NCQ_MAX_SECTORS	= 256,		/* max sects/io on Gen2 w/NCQ */

	/* Host private flags (hp_flags) */
	MV_HP_FLAG_MSI		= (1 << 0),
	MV_HP_ERRATA_50XXB0	= (1 << 1),
	MV_HP_ERRATA_50XXB2	= (1 << 2),
	MV_HP_ERRATA_60X1B2	= (1 << 3),
	MV_HP_ERRATA_60X1C0	= (1 << 4),
	MV_HP_ERRATA_XX42A0	= (1 << 5),
	MV_HP_GEN_I		= (1 << 6),	/* Generation I: 50xx */
	MV_HP_GEN_II		= (1 << 7),	/* Generation II: 60xx */
	MV_HP_GEN_IIE		= (1 << 8),	/* Generation IIE: 6042/7042 */
	MV_HP_PCIE		= (1 << 9),	/* PCIe bus/regs: 7042 */

	/* Port private flags (pp_flags) */
	MV_PP_FLAG_EDMA_EN	= (1 << 0),	/* is EDMA engine enabled? */
	MV_PP_FLAG_NCQ_EN	= (1 << 1),	/* is EDMA set up for NCQ? */
};

#define IS_GEN_I(hpriv) ((hpriv)->hp_flags & MV_HP_GEN_I)
#define IS_GEN_II(hpriv) ((hpriv)->hp_flags & MV_HP_GEN_II)
#define IS_GEN_IIE(hpriv) ((hpriv)->hp_flags & MV_HP_GEN_IIE)
#define HAS_PCI(host) (!((host)->ports[0]->flags & MV_FLAG_SOC))

#define WINDOW_CTRL(i)		(0x20030 + ((i) << 4))
#define WINDOW_BASE(i)		(0x20034 + ((i) << 4))

enum {
	/* DMA boundary 0xffff is required by the s/g splitting
	 * we need on /length/ in mv_fill-sg().
	 */
	MV_DMA_BOUNDARY		= 0xffffU,

	/* mask of register bits containing lower 32 bits
	 * of EDMA request queue DMA address
	 */
	EDMA_REQ_Q_BASE_LO_MASK	= 0xfffffc00U,

	/* ditto, for response queue */
	EDMA_RSP_Q_BASE_LO_MASK	= 0xffffff00U,
};

enum chip_type {
	chip_504x,
	chip_508x,
	chip_5080,
	chip_604x,
	chip_608x,
	chip_6042,
	chip_7042,
	chip_soc,
};

/* Command ReQuest Block: 32B */
struct mv_crqb {
	__le32			sg_addr;
	__le32			sg_addr_hi;
	__le16			ctrl_flags;
	__le16			ata_cmd[11];
};

struct mv_crqb_iie {
	__le32			addr;
	__le32			addr_hi;
	__le32			flags;
	__le32			len;
	__le32			ata_cmd[4];
};

/* Command ResPonse Block: 8B */
struct mv_crpb {
	__le16			id;
	__le16			flags;
	__le32			tmstmp;
};

/* EDMA Physical Region Descriptor (ePRD); A.K.A. SG */
struct mv_sg {
	__le32			addr;
	__le32			flags_size;
	__le32			addr_hi;
	__le32			reserved;
};

struct mv_port_priv {
	struct mv_crqb		*crqb;
	dma_addr_t		crqb_dma;
	struct mv_crpb		*crpb;
	dma_addr_t		crpb_dma;
	struct mv_sg		*sg_tbl[MV_MAX_Q_DEPTH];
	dma_addr_t		sg_tbl_dma[MV_MAX_Q_DEPTH];

	unsigned int		req_idx;
	unsigned int		resp_idx;

	u32			pp_flags;
};

struct mv_port_signal {
	u32			amps;
	u32			pre;
};

struct mv_host_priv {
	u32			hp_flags;
	struct mv_port_signal	signal[8];
	const struct mv_hw_ops	*ops;
	int			n_ports;
	void __iomem		*base;
	void __iomem		*main_cause_reg_addr;
	void __iomem		*main_mask_reg_addr;
	u32			irq_cause_ofs;
	u32			irq_mask_ofs;
	u32			unmask_all_irqs;
	/*
	 * These consistent DMA memory pools give us guaranteed
	 * alignment for hardware-accessed data structures,
	 * and less memory waste in accomplishing the alignment.
	 */
	struct dma_pool		*crqb_pool;
	struct dma_pool		*crpb_pool;
	struct dma_pool		*sg_tbl_pool;
};

struct mv_hw_ops {
	void (*phy_errata)(struct mv_host_priv *hpriv, void __iomem *mmio,
			   unsigned int port);
	void (*enable_leds)(struct mv_host_priv *hpriv, void __iomem *mmio);
	void (*read_preamp)(struct mv_host_priv *hpriv, int idx,
			   void __iomem *mmio);
	int (*reset_hc)(struct mv_host_priv *hpriv, void __iomem *mmio,
			unsigned int n_hc);
	void (*reset_flash)(struct mv_host_priv *hpriv, void __iomem *mmio);
	void (*reset_bus)(struct ata_host *host, void __iomem *mmio);
};

static int mv_scr_read(struct ata_port *ap, unsigned int sc_reg_in, u32 *val);
static int mv_scr_write(struct ata_port *ap, unsigned int sc_reg_in, u32 val);
static int mv5_scr_read(struct ata_port *ap, unsigned int sc_reg_in, u32 *val);
static int mv5_scr_write(struct ata_port *ap, unsigned int sc_reg_in, u32 val);
static int mv_port_start(struct ata_port *ap);
static void mv_port_stop(struct ata_port *ap);
static void mv_qc_prep(struct ata_queued_cmd *qc);
static void mv_qc_prep_iie(struct ata_queued_cmd *qc);
static unsigned int mv_qc_issue(struct ata_queued_cmd *qc);
static int mv_hardreset(struct ata_link *link, unsigned int *class,
			unsigned long deadline);
static void mv_eh_freeze(struct ata_port *ap);
static void mv_eh_thaw(struct ata_port *ap);
static void mv6_dev_config(struct ata_device *dev);

static void mv5_phy_errata(struct mv_host_priv *hpriv, void __iomem *mmio,
			   unsigned int port);
static void mv5_enable_leds(struct mv_host_priv *hpriv, void __iomem *mmio);
static void mv5_read_preamp(struct mv_host_priv *hpriv, int idx,
			   void __iomem *mmio);
static int mv5_reset_hc(struct mv_host_priv *hpriv, void __iomem *mmio,
			unsigned int n_hc);
static void mv5_reset_flash(struct mv_host_priv *hpriv, void __iomem *mmio);
static void mv5_reset_bus(struct ata_host *host, void __iomem *mmio);

static void mv6_phy_errata(struct mv_host_priv *hpriv, void __iomem *mmio,
			   unsigned int port);
static void mv6_enable_leds(struct mv_host_priv *hpriv, void __iomem *mmio);
static void mv6_read_preamp(struct mv_host_priv *hpriv, int idx,
			   void __iomem *mmio);
static int mv6_reset_hc(struct mv_host_priv *hpriv, void __iomem *mmio,
			unsigned int n_hc);
static void mv6_reset_flash(struct mv_host_priv *hpriv, void __iomem *mmio);
static void mv_soc_enable_leds(struct mv_host_priv *hpriv,
				      void __iomem *mmio);
static void mv_soc_read_preamp(struct mv_host_priv *hpriv, int idx,
				      void __iomem *mmio);
static int mv_soc_reset_hc(struct mv_host_priv *hpriv,
				  void __iomem *mmio, unsigned int n_hc);
static void mv_soc_reset_flash(struct mv_host_priv *hpriv,
				      void __iomem *mmio);
static void mv_soc_reset_bus(struct ata_host *host, void __iomem *mmio);
static void mv_reset_pci_bus(struct ata_host *host, void __iomem *mmio);
static void mv_reset_channel(struct mv_host_priv *hpriv, void __iomem *mmio,
			     unsigned int port_no);
static int mv_stop_edma(struct ata_port *ap);
static int mv_stop_edma_engine(void __iomem *port_mmio);
static void mv_edma_cfg(struct ata_port *ap, int want_ncq);

static void mv_pmp_select(struct ata_port *ap, int pmp);
static int mv_pmp_hardreset(struct ata_link *link, unsigned int *class,
				unsigned long deadline);
static int  mv_softreset(struct ata_link *link, unsigned int *class,
				unsigned long deadline);

/* .sg_tablesize is (MV_MAX_SG_CT / 2) in the structures below
 * because we have to allow room for worst case splitting of
 * PRDs for 64K boundaries in mv_fill_sg().
 */
static struct scsi_host_template mv5_sht = {
	ATA_BASE_SHT(DRV_NAME),
	.sg_tablesize		= MV_MAX_SG_CT / 2,
	.dma_boundary		= MV_DMA_BOUNDARY,
};

static struct scsi_host_template mv6_sht = {
	ATA_NCQ_SHT(DRV_NAME),
	.can_queue		= MV_MAX_Q_DEPTH - 1,
	.sg_tablesize		= MV_MAX_SG_CT / 2,
	.dma_boundary		= MV_DMA_BOUNDARY,
};

static struct ata_port_operations mv5_ops = {
	.inherits		= &ata_sff_port_ops,

	.qc_prep		= mv_qc_prep,
	.qc_issue		= mv_qc_issue,

	.freeze			= mv_eh_freeze,
	.thaw			= mv_eh_thaw,
	.hardreset		= mv_hardreset,
	.error_handler		= ata_std_error_handler, /* avoid SFF EH */
	.post_internal_cmd	= ATA_OP_NULL,

	.scr_read		= mv5_scr_read,
	.scr_write		= mv5_scr_write,

	.port_start		= mv_port_start,
	.port_stop		= mv_port_stop,
};

static struct ata_port_operations mv6_ops = {
	.inherits		= &mv5_ops,
	.qc_defer		= sata_pmp_qc_defer_cmd_switch,
	.dev_config             = mv6_dev_config,
	.scr_read		= mv_scr_read,
	.scr_write		= mv_scr_write,

	.pmp_hardreset		= mv_pmp_hardreset,
	.pmp_softreset		= mv_softreset,
	.softreset		= mv_softreset,
	.error_handler		= sata_pmp_error_handler,
};

static struct ata_port_operations mv_iie_ops = {
	.inherits		= &mv6_ops,
	.qc_defer		= ata_std_qc_defer, /* FIS-based switching */
	.dev_config		= ATA_OP_NULL,
	.qc_prep		= mv_qc_prep_iie,
};

static const struct ata_port_info mv_port_info[] = {
	{  /* chip_504x */
		.flags		= MV_COMMON_FLAGS,
		.pio_mask	= 0x1f,	/* pio0-4 */
		.udma_mask	= ATA_UDMA6,
		.port_ops	= &mv5_ops,
	},
	{  /* chip_508x */
		.flags		= MV_COMMON_FLAGS | MV_FLAG_DUAL_HC,
		.pio_mask	= 0x1f,	/* pio0-4 */
		.udma_mask	= ATA_UDMA6,
		.port_ops	= &mv5_ops,
	},
	{  /* chip_5080 */
		.flags		= MV_COMMON_FLAGS | MV_FLAG_DUAL_HC,
		.pio_mask	= 0x1f,	/* pio0-4 */
		.udma_mask	= ATA_UDMA6,
		.port_ops	= &mv5_ops,
	},
	{  /* chip_604x */
		.flags		= MV_COMMON_FLAGS | MV_6XXX_FLAGS |
				  ATA_FLAG_PMP | ATA_FLAG_ACPI_SATA |
				  ATA_FLAG_NCQ,
		.pio_mask	= 0x1f,	/* pio0-4 */
		.udma_mask	= ATA_UDMA6,
		.port_ops	= &mv6_ops,
	},
	{  /* chip_608x */
		.flags		= MV_COMMON_FLAGS | MV_6XXX_FLAGS |
				  ATA_FLAG_PMP | ATA_FLAG_ACPI_SATA |
				  ATA_FLAG_NCQ | MV_FLAG_DUAL_HC,
		.pio_mask	= 0x1f,	/* pio0-4 */
		.udma_mask	= ATA_UDMA6,
		.port_ops	= &mv6_ops,
	},
	{  /* chip_6042 */
		.flags		= MV_COMMON_FLAGS | MV_6XXX_FLAGS |
				  ATA_FLAG_PMP | ATA_FLAG_ACPI_SATA |
				  ATA_FLAG_NCQ,
		.pio_mask	= 0x1f,	/* pio0-4 */
		.udma_mask	= ATA_UDMA6,
		.port_ops	= &mv_iie_ops,
	},
	{  /* chip_7042 */
		.flags		= MV_COMMON_FLAGS | MV_6XXX_FLAGS |
				  ATA_FLAG_PMP | ATA_FLAG_ACPI_SATA |
				  ATA_FLAG_NCQ,
		.pio_mask	= 0x1f,	/* pio0-4 */
		.udma_mask	= ATA_UDMA6,
		.port_ops	= &mv_iie_ops,
	},
	{  /* chip_soc */
		.flags		= MV_COMMON_FLAGS | MV_6XXX_FLAGS |
				  ATA_FLAG_PMP | ATA_FLAG_ACPI_SATA |
				  ATA_FLAG_NCQ | MV_FLAG_SOC,
		.pio_mask	= 0x1f,	/* pio0-4 */
		.udma_mask	= ATA_UDMA6,
		.port_ops	= &mv_iie_ops,
	},
};

static const struct pci_device_id mv_pci_tbl[] = {
	{ PCI_VDEVICE(MARVELL, 0x5040), chip_504x },
	{ PCI_VDEVICE(MARVELL, 0x5041), chip_504x },
	{ PCI_VDEVICE(MARVELL, 0x5080), chip_5080 },
	{ PCI_VDEVICE(MARVELL, 0x5081), chip_508x },
	/* RocketRAID 1740/174x have different identifiers */
	{ PCI_VDEVICE(TTI, 0x1740), chip_508x },
	{ PCI_VDEVICE(TTI, 0x1742), chip_508x },

	{ PCI_VDEVICE(MARVELL, 0x6040), chip_604x },
	{ PCI_VDEVICE(MARVELL, 0x6041), chip_604x },
	{ PCI_VDEVICE(MARVELL, 0x6042), chip_6042 },
	{ PCI_VDEVICE(MARVELL, 0x6080), chip_608x },
	{ PCI_VDEVICE(MARVELL, 0x6081), chip_608x },

	{ PCI_VDEVICE(ADAPTEC2, 0x0241), chip_604x },

	/* Adaptec 1430SA */
	{ PCI_VDEVICE(ADAPTEC2, 0x0243), chip_7042 },

	/* Marvell 7042 support */
	{ PCI_VDEVICE(MARVELL, 0x7042), chip_7042 },

	/* Highpoint RocketRAID PCIe series */
	{ PCI_VDEVICE(TTI, 0x2300), chip_7042 },
	{ PCI_VDEVICE(TTI, 0x2310), chip_7042 },

	{ }			/* terminate list */
};

static const struct mv_hw_ops mv5xxx_ops = {
	.phy_errata		= mv5_phy_errata,
	.enable_leds		= mv5_enable_leds,
	.read_preamp		= mv5_read_preamp,
	.reset_hc		= mv5_reset_hc,
	.reset_flash		= mv5_reset_flash,
	.reset_bus		= mv5_reset_bus,
};

static const struct mv_hw_ops mv6xxx_ops = {
	.phy_errata		= mv6_phy_errata,
	.enable_leds		= mv6_enable_leds,
	.read_preamp		= mv6_read_preamp,
	.reset_hc		= mv6_reset_hc,
	.reset_flash		= mv6_reset_flash,
	.reset_bus		= mv_reset_pci_bus,
};

static const struct mv_hw_ops mv_soc_ops = {
	.phy_errata		= mv6_phy_errata,
	.enable_leds		= mv_soc_enable_leds,
	.read_preamp		= mv_soc_read_preamp,
	.reset_hc		= mv_soc_reset_hc,
	.reset_flash		= mv_soc_reset_flash,
	.reset_bus		= mv_soc_reset_bus,
};

/*
 * Functions
 */

static inline void writelfl(unsigned long data, void __iomem *addr)
{
	writel(data, addr);
	(void) readl(addr);	/* flush to avoid PCI posted write */
}

static inline unsigned int mv_hc_from_port(unsigned int port)
{
	return port >> MV_PORT_HC_SHIFT;
}

static inline unsigned int mv_hardport_from_port(unsigned int port)
{
	return port & MV_PORT_MASK;
}

static inline void __iomem *mv_hc_base(void __iomem *base, unsigned int hc)
{
	return (base + MV_SATAHC0_REG_BASE + (hc * MV_SATAHC_REG_SZ));
}

static inline void __iomem *mv_hc_base_from_port(void __iomem *base,
						 unsigned int port)
{
	return mv_hc_base(base, mv_hc_from_port(port));
}

static inline void __iomem *mv_port_base(void __iomem *base, unsigned int port)
{
	return  mv_hc_base_from_port(base, port) +
		MV_SATAHC_ARBTR_REG_SZ +
		(mv_hardport_from_port(port) * MV_PORT_REG_SZ);
}

static void __iomem *mv5_phy_base(void __iomem *mmio, unsigned int port)
{
	void __iomem *hc_mmio = mv_hc_base_from_port(mmio, port);
	unsigned long ofs = (mv_hardport_from_port(port) + 1) * 0x100UL;

	return hc_mmio + ofs;
}

static inline void __iomem *mv_host_base(struct ata_host *host)
{
	struct mv_host_priv *hpriv = host->private_data;
	return hpriv->base;
}

static inline void __iomem *mv_ap_base(struct ata_port *ap)
{
	return mv_port_base(mv_host_base(ap->host), ap->port_no);
}

static inline int mv_get_hc_count(unsigned long port_flags)
{
	return ((port_flags & MV_FLAG_DUAL_HC) ? 2 : 1);
}

static void mv_set_edma_ptrs(void __iomem *port_mmio,
			     struct mv_host_priv *hpriv,
			     struct mv_port_priv *pp)
{
	u32 index;

	/*
	 * initialize request queue
	 */
	index = (pp->req_idx & MV_MAX_Q_DEPTH_MASK) << EDMA_REQ_Q_PTR_SHIFT;

	WARN_ON(pp->crqb_dma & 0x3ff);
	writel((pp->crqb_dma >> 16) >> 16, port_mmio + EDMA_REQ_Q_BASE_HI_OFS);
	writelfl((pp->crqb_dma & EDMA_REQ_Q_BASE_LO_MASK) | index,
		 port_mmio + EDMA_REQ_Q_IN_PTR_OFS);

	if (hpriv->hp_flags & MV_HP_ERRATA_XX42A0)
		writelfl((pp->crqb_dma & 0xffffffff) | index,
			 port_mmio + EDMA_REQ_Q_OUT_PTR_OFS);
	else
		writelfl(index, port_mmio + EDMA_REQ_Q_OUT_PTR_OFS);

	/*
	 * initialize response queue
	 */
	index = (pp->resp_idx & MV_MAX_Q_DEPTH_MASK) << EDMA_RSP_Q_PTR_SHIFT;

	WARN_ON(pp->crpb_dma & 0xff);
	writel((pp->crpb_dma >> 16) >> 16, port_mmio + EDMA_RSP_Q_BASE_HI_OFS);

	if (hpriv->hp_flags & MV_HP_ERRATA_XX42A0)
		writelfl((pp->crpb_dma & 0xffffffff) | index,
			 port_mmio + EDMA_RSP_Q_IN_PTR_OFS);
	else
		writelfl(index, port_mmio + EDMA_RSP_Q_IN_PTR_OFS);

	writelfl((pp->crpb_dma & EDMA_RSP_Q_BASE_LO_MASK) | index,
		 port_mmio + EDMA_RSP_Q_OUT_PTR_OFS);
}

/**
 *      mv_start_dma - Enable eDMA engine
 *      @base: port base address
 *      @pp: port private data
 *
 *      Verify the local cache of the eDMA state is accurate with a
 *      WARN_ON.
 *
 *      LOCKING:
 *      Inherited from caller.
 */
static void mv_start_dma(struct ata_port *ap, void __iomem *port_mmio,
			 struct mv_port_priv *pp, u8 protocol)
{
	int want_ncq = (protocol == ATA_PROT_NCQ);

	if (pp->pp_flags & MV_PP_FLAG_EDMA_EN) {
		int using_ncq = ((pp->pp_flags & MV_PP_FLAG_NCQ_EN) != 0);
		if (want_ncq != using_ncq)
			mv_stop_edma(ap);
	}
	if (!(pp->pp_flags & MV_PP_FLAG_EDMA_EN)) {
		struct mv_host_priv *hpriv = ap->host->private_data;
		int hardport = mv_hardport_from_port(ap->port_no);
		void __iomem *hc_mmio = mv_hc_base_from_port(
					mv_host_base(ap->host), hardport);
		u32 hc_irq_cause, ipending;

		/* clear EDMA event indicators, if any */
		writelfl(0, port_mmio + EDMA_ERR_IRQ_CAUSE_OFS);

		/* clear EDMA interrupt indicator, if any */
		hc_irq_cause = readl(hc_mmio + HC_IRQ_CAUSE_OFS);
		ipending = (DEV_IRQ | DMA_IRQ) << hardport;
		if (hc_irq_cause & ipending) {
			writelfl(hc_irq_cause & ~ipending,
				 hc_mmio + HC_IRQ_CAUSE_OFS);
		}

		mv_edma_cfg(ap, want_ncq);

		/* clear FIS IRQ Cause */
		writelfl(0, port_mmio + SATA_FIS_IRQ_CAUSE_OFS);

		mv_set_edma_ptrs(port_mmio, hpriv, pp);

		writelfl(EDMA_EN, port_mmio + EDMA_CMD_OFS);
		pp->pp_flags |= MV_PP_FLAG_EDMA_EN;
	}
}

/**
 *      mv_stop_edma_engine - Disable eDMA engine
 *      @port_mmio: io base address
 *
 *      LOCKING:
 *      Inherited from caller.
 */
static int mv_stop_edma_engine(void __iomem *port_mmio)
{
	int i;

	/* Disable eDMA.  The disable bit auto clears. */
	writelfl(EDMA_DS, port_mmio + EDMA_CMD_OFS);

	/* Wait for the chip to confirm eDMA is off. */
	for (i = 10000; i > 0; i--) {
		u32 reg = readl(port_mmio + EDMA_CMD_OFS);
		if (!(reg & EDMA_EN))
			return 0;
		udelay(10);
	}
	return -EIO;
}

static int mv_stop_edma(struct ata_port *ap)
{
	void __iomem *port_mmio = mv_ap_base(ap);
	struct mv_port_priv *pp = ap->private_data;

	if (!(pp->pp_flags & MV_PP_FLAG_EDMA_EN))
		return 0;
	pp->pp_flags &= ~MV_PP_FLAG_EDMA_EN;
	if (mv_stop_edma_engine(port_mmio)) {
		ata_port_printk(ap, KERN_ERR, "Unable to stop eDMA\n");
		return -EIO;
	}
	return 0;
}

#ifdef ATA_DEBUG
static void mv_dump_mem(void __iomem *start, unsigned bytes)
{
	int b, w;
	for (b = 0; b < bytes; ) {
		DPRINTK("%p: ", start + b);
		for (w = 0; b < bytes && w < 4; w++) {
			printk("%08x ", readl(start + b));
			b += sizeof(u32);
		}
		printk("\n");
	}
}
#endif

static void mv_dump_pci_cfg(struct pci_dev *pdev, unsigned bytes)
{
#ifdef ATA_DEBUG
	int b, w;
	u32 dw;
	for (b = 0; b < bytes; ) {
		DPRINTK("%02x: ", b);
		for (w = 0; b < bytes && w < 4; w++) {
			(void) pci_read_config_dword(pdev, b, &dw);
			printk("%08x ", dw);
			b += sizeof(u32);
		}
		printk("\n");
	}
#endif
}
static void mv_dump_all_regs(void __iomem *mmio_base, int port,
			     struct pci_dev *pdev)
{
#ifdef ATA_DEBUG
	void __iomem *hc_base = mv_hc_base(mmio_base,
					   port >> MV_PORT_HC_SHIFT);
	void __iomem *port_base;
	int start_port, num_ports, p, start_hc, num_hcs, hc;

	if (0 > port) {
		start_hc = start_port = 0;
		num_ports = 8;		/* shld be benign for 4 port devs */
		num_hcs = 2;
	} else {
		start_hc = port >> MV_PORT_HC_SHIFT;
		start_port = port;
		num_ports = num_hcs = 1;
	}
	DPRINTK("All registers for port(s) %u-%u:\n", start_port,
		num_ports > 1 ? num_ports - 1 : start_port);

	if (NULL != pdev) {
		DPRINTK("PCI config space regs:\n");
		mv_dump_pci_cfg(pdev, 0x68);
	}
	DPRINTK("PCI regs:\n");
	mv_dump_mem(mmio_base+0xc00, 0x3c);
	mv_dump_mem(mmio_base+0xd00, 0x34);
	mv_dump_mem(mmio_base+0xf00, 0x4);
	mv_dump_mem(mmio_base+0x1d00, 0x6c);
	for (hc = start_hc; hc < start_hc + num_hcs; hc++) {
		hc_base = mv_hc_base(mmio_base, hc);
		DPRINTK("HC regs (HC %i):\n", hc);
		mv_dump_mem(hc_base, 0x1c);
	}
	for (p = start_port; p < start_port + num_ports; p++) {
		port_base = mv_port_base(mmio_base, p);
		DPRINTK("EDMA regs (port %i):\n", p);
		mv_dump_mem(port_base, 0x54);
		DPRINTK("SATA regs (port %i):\n", p);
		mv_dump_mem(port_base+0x300, 0x60);
	}
#endif
}

static unsigned int mv_scr_offset(unsigned int sc_reg_in)
{
	unsigned int ofs;

	switch (sc_reg_in) {
	case SCR_STATUS:
	case SCR_CONTROL:
	case SCR_ERROR:
		ofs = SATA_STATUS_OFS + (sc_reg_in * sizeof(u32));
		break;
	case SCR_ACTIVE:
		ofs = SATA_ACTIVE_OFS;   /* active is not with the others */
		break;
	default:
		ofs = 0xffffffffU;
		break;
	}
	return ofs;
}

static int mv_scr_read(struct ata_port *ap, unsigned int sc_reg_in, u32 *val)
{
	unsigned int ofs = mv_scr_offset(sc_reg_in);

	if (ofs != 0xffffffffU) {
		*val = readl(mv_ap_base(ap) + ofs);
		return 0;
	} else
		return -EINVAL;
}

static int mv_scr_write(struct ata_port *ap, unsigned int sc_reg_in, u32 val)
{
	unsigned int ofs = mv_scr_offset(sc_reg_in);

	if (ofs != 0xffffffffU) {
		writelfl(val, mv_ap_base(ap) + ofs);
		return 0;
	} else
		return -EINVAL;
}

static void mv6_dev_config(struct ata_device *adev)
{
	/*
	 * Deal with Gen-II ("mv6") hardware quirks/restrictions:
	 *
	 * Gen-II does not support NCQ over a port multiplier
	 *  (no FIS-based switching).
	 *
	 * We don't have hob_nsect when doing NCQ commands on Gen-II.
	 * See mv_qc_prep() for more info.
	 */
	if (adev->flags & ATA_DFLAG_NCQ) {
		if (sata_pmp_attached(adev->link->ap)) {
			adev->flags &= ~ATA_DFLAG_NCQ;
			ata_dev_printk(adev, KERN_INFO,
				"NCQ disabled for command-based switching\n");
		} else if (adev->max_sectors > GEN_II_NCQ_MAX_SECTORS) {
			adev->max_sectors = GEN_II_NCQ_MAX_SECTORS;
			ata_dev_printk(adev, KERN_INFO,
				"max_sectors limited to %u for NCQ\n",
				adev->max_sectors);
		}
	}
}

static void mv_config_fbs(void __iomem *port_mmio, int enable_fbs)
{
	u32 old_fcfg, new_fcfg, old_ltmode, new_ltmode;
	/*
	 * Various bit settings required for operation
	 * in FIS-based switching (fbs) mode on GenIIe:
	 */
	old_fcfg   = readl(port_mmio + FIS_CFG_OFS);
	old_ltmode = readl(port_mmio + LTMODE_OFS);
	if (enable_fbs) {
		new_fcfg   = old_fcfg   |  FIS_CFG_SINGLE_SYNC;
		new_ltmode = old_ltmode |  LTMODE_BIT8;
	} else { /* disable fbs */
		new_fcfg   = old_fcfg   & ~FIS_CFG_SINGLE_SYNC;
		new_ltmode = old_ltmode & ~LTMODE_BIT8;
	}
	if (new_fcfg != old_fcfg)
		writelfl(new_fcfg, port_mmio + FIS_CFG_OFS);
	if (new_ltmode != old_ltmode)
		writelfl(new_ltmode, port_mmio + LTMODE_OFS);
}

static void mv_edma_cfg(struct ata_port *ap, int want_ncq)
{
	u32 cfg;
	struct mv_port_priv *pp    = ap->private_data;
	struct mv_host_priv *hpriv = ap->host->private_data;
	void __iomem *port_mmio    = mv_ap_base(ap);

	/* set up non-NCQ EDMA configuration */
	cfg = EDMA_CFG_Q_DEPTH;		/* always 0x1f for *all* chips */

	if (IS_GEN_I(hpriv))
		cfg |= (1 << 8);	/* enab config burst size mask */

	else if (IS_GEN_II(hpriv))
		cfg |= EDMA_CFG_RD_BRST_EXT | EDMA_CFG_WR_BUFF_LEN;

	else if (IS_GEN_IIE(hpriv)) {
		cfg |= (1 << 23);	/* do not mask PM field in rx'd FIS */
		cfg |= (1 << 22);	/* enab 4-entry host queue cache */
		cfg |= (1 << 18);	/* enab early completion */
		cfg |= (1 << 17);	/* enab cut-through (dis stor&forwrd) */

		if (want_ncq && sata_pmp_attached(ap)) {
			cfg |= EDMA_CFG_EDMA_FBS; /* FIS-based switching */
			mv_config_fbs(port_mmio, 1);
		} else {
			mv_config_fbs(port_mmio, 0);
		}
	}

	if (want_ncq) {
		cfg |= EDMA_CFG_NCQ;
		pp->pp_flags |=  MV_PP_FLAG_NCQ_EN;
	} else
		pp->pp_flags &= ~MV_PP_FLAG_NCQ_EN;

	writelfl(cfg, port_mmio + EDMA_CFG_OFS);
}

static void mv_port_free_dma_mem(struct ata_port *ap)
{
	struct mv_host_priv *hpriv = ap->host->private_data;
	struct mv_port_priv *pp = ap->private_data;
	int tag;

	if (pp->crqb) {
		dma_pool_free(hpriv->crqb_pool, pp->crqb, pp->crqb_dma);
		pp->crqb = NULL;
	}
	if (pp->crpb) {
		dma_pool_free(hpriv->crpb_pool, pp->crpb, pp->crpb_dma);
		pp->crpb = NULL;
	}
	/*
	 * For GEN_I, there's no NCQ, so we have only a single sg_tbl.
	 * For later hardware, we have one unique sg_tbl per NCQ tag.
	 */
	for (tag = 0; tag < MV_MAX_Q_DEPTH; ++tag) {
		if (pp->sg_tbl[tag]) {
			if (tag == 0 || !IS_GEN_I(hpriv))
				dma_pool_free(hpriv->sg_tbl_pool,
					      pp->sg_tbl[tag],
					      pp->sg_tbl_dma[tag]);
			pp->sg_tbl[tag] = NULL;
		}
	}
}

/**
 *      mv_port_start - Port specific init/start routine.
 *      @ap: ATA channel to manipulate
 *
 *      Allocate and point to DMA memory, init port private memory,
 *      zero indices.
 *
 *      LOCKING:
 *      Inherited from caller.
 */
static int mv_port_start(struct ata_port *ap)
{
	struct device *dev = ap->host->dev;
	struct mv_host_priv *hpriv = ap->host->private_data;
	struct mv_port_priv *pp;
	int tag;

	pp = devm_kzalloc(dev, sizeof(*pp), GFP_KERNEL);
	if (!pp)
		return -ENOMEM;
	ap->private_data = pp;

	pp->crqb = dma_pool_alloc(hpriv->crqb_pool, GFP_KERNEL, &pp->crqb_dma);
	if (!pp->crqb)
		return -ENOMEM;
	memset(pp->crqb, 0, MV_CRQB_Q_SZ);

	pp->crpb = dma_pool_alloc(hpriv->crpb_pool, GFP_KERNEL, &pp->crpb_dma);
	if (!pp->crpb)
		goto out_port_free_dma_mem;
	memset(pp->crpb, 0, MV_CRPB_Q_SZ);

	/*
	 * For GEN_I, there's no NCQ, so we only allocate a single sg_tbl.
	 * For later hardware, we need one unique sg_tbl per NCQ tag.
	 */
	for (tag = 0; tag < MV_MAX_Q_DEPTH; ++tag) {
		if (tag == 0 || !IS_GEN_I(hpriv)) {
			pp->sg_tbl[tag] = dma_pool_alloc(hpriv->sg_tbl_pool,
					      GFP_KERNEL, &pp->sg_tbl_dma[tag]);
			if (!pp->sg_tbl[tag])
				goto out_port_free_dma_mem;
		} else {
			pp->sg_tbl[tag]     = pp->sg_tbl[0];
			pp->sg_tbl_dma[tag] = pp->sg_tbl_dma[0];
		}
	}
	return 0;

out_port_free_dma_mem:
	mv_port_free_dma_mem(ap);
	return -ENOMEM;
}

/**
 *      mv_port_stop - Port specific cleanup/stop routine.
 *      @ap: ATA channel to manipulate
 *
 *      Stop DMA, cleanup port memory.
 *
 *      LOCKING:
 *      This routine uses the host lock to protect the DMA stop.
 */
static void mv_port_stop(struct ata_port *ap)
{
	mv_stop_edma(ap);
	mv_port_free_dma_mem(ap);
}

/**
 *      mv_fill_sg - Fill out the Marvell ePRD (scatter gather) entries
 *      @qc: queued command whose SG list to source from
 *
 *      Populate the SG list and mark the last entry.
 *
 *      LOCKING:
 *      Inherited from caller.
 */
static void mv_fill_sg(struct ata_queued_cmd *qc)
{
	struct mv_port_priv *pp = qc->ap->private_data;
	struct scatterlist *sg;
	struct mv_sg *mv_sg, *last_sg = NULL;
	unsigned int si;

	mv_sg = pp->sg_tbl[qc->tag];
	for_each_sg(qc->sg, sg, qc->n_elem, si) {
		dma_addr_t addr = sg_dma_address(sg);
		u32 sg_len = sg_dma_len(sg);

		while (sg_len) {
			u32 offset = addr & 0xffff;
			u32 len = sg_len;

			if ((offset + sg_len > 0x10000))
				len = 0x10000 - offset;

			mv_sg->addr = cpu_to_le32(addr & 0xffffffff);
			mv_sg->addr_hi = cpu_to_le32((addr >> 16) >> 16);
			mv_sg->flags_size = cpu_to_le32(len & 0xffff);

			sg_len -= len;
			addr += len;

			last_sg = mv_sg;
			mv_sg++;
		}
	}

	if (likely(last_sg))
		last_sg->flags_size |= cpu_to_le32(EPRD_FLAG_END_OF_TBL);
}

static void mv_crqb_pack_cmd(__le16 *cmdw, u8 data, u8 addr, unsigned last)
{
	u16 tmp = data | (addr << CRQB_CMD_ADDR_SHIFT) | CRQB_CMD_CS |
		(last ? CRQB_CMD_LAST : 0);
	*cmdw = cpu_to_le16(tmp);
}

/**
 *      mv_qc_prep - Host specific command preparation.
 *      @qc: queued command to prepare
 *
 *      This routine simply redirects to the general purpose routine
 *      if command is not DMA.  Else, it handles prep of the CRQB
 *      (command request block), does some sanity checking, and calls
 *      the SG load routine.
 *
 *      LOCKING:
 *      Inherited from caller.
 */
static void mv_qc_prep(struct ata_queued_cmd *qc)
{
	struct ata_port *ap = qc->ap;
	struct mv_port_priv *pp = ap->private_data;
	__le16 *cw;
	struct ata_taskfile *tf;
	u16 flags = 0;
	unsigned in_index;

	if ((qc->tf.protocol != ATA_PROT_DMA) &&
	    (qc->tf.protocol != ATA_PROT_NCQ))
		return;

	/* Fill in command request block
	 */
	if (!(qc->tf.flags & ATA_TFLAG_WRITE))
		flags |= CRQB_FLAG_READ;
	WARN_ON(MV_MAX_Q_DEPTH <= qc->tag);
	flags |= qc->tag << CRQB_TAG_SHIFT;
	flags |= (qc->dev->link->pmp & 0xf) << CRQB_PMP_SHIFT;

	/* get current queue index from software */
	in_index = pp->req_idx & MV_MAX_Q_DEPTH_MASK;

	pp->crqb[in_index].sg_addr =
		cpu_to_le32(pp->sg_tbl_dma[qc->tag] & 0xffffffff);
	pp->crqb[in_index].sg_addr_hi =
		cpu_to_le32((pp->sg_tbl_dma[qc->tag] >> 16) >> 16);
	pp->crqb[in_index].ctrl_flags = cpu_to_le16(flags);

	cw = &pp->crqb[in_index].ata_cmd[0];
	tf = &qc->tf;

	/* Sadly, the CRQB cannot accomodate all registers--there are
	 * only 11 bytes...so we must pick and choose required
	 * registers based on the command.  So, we drop feature and
	 * hob_feature for [RW] DMA commands, but they are needed for
	 * NCQ.  NCQ will drop hob_nsect.
	 */
	switch (tf->command) {
	case ATA_CMD_READ:
	case ATA_CMD_READ_EXT:
	case ATA_CMD_WRITE:
	case ATA_CMD_WRITE_EXT:
	case ATA_CMD_WRITE_FUA_EXT:
		mv_crqb_pack_cmd(cw++, tf->hob_nsect, ATA_REG_NSECT, 0);
		break;
	case ATA_CMD_FPDMA_READ:
	case ATA_CMD_FPDMA_WRITE:
		mv_crqb_pack_cmd(cw++, tf->hob_feature, ATA_REG_FEATURE, 0);
		mv_crqb_pack_cmd(cw++, tf->feature, ATA_REG_FEATURE, 0);
		break;
	default:
		/* The only other commands EDMA supports in non-queued and
		 * non-NCQ mode are: [RW] STREAM DMA and W DMA FUA EXT, none
		 * of which are defined/used by Linux.  If we get here, this
		 * driver needs work.
		 *
		 * FIXME: modify libata to give qc_prep a return value and
		 * return error here.
		 */
		BUG_ON(tf->command);
		break;
	}
	mv_crqb_pack_cmd(cw++, tf->nsect, ATA_REG_NSECT, 0);
	mv_crqb_pack_cmd(cw++, tf->hob_lbal, ATA_REG_LBAL, 0);
	mv_crqb_pack_cmd(cw++, tf->lbal, ATA_REG_LBAL, 0);
	mv_crqb_pack_cmd(cw++, tf->hob_lbam, ATA_REG_LBAM, 0);
	mv_crqb_pack_cmd(cw++, tf->lbam, ATA_REG_LBAM, 0);
	mv_crqb_pack_cmd(cw++, tf->hob_lbah, ATA_REG_LBAH, 0);
	mv_crqb_pack_cmd(cw++, tf->lbah, ATA_REG_LBAH, 0);
	mv_crqb_pack_cmd(cw++, tf->device, ATA_REG_DEVICE, 0);
	mv_crqb_pack_cmd(cw++, tf->command, ATA_REG_CMD, 1);	/* last */

	if (!(qc->flags & ATA_QCFLAG_DMAMAP))
		return;
	mv_fill_sg(qc);
}

/**
 *      mv_qc_prep_iie - Host specific command preparation.
 *      @qc: queued command to prepare
 *
 *      This routine simply redirects to the general purpose routine
 *      if command is not DMA.  Else, it handles prep of the CRQB
 *      (command request block), does some sanity checking, and calls
 *      the SG load routine.
 *
 *      LOCKING:
 *      Inherited from caller.
 */
static void mv_qc_prep_iie(struct ata_queued_cmd *qc)
{
	struct ata_port *ap = qc->ap;
	struct mv_port_priv *pp = ap->private_data;
	struct mv_crqb_iie *crqb;
	struct ata_taskfile *tf;
	unsigned in_index;
	u32 flags = 0;

	if ((qc->tf.protocol != ATA_PROT_DMA) &&
	    (qc->tf.protocol != ATA_PROT_NCQ))
		return;

	/* Fill in Gen IIE command request block */
	if (!(qc->tf.flags & ATA_TFLAG_WRITE))
		flags |= CRQB_FLAG_READ;

	WARN_ON(MV_MAX_Q_DEPTH <= qc->tag);
	flags |= qc->tag << CRQB_TAG_SHIFT;
	flags |= qc->tag << CRQB_HOSTQ_SHIFT;
	flags |= (qc->dev->link->pmp & 0xf) << CRQB_PMP_SHIFT;

	/* get current queue index from software */
	in_index = pp->req_idx & MV_MAX_Q_DEPTH_MASK;

	crqb = (struct mv_crqb_iie *) &pp->crqb[in_index];
	crqb->addr = cpu_to_le32(pp->sg_tbl_dma[qc->tag] & 0xffffffff);
	crqb->addr_hi = cpu_to_le32((pp->sg_tbl_dma[qc->tag] >> 16) >> 16);
	crqb->flags = cpu_to_le32(flags);

	tf = &qc->tf;
	crqb->ata_cmd[0] = cpu_to_le32(
			(tf->command << 16) |
			(tf->feature << 24)
		);
	crqb->ata_cmd[1] = cpu_to_le32(
			(tf->lbal << 0) |
			(tf->lbam << 8) |
			(tf->lbah << 16) |
			(tf->device << 24)
		);
	crqb->ata_cmd[2] = cpu_to_le32(
			(tf->hob_lbal << 0) |
			(tf->hob_lbam << 8) |
			(tf->hob_lbah << 16) |
			(tf->hob_feature << 24)
		);
	crqb->ata_cmd[3] = cpu_to_le32(
			(tf->nsect << 0) |
			(tf->hob_nsect << 8)
		);

	if (!(qc->flags & ATA_QCFLAG_DMAMAP))
		return;
	mv_fill_sg(qc);
}

/**
 *      mv_qc_issue - Initiate a command to the host
 *      @qc: queued command to start
 *
 *      This routine simply redirects to the general purpose routine
 *      if command is not DMA.  Else, it sanity checks our local
 *      caches of the request producer/consumer indices then enables
 *      DMA and bumps the request producer index.
 *
 *      LOCKING:
 *      Inherited from caller.
 */
static unsigned int mv_qc_issue(struct ata_queued_cmd *qc)
{
	struct ata_port *ap = qc->ap;
	void __iomem *port_mmio = mv_ap_base(ap);
	struct mv_port_priv *pp = ap->private_data;
	u32 in_index;

	if ((qc->tf.protocol != ATA_PROT_DMA) &&
	    (qc->tf.protocol != ATA_PROT_NCQ)) {
		/*
		 * We're about to send a non-EDMA capable command to the
		 * port.  Turn off EDMA so there won't be problems accessing
		 * shadow block, etc registers.
		 */
		mv_stop_edma(ap);
		mv_pmp_select(ap, qc->dev->link->pmp);
		return ata_sff_qc_issue(qc);
	}

	mv_start_dma(ap, port_mmio, pp, qc->tf.protocol);

	pp->req_idx++;

	in_index = (pp->req_idx & MV_MAX_Q_DEPTH_MASK) << EDMA_REQ_Q_PTR_SHIFT;

	/* and write the request in pointer to kick the EDMA to life */
	writelfl((pp->crqb_dma & EDMA_REQ_Q_BASE_LO_MASK) | in_index,
		 port_mmio + EDMA_REQ_Q_IN_PTR_OFS);

	return 0;
}

/**
 *      mv_err_intr - Handle error interrupts on the port
 *      @ap: ATA channel to manipulate
 *      @reset_allowed: bool: 0 == don't trigger from reset here
 *
 *      In most cases, just clear the interrupt and move on.  However,
 *      some cases require an eDMA reset, which also performs a COMRESET.
 *      The SERR case requires a clear of pending errors in the SATA
 *      SERROR register.  Finally, if the port disabled DMA,
 *      update our cached copy to match.
 *
 *      LOCKING:
 *      Inherited from caller.
 */
static void mv_err_intr(struct ata_port *ap, struct ata_queued_cmd *qc)
{
	void __iomem *port_mmio = mv_ap_base(ap);
	u32 edma_err_cause, eh_freeze_mask, serr = 0;
	struct mv_port_priv *pp = ap->private_data;
	struct mv_host_priv *hpriv = ap->host->private_data;
	unsigned int edma_enabled = (pp->pp_flags & MV_PP_FLAG_EDMA_EN);
	unsigned int action = 0, err_mask = 0;
	struct ata_eh_info *ehi = &ap->link.eh_info;

	ata_ehi_clear_desc(ehi);

	if (!edma_enabled) {
		/* just a guess: do we need to do this? should we
		 * expand this, and do it in all cases?
		 */
		sata_scr_read(&ap->link, SCR_ERROR, &serr);
		sata_scr_write_flush(&ap->link, SCR_ERROR, serr);
	}

	edma_err_cause = readl(port_mmio + EDMA_ERR_IRQ_CAUSE_OFS);

	ata_ehi_push_desc(ehi, "edma_err_cause=%08x", edma_err_cause);

	/*
	 * All generations share these EDMA error cause bits:
	 */
	if (edma_err_cause & EDMA_ERR_DEV)
		err_mask |= AC_ERR_DEV;
	if (edma_err_cause & (EDMA_ERR_D_PAR | EDMA_ERR_PRD_PAR |
			EDMA_ERR_CRQB_PAR | EDMA_ERR_CRPB_PAR |
			EDMA_ERR_INTRL_PAR)) {
		err_mask |= AC_ERR_ATA_BUS;
		action |= ATA_EH_RESET;
		ata_ehi_push_desc(ehi, "parity error");
	}
	if (edma_err_cause & (EDMA_ERR_DEV_DCON | EDMA_ERR_DEV_CON)) {
		ata_ehi_hotplugged(ehi);
		ata_ehi_push_desc(ehi, edma_err_cause & EDMA_ERR_DEV_DCON ?
			"dev disconnect" : "dev connect");
		action |= ATA_EH_RESET;
	}

	/*
	 * Gen-I has a different SELF_DIS bit,
	 * different FREEZE bits, and no SERR bit:
	 */
	if (IS_GEN_I(hpriv)) {
		eh_freeze_mask = EDMA_EH_FREEZE_5;
		if (edma_err_cause & EDMA_ERR_SELF_DIS_5) {
			pp->pp_flags &= ~MV_PP_FLAG_EDMA_EN;
			ata_ehi_push_desc(ehi, "EDMA self-disable");
		}
	} else {
		eh_freeze_mask = EDMA_EH_FREEZE;
		if (edma_err_cause & EDMA_ERR_SELF_DIS) {
			pp->pp_flags &= ~MV_PP_FLAG_EDMA_EN;
			ata_ehi_push_desc(ehi, "EDMA self-disable");
		}
		if (edma_err_cause & EDMA_ERR_SERR) {
			sata_scr_read(&ap->link, SCR_ERROR, &serr);
			sata_scr_write_flush(&ap->link, SCR_ERROR, serr);
			err_mask = AC_ERR_ATA_BUS;
			action |= ATA_EH_RESET;
		}
	}

	/* Clear EDMA now that SERR cleanup done */
	writelfl(~edma_err_cause, port_mmio + EDMA_ERR_IRQ_CAUSE_OFS);

	if (!err_mask) {
		err_mask = AC_ERR_OTHER;
		action |= ATA_EH_RESET;
	}

	ehi->serror |= serr;
	ehi->action |= action;

	if (qc)
		qc->err_mask |= err_mask;
	else
		ehi->err_mask |= err_mask;

	if (edma_err_cause & eh_freeze_mask)
		ata_port_freeze(ap);
	else
		ata_port_abort(ap);
}

static void mv_intr_pio(struct ata_port *ap)
{
	struct ata_queued_cmd *qc;
	u8 ata_status;

	/* ignore spurious intr if drive still BUSY */
	ata_status = readb(ap->ioaddr.status_addr);
	if (unlikely(ata_status & ATA_BUSY))
		return;

	/* get active ATA command */
	qc = ata_qc_from_tag(ap, ap->link.active_tag);
	if (unlikely(!qc))			/* no active tag */
		return;
	if (qc->tf.flags & ATA_TFLAG_POLLING)	/* polling; we don't own qc */
		return;

	/* and finally, complete the ATA command */
	qc->err_mask |= ac_err_mask(ata_status);
	ata_qc_complete(qc);
}

static void mv_intr_edma(struct ata_port *ap)
{
	void __iomem *port_mmio = mv_ap_base(ap);
	struct mv_host_priv *hpriv = ap->host->private_data;
	struct mv_port_priv *pp = ap->private_data;
	struct ata_queued_cmd *qc;
	u32 out_index, in_index;
	bool work_done = false;

	/* get h/w response queue pointer */
	in_index = (readl(port_mmio + EDMA_RSP_Q_IN_PTR_OFS)
			>> EDMA_RSP_Q_PTR_SHIFT) & MV_MAX_Q_DEPTH_MASK;

	while (1) {
		u16 status;
		unsigned int tag;

		/* get s/w response queue last-read pointer, and compare */
		out_index = pp->resp_idx & MV_MAX_Q_DEPTH_MASK;
		if (in_index == out_index)
			break;

		/* 50xx: get active ATA command */
		if (IS_GEN_I(hpriv))
			tag = ap->link.active_tag;

		/* Gen II/IIE: get active ATA command via tag, to enable
		 * support for queueing.  this works transparently for
		 * queued and non-queued modes.
		 */
		else
			tag = le16_to_cpu(pp->crpb[out_index].id) & 0x1f;

		qc = ata_qc_from_tag(ap, tag);

		/* For non-NCQ mode, the lower 8 bits of status
		 * are from EDMA_ERR_IRQ_CAUSE_OFS,
		 * which should be zero if all went well.
		 */
		status = le16_to_cpu(pp->crpb[out_index].flags);
		if ((status & 0xff) && !(pp->pp_flags & MV_PP_FLAG_NCQ_EN)) {
			mv_err_intr(ap, qc);
			return;
		}

		/* and finally, complete the ATA command */
		if (qc) {
			qc->err_mask |=
				ac_err_mask(status >> CRPB_FLAG_STATUS_SHIFT);
			ata_qc_complete(qc);
		}

		/* advance software response queue pointer, to
		 * indicate (after the loop completes) to hardware
		 * that we have consumed a response queue entry.
		 */
		work_done = true;
		pp->resp_idx++;
	}

	/* Update the software queue position index in hardware */
	if (work_done)
		writelfl((pp->crpb_dma & EDMA_RSP_Q_BASE_LO_MASK) |
			 (out_index << EDMA_RSP_Q_PTR_SHIFT),
			 port_mmio + EDMA_RSP_Q_OUT_PTR_OFS);
}

/**
 *      mv_host_intr - Handle all interrupts on the given host controller
 *      @host: host specific structure
 *      @relevant: port error bits relevant to this host controller
 *      @hc: which host controller we're to look at
 *
 *      Read then write clear the HC interrupt status then walk each
 *      port connected to the HC and see if it needs servicing.  Port
 *      success ints are reported in the HC interrupt status reg, the
 *      port error ints are reported in the higher level main
 *      interrupt status register and thus are passed in via the
 *      'relevant' argument.
 *
 *      LOCKING:
 *      Inherited from caller.
 */
static void mv_host_intr(struct ata_host *host, u32 relevant, unsigned int hc)
{
	struct mv_host_priv *hpriv = host->private_data;
	void __iomem *mmio = hpriv->base;
	void __iomem *hc_mmio = mv_hc_base(mmio, hc);
	u32 hc_irq_cause;
	int port, port0, last_port;

	if (hc == 0)
		port0 = 0;
	else
		port0 = MV_PORTS_PER_HC;

	if (HAS_PCI(host))
		last_port = port0 + MV_PORTS_PER_HC;
	else
		last_port = port0 + hpriv->n_ports;
	/* we'll need the HC success int register in most cases */
	hc_irq_cause = readl(hc_mmio + HC_IRQ_CAUSE_OFS);
	if (!hc_irq_cause)
		return;

	writelfl(~hc_irq_cause, hc_mmio + HC_IRQ_CAUSE_OFS);

	VPRINTK("ENTER, hc%u relevant=0x%08x HC IRQ cause=0x%08x\n",
		hc, relevant, hc_irq_cause);

	for (port = port0; port < last_port; port++) {
		struct ata_port *ap = host->ports[port];
		struct mv_port_priv *pp;
		int have_err_bits, hardport, shift;

		if ((!ap) || (ap->flags & ATA_FLAG_DISABLED))
			continue;

		pp = ap->private_data;

		shift = port << 1;		/* (port * 2) */
		if (port >= MV_PORTS_PER_HC)
			shift++;	/* skip bit 8 in the HC Main IRQ reg */

		have_err_bits = ((ERR_IRQ << shift) & relevant);

		if (unlikely(have_err_bits)) {
			struct ata_queued_cmd *qc;

			qc = ata_qc_from_tag(ap, ap->link.active_tag);
			if (qc && (qc->tf.flags & ATA_TFLAG_POLLING))
				continue;

			mv_err_intr(ap, qc);
			continue;
		}

		hardport = mv_hardport_from_port(port); /* range 0..3 */

		if (pp->pp_flags & MV_PP_FLAG_EDMA_EN) {
			if ((DMA_IRQ << hardport) & hc_irq_cause)
				mv_intr_edma(ap);
		} else {
			if ((DEV_IRQ << hardport) & hc_irq_cause)
				mv_intr_pio(ap);
		}
	}
	VPRINTK("EXIT\n");
}

static void mv_pci_error(struct ata_host *host, void __iomem *mmio)
{
	struct mv_host_priv *hpriv = host->private_data;
	struct ata_port *ap;
	struct ata_queued_cmd *qc;
	struct ata_eh_info *ehi;
	unsigned int i, err_mask, printed = 0;
	u32 err_cause;

	err_cause = readl(mmio + hpriv->irq_cause_ofs);

	dev_printk(KERN_ERR, host->dev, "PCI ERROR; PCI IRQ cause=0x%08x\n",
		   err_cause);

	DPRINTK("All regs @ PCI error\n");
	mv_dump_all_regs(mmio, -1, to_pci_dev(host->dev));

	writelfl(0, mmio + hpriv->irq_cause_ofs);

	for (i = 0; i < host->n_ports; i++) {
		ap = host->ports[i];
		if (!ata_link_offline(&ap->link)) {
			ehi = &ap->link.eh_info;
			ata_ehi_clear_desc(ehi);
			if (!printed++)
				ata_ehi_push_desc(ehi,
					"PCI err cause 0x%08x", err_cause);
			err_mask = AC_ERR_HOST_BUS;
			ehi->action = ATA_EH_RESET;
			qc = ata_qc_from_tag(ap, ap->link.active_tag);
			if (qc)
				qc->err_mask |= err_mask;
			else
				ehi->err_mask |= err_mask;

			ata_port_freeze(ap);
		}
	}
}

/**
 *      mv_interrupt - Main interrupt event handler
 *      @irq: unused
 *      @dev_instance: private data; in this case the host structure
 *
 *      Read the read only register to determine if any host
 *      controllers have pending interrupts.  If so, call lower level
 *      routine to handle.  Also check for PCI errors which are only
 *      reported here.
 *
 *      LOCKING:
 *      This routine holds the host lock while processing pending
 *      interrupts.
 */
static irqreturn_t mv_interrupt(int irq, void *dev_instance)
{
	struct ata_host *host = dev_instance;
	struct mv_host_priv *hpriv = host->private_data;
	unsigned int hc, handled = 0, n_hcs;
	void __iomem *mmio = hpriv->base;
	u32 main_cause, main_mask;

	spin_lock(&host->lock);
	main_cause = readl(hpriv->main_cause_reg_addr);
	main_mask  = readl(hpriv->main_mask_reg_addr);
	/*
	 * Deal with cases where we either have nothing pending, or have read
	 * a bogus register value which can indicate HW removal or PCI fault.
	 */
	if (!(main_cause & main_mask) || (main_cause == 0xffffffffU))
		goto out_unlock;

	n_hcs = mv_get_hc_count(host->ports[0]->flags);

	if (unlikely((main_cause & PCI_ERR) && HAS_PCI(host))) {
		mv_pci_error(host, mmio);
		handled = 1;
		goto out_unlock;	/* skip all other HC irq handling */
	}

	for (hc = 0; hc < n_hcs; hc++) {
		u32 relevant = main_cause & (HC0_IRQ_PEND << (hc * HC_SHIFT));
		if (relevant) {
			mv_host_intr(host, relevant, hc);
			handled = 1;
		}
	}

out_unlock:
	spin_unlock(&host->lock);
	return IRQ_RETVAL(handled);
}

static unsigned int mv5_scr_offset(unsigned int sc_reg_in)
{
	unsigned int ofs;

	switch (sc_reg_in) {
	case SCR_STATUS:
	case SCR_ERROR:
	case SCR_CONTROL:
		ofs = sc_reg_in * sizeof(u32);
		break;
	default:
		ofs = 0xffffffffU;
		break;
	}
	return ofs;
}

static int mv5_scr_read(struct ata_port *ap, unsigned int sc_reg_in, u32 *val)
{
	struct mv_host_priv *hpriv = ap->host->private_data;
	void __iomem *mmio = hpriv->base;
	void __iomem *addr = mv5_phy_base(mmio, ap->port_no);
	unsigned int ofs = mv5_scr_offset(sc_reg_in);

	if (ofs != 0xffffffffU) {
		*val = readl(addr + ofs);
		return 0;
	} else
		return -EINVAL;
}

static int mv5_scr_write(struct ata_port *ap, unsigned int sc_reg_in, u32 val)
{
	struct mv_host_priv *hpriv = ap->host->private_data;
	void __iomem *mmio = hpriv->base;
	void __iomem *addr = mv5_phy_base(mmio, ap->port_no);
	unsigned int ofs = mv5_scr_offset(sc_reg_in);

	if (ofs != 0xffffffffU) {
		writelfl(val, addr + ofs);
		return 0;
	} else
		return -EINVAL;
}

static void mv5_reset_bus(struct ata_host *host, void __iomem *mmio)
{
	struct pci_dev *pdev = to_pci_dev(host->dev);
	int early_5080;

	early_5080 = (pdev->device == 0x5080) && (pdev->revision == 0);

	if (!early_5080) {
		u32 tmp = readl(mmio + MV_PCI_EXP_ROM_BAR_CTL);
		tmp |= (1 << 0);
		writel(tmp, mmio + MV_PCI_EXP_ROM_BAR_CTL);
	}

	mv_reset_pci_bus(host, mmio);
}

static void mv5_reset_flash(struct mv_host_priv *hpriv, void __iomem *mmio)
{
	writel(0x0fcfffff, mmio + MV_FLASH_CTL);
}

static void mv5_read_preamp(struct mv_host_priv *hpriv, int idx,
			   void __iomem *mmio)
{
	void __iomem *phy_mmio = mv5_phy_base(mmio, idx);
	u32 tmp;

	tmp = readl(phy_mmio + MV5_PHY_MODE);

	hpriv->signal[idx].pre = tmp & 0x1800;	/* bits 12:11 */
	hpriv->signal[idx].amps = tmp & 0xe0;	/* bits 7:5 */
}

static void mv5_enable_leds(struct mv_host_priv *hpriv, void __iomem *mmio)
{
	u32 tmp;

	writel(0, mmio + MV_GPIO_PORT_CTL);

	/* FIXME: handle MV_HP_ERRATA_50XXB2 errata */

	tmp = readl(mmio + MV_PCI_EXP_ROM_BAR_CTL);
	tmp |= ~(1 << 0);
	writel(tmp, mmio + MV_PCI_EXP_ROM_BAR_CTL);
}

static void mv5_phy_errata(struct mv_host_priv *hpriv, void __iomem *mmio,
			   unsigned int port)
{
	void __iomem *phy_mmio = mv5_phy_base(mmio, port);
	const u32 mask = (1<<12) | (1<<11) | (1<<7) | (1<<6) | (1<<5);
	u32 tmp;
	int fix_apm_sq = (hpriv->hp_flags & MV_HP_ERRATA_50XXB0);

	if (fix_apm_sq) {
		tmp = readl(phy_mmio + MV5_LT_MODE);
		tmp |= (1 << 19);
		writel(tmp, phy_mmio + MV5_LT_MODE);

		tmp = readl(phy_mmio + MV5_PHY_CTL);
		tmp &= ~0x3;
		tmp |= 0x1;
		writel(tmp, phy_mmio + MV5_PHY_CTL);
	}

	tmp = readl(phy_mmio + MV5_PHY_MODE);
	tmp &= ~mask;
	tmp |= hpriv->signal[port].pre;
	tmp |= hpriv->signal[port].amps;
	writel(tmp, phy_mmio + MV5_PHY_MODE);
}


#undef ZERO
#define ZERO(reg) writel(0, port_mmio + (reg))
static void mv5_reset_hc_port(struct mv_host_priv *hpriv, void __iomem *mmio,
			     unsigned int port)
{
	void __iomem *port_mmio = mv_port_base(mmio, port);

	/*
	 * The datasheet warns against setting ATA_RST when EDMA is active
	 * (but doesn't say what the problem might be).  So we first try
	 * to disable the EDMA engine before doing the ATA_RST operation.
	 */
	mv_reset_channel(hpriv, mmio, port);

	ZERO(0x028);	/* command */
	writel(0x11f, port_mmio + EDMA_CFG_OFS);
	ZERO(0x004);	/* timer */
	ZERO(0x008);	/* irq err cause */
	ZERO(0x00c);	/* irq err mask */
	ZERO(0x010);	/* rq bah */
	ZERO(0x014);	/* rq inp */
	ZERO(0x018);	/* rq outp */
	ZERO(0x01c);	/* respq bah */
	ZERO(0x024);	/* respq outp */
	ZERO(0x020);	/* respq inp */
	ZERO(0x02c);	/* test control */
	writel(0xbc, port_mmio + EDMA_IORDY_TMOUT);
}
#undef ZERO

#define ZERO(reg) writel(0, hc_mmio + (reg))
static void mv5_reset_one_hc(struct mv_host_priv *hpriv, void __iomem *mmio,
			unsigned int hc)
{
	void __iomem *hc_mmio = mv_hc_base(mmio, hc);
	u32 tmp;

	ZERO(0x00c);
	ZERO(0x010);
	ZERO(0x014);
	ZERO(0x018);

	tmp = readl(hc_mmio + 0x20);
	tmp &= 0x1c1c1c1c;
	tmp |= 0x03030303;
	writel(tmp, hc_mmio + 0x20);
}
#undef ZERO

static int mv5_reset_hc(struct mv_host_priv *hpriv, void __iomem *mmio,
			unsigned int n_hc)
{
	unsigned int hc, port;

	for (hc = 0; hc < n_hc; hc++) {
		for (port = 0; port < MV_PORTS_PER_HC; port++)
			mv5_reset_hc_port(hpriv, mmio,
					  (hc * MV_PORTS_PER_HC) + port);

		mv5_reset_one_hc(hpriv, mmio, hc);
	}

	return 0;
}

#undef ZERO
#define ZERO(reg) writel(0, mmio + (reg))
static void mv_reset_pci_bus(struct ata_host *host, void __iomem *mmio)
{
	struct mv_host_priv *hpriv = host->private_data;
	u32 tmp;

	tmp = readl(mmio + MV_PCI_MODE);
	tmp &= 0xff00ffff;
	writel(tmp, mmio + MV_PCI_MODE);

	ZERO(MV_PCI_DISC_TIMER);
	ZERO(MV_PCI_MSI_TRIGGER);
	writel(0x000100ff, mmio + MV_PCI_XBAR_TMOUT);
	ZERO(HC_MAIN_IRQ_MASK_OFS);
	ZERO(MV_PCI_SERR_MASK);
	ZERO(hpriv->irq_cause_ofs);
	ZERO(hpriv->irq_mask_ofs);
	ZERO(MV_PCI_ERR_LOW_ADDRESS);
	ZERO(MV_PCI_ERR_HIGH_ADDRESS);
	ZERO(MV_PCI_ERR_ATTRIBUTE);
	ZERO(MV_PCI_ERR_COMMAND);
}
#undef ZERO

static void mv6_reset_flash(struct mv_host_priv *hpriv, void __iomem *mmio)
{
	u32 tmp;

	mv5_reset_flash(hpriv, mmio);

	tmp = readl(mmio + MV_GPIO_PORT_CTL);
	tmp &= 0x3;
	tmp |= (1 << 5) | (1 << 6);
	writel(tmp, mmio + MV_GPIO_PORT_CTL);
}

/**
 *      mv6_reset_hc - Perform the 6xxx global soft reset
 *      @mmio: base address of the HBA
 *
 *      This routine only applies to 6xxx parts.
 *
 *      LOCKING:
 *      Inherited from caller.
 */
static int mv6_reset_hc(struct mv_host_priv *hpriv, void __iomem *mmio,
			unsigned int n_hc)
{
	void __iomem *reg = mmio + PCI_MAIN_CMD_STS_OFS;
	int i, rc = 0;
	u32 t;

	/* Following procedure defined in PCI "main command and status
	 * register" table.
	 */
	t = readl(reg);
	writel(t | STOP_PCI_MASTER, reg);

	for (i = 0; i < 1000; i++) {
		udelay(1);
		t = readl(reg);
		if (PCI_MASTER_EMPTY & t)
			break;
	}
	if (!(PCI_MASTER_EMPTY & t)) {
		printk(KERN_ERR DRV_NAME ": PCI master won't flush\n");
		rc = 1;
		goto done;
	}

	/* set reset */
	i = 5;
	do {
		writel(t | GLOB_SFT_RST, reg);
		t = readl(reg);
		udelay(1);
	} while (!(GLOB_SFT_RST & t) && (i-- > 0));

	if (!(GLOB_SFT_RST & t)) {
		printk(KERN_ERR DRV_NAME ": can't set global reset\n");
		rc = 1;
		goto done;
	}

	/* clear reset and *reenable the PCI master* (not mentioned in spec) */
	i = 5;
	do {
		writel(t & ~(GLOB_SFT_RST | STOP_PCI_MASTER), reg);
		t = readl(reg);
		udelay(1);
	} while ((GLOB_SFT_RST & t) && (i-- > 0));

	if (GLOB_SFT_RST & t) {
		printk(KERN_ERR DRV_NAME ": can't clear global reset\n");
		rc = 1;
	}
	/*
	 * Temporary: wait 3 seconds before port-probing can happen,
	 * so that we don't miss finding sleepy SilXXXX port-multipliers.
	 * This can go away once hotplug is fully/correctly implemented.
	 */
	if (rc == 0)
		msleep(3000);
done:
	return rc;
}

static void mv6_read_preamp(struct mv_host_priv *hpriv, int idx,
			   void __iomem *mmio)
{
	void __iomem *port_mmio;
	u32 tmp;

	tmp = readl(mmio + MV_RESET_CFG);
	if ((tmp & (1 << 0)) == 0) {
		hpriv->signal[idx].amps = 0x7 << 8;
		hpriv->signal[idx].pre = 0x1 << 5;
		return;
	}

	port_mmio = mv_port_base(mmio, idx);
	tmp = readl(port_mmio + PHY_MODE2);

	hpriv->signal[idx].amps = tmp & 0x700;	/* bits 10:8 */
	hpriv->signal[idx].pre = tmp & 0xe0;	/* bits 7:5 */
}

static void mv6_enable_leds(struct mv_host_priv *hpriv, void __iomem *mmio)
{
	writel(0x00000060, mmio + MV_GPIO_PORT_CTL);
}

static void mv6_phy_errata(struct mv_host_priv *hpriv, void __iomem *mmio,
			   unsigned int port)
{
	void __iomem *port_mmio = mv_port_base(mmio, port);

	u32 hp_flags = hpriv->hp_flags;
	int fix_phy_mode2 =
		hp_flags & (MV_HP_ERRATA_60X1B2 | MV_HP_ERRATA_60X1C0);
	int fix_phy_mode4 =
		hp_flags & (MV_HP_ERRATA_60X1B2 | MV_HP_ERRATA_60X1C0);
	u32 m2, tmp;

	if (fix_phy_mode2) {
		m2 = readl(port_mmio + PHY_MODE2);
		m2 &= ~(1 << 16);
		m2 |= (1 << 31);
		writel(m2, port_mmio + PHY_MODE2);

		udelay(200);

		m2 = readl(port_mmio + PHY_MODE2);
		m2 &= ~((1 << 16) | (1 << 31));
		writel(m2, port_mmio + PHY_MODE2);

		udelay(200);
	}

	/* who knows what this magic does */
	tmp = readl(port_mmio + PHY_MODE3);
	tmp &= ~0x7F800000;
	tmp |= 0x2A800000;
	writel(tmp, port_mmio + PHY_MODE3);

	if (fix_phy_mode4) {
		u32 m4;

		m4 = readl(port_mmio + PHY_MODE4);

		if (hp_flags & MV_HP_ERRATA_60X1B2)
			tmp = readl(port_mmio + PHY_MODE3);

		/* workaround for errata FEr SATA#10 (part 1) */
		m4 = (m4 & ~(1 << 1)) | (1 << 0);

		writel(m4, port_mmio + PHY_MODE4);

		if (hp_flags & MV_HP_ERRATA_60X1B2)
			writel(tmp, port_mmio + PHY_MODE3);
	}

	/* Revert values of pre-emphasis and signal amps to the saved ones */
	m2 = readl(port_mmio + PHY_MODE2);

	m2 &= ~MV_M2_PREAMP_MASK;
	m2 |= hpriv->signal[port].amps;
	m2 |= hpriv->signal[port].pre;
	m2 &= ~(1 << 16);

	/* according to mvSata 3.6.1, some IIE values are fixed */
	if (IS_GEN_IIE(hpriv)) {
		m2 &= ~0xC30FF01F;
		m2 |= 0x0000900F;
	}

	writel(m2, port_mmio + PHY_MODE2);
}

/* TODO: use the generic LED interface to configure the SATA Presence */
/* & Acitivy LEDs on the board */
static void mv_soc_enable_leds(struct mv_host_priv *hpriv,
				      void __iomem *mmio)
{
	return;
}

static void mv_soc_read_preamp(struct mv_host_priv *hpriv, int idx,
			   void __iomem *mmio)
{
	void __iomem *port_mmio;
	u32 tmp;

	port_mmio = mv_port_base(mmio, idx);
	tmp = readl(port_mmio + PHY_MODE2);

	hpriv->signal[idx].amps = tmp & 0x700;	/* bits 10:8 */
	hpriv->signal[idx].pre = tmp & 0xe0;	/* bits 7:5 */
}

#undef ZERO
#define ZERO(reg) writel(0, port_mmio + (reg))
static void mv_soc_reset_hc_port(struct mv_host_priv *hpriv,
					void __iomem *mmio, unsigned int port)
{
	void __iomem *port_mmio = mv_port_base(mmio, port);

	/*
	 * The datasheet warns against setting ATA_RST when EDMA is active
	 * (but doesn't say what the problem might be).  So we first try
	 * to disable the EDMA engine before doing the ATA_RST operation.
	 */
	mv_reset_channel(hpriv, mmio, port);

	ZERO(0x028);		/* command */
	writel(0x101f, port_mmio + EDMA_CFG_OFS);
	ZERO(0x004);		/* timer */
	ZERO(0x008);		/* irq err cause */
	ZERO(0x00c);		/* irq err mask */
	ZERO(0x010);		/* rq bah */
	ZERO(0x014);		/* rq inp */
	ZERO(0x018);		/* rq outp */
	ZERO(0x01c);		/* respq bah */
	ZERO(0x024);		/* respq outp */
	ZERO(0x020);		/* respq inp */
	ZERO(0x02c);		/* test control */
	writel(0xbc, port_mmio + EDMA_IORDY_TMOUT);
}

#undef ZERO

#define ZERO(reg) writel(0, hc_mmio + (reg))
static void mv_soc_reset_one_hc(struct mv_host_priv *hpriv,
				       void __iomem *mmio)
{
	void __iomem *hc_mmio = mv_hc_base(mmio, 0);

	ZERO(0x00c);
	ZERO(0x010);
	ZERO(0x014);

}

#undef ZERO

static int mv_soc_reset_hc(struct mv_host_priv *hpriv,
				  void __iomem *mmio, unsigned int n_hc)
{
	unsigned int port;

	for (port = 0; port < hpriv->n_ports; port++)
		mv_soc_reset_hc_port(hpriv, mmio, port);

	mv_soc_reset_one_hc(hpriv, mmio);

	return 0;
}

static void mv_soc_reset_flash(struct mv_host_priv *hpriv,
				      void __iomem *mmio)
{
	return;
}

static void mv_soc_reset_bus(struct ata_host *host, void __iomem *mmio)
{
	return;
}

static void mv_setup_ifctl(void __iomem *port_mmio, int want_gen2i)
{
	u32 ifctl = readl(port_mmio + SATA_INTERFACE_CFG);

	ifctl = (ifctl & 0xf7f) | 0x9b1000;	/* from chip spec */
	if (want_gen2i)
		ifctl |= (1 << 7);		/* enable gen2i speed */
	writelfl(ifctl, port_mmio + SATA_INTERFACE_CFG);
}

/*
 * Caller must ensure that EDMA is not active,
 * by first doing mv_stop_edma() where needed.
 */
static void mv_reset_channel(struct mv_host_priv *hpriv, void __iomem *mmio,
			     unsigned int port_no)
{
	void __iomem *port_mmio = mv_port_base(mmio, port_no);

	mv_stop_edma_engine(port_mmio);
	writelfl(ATA_RST, port_mmio + EDMA_CMD_OFS);

	if (!IS_GEN_I(hpriv)) {
		/* Enable 3.0gb/s link speed */
		mv_setup_ifctl(port_mmio, 1);
	}
	/*
	 * Strobing ATA_RST here causes a hard reset of the SATA transport,
	 * link, and physical layers.  It resets all SATA interface registers
	 * (except for SATA_INTERFACE_CFG), and issues a COMRESET to the dev.
	 */
	writelfl(ATA_RST, port_mmio + EDMA_CMD_OFS);
	udelay(25);	/* allow reset propagation */
	writelfl(0, port_mmio + EDMA_CMD_OFS);

	hpriv->ops->phy_errata(hpriv, mmio, port_no);

	if (IS_GEN_I(hpriv))
		mdelay(1);
}

static void mv_pmp_select(struct ata_port *ap, int pmp)
{
	if (sata_pmp_supported(ap)) {
		void __iomem *port_mmio = mv_ap_base(ap);
		u32 reg = readl(port_mmio + SATA_IFCTL_OFS);
		int old = reg & 0xf;

		if (old != pmp) {
			reg = (reg & ~0xf) | pmp;
			writelfl(reg, port_mmio + SATA_IFCTL_OFS);
		}
	}
}

static int mv_pmp_hardreset(struct ata_link *link, unsigned int *class,
				unsigned long deadline)
{
	mv_pmp_select(link->ap, sata_srst_pmp(link));
	return sata_std_hardreset(link, class, deadline);
}

static int mv_softreset(struct ata_link *link, unsigned int *class,
				unsigned long deadline)
{
	mv_pmp_select(link->ap, sata_srst_pmp(link));
	return ata_sff_softreset(link, class, deadline);
}

static int mv_hardreset(struct ata_link *link, unsigned int *class,
			unsigned long deadline)
{
	struct ata_port *ap = link->ap;
	struct mv_host_priv *hpriv = ap->host->private_data;
	struct mv_port_priv *pp = ap->private_data;
	void __iomem *mmio = hpriv->base;
	int rc, attempts = 0, extra = 0;
	u32 sstatus;
	bool online;

	mv_reset_channel(hpriv, mmio, ap->port_no);
	pp->pp_flags &= ~MV_PP_FLAG_EDMA_EN;

	/* Workaround for errata FEr SATA#10 (part 2) */
	do {
		const unsigned long *timing =
				sata_ehc_deb_timing(&link->eh_context);

		rc = sata_link_hardreset(link, timing, deadline + extra,
					 &online, NULL);
		if (rc)
			return rc;
		sata_scr_read(link, SCR_STATUS, &sstatus);
		if (!IS_GEN_I(hpriv) && ++attempts >= 5 && sstatus == 0x121) {
			/* Force 1.5gb/s link speed and try again */
			mv_setup_ifctl(mv_ap_base(ap), 0);
			if (time_after(jiffies + HZ, deadline))
				extra = HZ; /* only extend it once, max */
		}
	} while (sstatus != 0x0 && sstatus != 0x113 && sstatus != 0x123);

	return rc;
}

static void mv_eh_freeze(struct ata_port *ap)
{
	struct mv_host_priv *hpriv = ap->host->private_data;
	unsigned int hc = (ap->port_no > 3) ? 1 : 0;
	unsigned int shift;
	u32 main_mask;

	/* FIXME: handle coalescing completion events properly */

	shift = ap->port_no * 2;
	if (hc > 0)
		shift++;

	/* disable assertion of portN err, done events */
	main_mask = readl(hpriv->main_mask_reg_addr);
	main_mask &= ~((DONE_IRQ | ERR_IRQ) << shift);
	writelfl(main_mask, hpriv->main_mask_reg_addr);
}

static void mv_eh_thaw(struct ata_port *ap)
{
	struct mv_host_priv *hpriv = ap->host->private_data;
	void __iomem *mmio = hpriv->base;
	unsigned int hc = (ap->port_no > 3) ? 1 : 0;
	void __iomem *hc_mmio = mv_hc_base(mmio, hc);
	void __iomem *port_mmio = mv_ap_base(ap);
	unsigned int shift, hc_port_no = ap->port_no;
	u32 main_mask, hc_irq_cause;

	/* FIXME: handle coalescing completion events properly */

	shift = ap->port_no * 2;
	if (hc > 0) {
		shift++;
		hc_port_no -= 4;
	}

	/* clear EDMA errors on this port */
	writel(0, port_mmio + EDMA_ERR_IRQ_CAUSE_OFS);

	/* clear pending irq events */
	hc_irq_cause = readl(hc_mmio + HC_IRQ_CAUSE_OFS);
	hc_irq_cause &= ~((DEV_IRQ | DMA_IRQ) << hc_port_no);
	writel(hc_irq_cause, hc_mmio + HC_IRQ_CAUSE_OFS);

	/* enable assertion of portN err, done events */
	main_mask = readl(hpriv->main_mask_reg_addr);
	main_mask |= ((DONE_IRQ | ERR_IRQ) << shift);
	writelfl(main_mask, hpriv->main_mask_reg_addr);
}

/**
 *      mv_port_init - Perform some early initialization on a single port.
 *      @port: libata data structure storing shadow register addresses
 *      @port_mmio: base address of the port
 *
 *      Initialize shadow register mmio addresses, clear outstanding
 *      interrupts on the port, and unmask interrupts for the future
 *      start of the port.
 *
 *      LOCKING:
 *      Inherited from caller.
 */
static void mv_port_init(struct ata_ioports *port,  void __iomem *port_mmio)
{
	void __iomem *shd_base = port_mmio + SHD_BLK_OFS;
	unsigned serr_ofs;

	/* PIO related setup
	 */
	port->data_addr = shd_base + (sizeof(u32) * ATA_REG_DATA);
	port->error_addr =
		port->feature_addr = shd_base + (sizeof(u32) * ATA_REG_ERR);
	port->nsect_addr = shd_base + (sizeof(u32) * ATA_REG_NSECT);
	port->lbal_addr = shd_base + (sizeof(u32) * ATA_REG_LBAL);
	port->lbam_addr = shd_base + (sizeof(u32) * ATA_REG_LBAM);
	port->lbah_addr = shd_base + (sizeof(u32) * ATA_REG_LBAH);
	port->device_addr = shd_base + (sizeof(u32) * ATA_REG_DEVICE);
	port->status_addr =
		port->command_addr = shd_base + (sizeof(u32) * ATA_REG_STATUS);
	/* special case: control/altstatus doesn't have ATA_REG_ address */
	port->altstatus_addr = port->ctl_addr = shd_base + SHD_CTL_AST_OFS;

	/* unused: */
	port->cmd_addr = port->bmdma_addr = port->scr_addr = NULL;

	/* Clear any currently outstanding port interrupt conditions */
	serr_ofs = mv_scr_offset(SCR_ERROR);
	writelfl(readl(port_mmio + serr_ofs), port_mmio + serr_ofs);
	writelfl(0, port_mmio + EDMA_ERR_IRQ_CAUSE_OFS);

	/* unmask all non-transient EDMA error interrupts */
	writelfl(~EDMA_ERR_IRQ_TRANSIENT, port_mmio + EDMA_ERR_IRQ_MASK_OFS);

	VPRINTK("EDMA cfg=0x%08x EDMA IRQ err cause/mask=0x%08x/0x%08x\n",
		readl(port_mmio + EDMA_CFG_OFS),
		readl(port_mmio + EDMA_ERR_IRQ_CAUSE_OFS),
		readl(port_mmio + EDMA_ERR_IRQ_MASK_OFS));
}

static int mv_chip_id(struct ata_host *host, unsigned int board_idx)
{
	struct pci_dev *pdev = to_pci_dev(host->dev);
	struct mv_host_priv *hpriv = host->private_data;
	u32 hp_flags = hpriv->hp_flags;

	switch (board_idx) {
	case chip_5080:
		hpriv->ops = &mv5xxx_ops;
		hp_flags |= MV_HP_GEN_I;

		switch (pdev->revision) {
		case 0x1:
			hp_flags |= MV_HP_ERRATA_50XXB0;
			break;
		case 0x3:
			hp_flags |= MV_HP_ERRATA_50XXB2;
			break;
		default:
			dev_printk(KERN_WARNING, &pdev->dev,
			   "Applying 50XXB2 workarounds to unknown rev\n");
			hp_flags |= MV_HP_ERRATA_50XXB2;
			break;
		}
		break;

	case chip_504x:
	case chip_508x:
		hpriv->ops = &mv5xxx_ops;
		hp_flags |= MV_HP_GEN_I;

		switch (pdev->revision) {
		case 0x0:
			hp_flags |= MV_HP_ERRATA_50XXB0;
			break;
		case 0x3:
			hp_flags |= MV_HP_ERRATA_50XXB2;
			break;
		default:
			dev_printk(KERN_WARNING, &pdev->dev,
			   "Applying B2 workarounds to unknown rev\n");
			hp_flags |= MV_HP_ERRATA_50XXB2;
			break;
		}
		break;

	case chip_604x:
	case chip_608x:
		hpriv->ops = &mv6xxx_ops;
		hp_flags |= MV_HP_GEN_II;

		switch (pdev->revision) {
		case 0x7:
			hp_flags |= MV_HP_ERRATA_60X1B2;
			break;
		case 0x9:
			hp_flags |= MV_HP_ERRATA_60X1C0;
			break;
		default:
			dev_printk(KERN_WARNING, &pdev->dev,
				   "Applying B2 workarounds to unknown rev\n");
			hp_flags |= MV_HP_ERRATA_60X1B2;
			break;
		}
		break;

	case chip_7042:
		hp_flags |= MV_HP_PCIE;
		if (pdev->vendor == PCI_VENDOR_ID_TTI &&
		    (pdev->device == 0x2300 || pdev->device == 0x2310))
		{
			/*
			 * Highpoint RocketRAID PCIe 23xx series cards:
			 *
			 * Unconfigured drives are treated as "Legacy"
			 * by the BIOS, and it overwrites sector 8 with
			 * a "Lgcy" metadata block prior to Linux boot.
			 *
			 * Configured drives (RAID or JBOD) leave sector 8
			 * alone, but instead overwrite a high numbered
			 * sector for the RAID metadata.  This sector can
			 * be determined exactly, by truncating the physical
			 * drive capacity to a nice even GB value.
			 *
			 * RAID metadata is at: (dev->n_sectors & ~0xfffff)
			 *
			 * Warn the user, lest they think we're just buggy.
			 */
			printk(KERN_WARNING DRV_NAME ": Highpoint RocketRAID"
				" BIOS CORRUPTS DATA on all attached drives,"
				" regardless of if/how they are configured."
				" BEWARE!\n");
			printk(KERN_WARNING DRV_NAME ": For data safety, do not"
				" use sectors 8-9 on \"Legacy\" drives,"
				" and avoid the final two gigabytes on"
				" all RocketRAID BIOS initialized drives.\n");
		}
	case chip_6042:
		hpriv->ops = &mv6xxx_ops;
		hp_flags |= MV_HP_GEN_IIE;

		switch (pdev->revision) {
		case 0x0:
			hp_flags |= MV_HP_ERRATA_XX42A0;
			break;
		case 0x1:
			hp_flags |= MV_HP_ERRATA_60X1C0;
			break;
		default:
			dev_printk(KERN_WARNING, &pdev->dev,
			   "Applying 60X1C0 workarounds to unknown rev\n");
			hp_flags |= MV_HP_ERRATA_60X1C0;
			break;
		}
		break;
	case chip_soc:
		hpriv->ops = &mv_soc_ops;
		hp_flags |= MV_HP_ERRATA_60X1C0;
		break;

	default:
		dev_printk(KERN_ERR, host->dev,
			   "BUG: invalid board index %u\n", board_idx);
		return 1;
	}

	hpriv->hp_flags = hp_flags;
	if (hp_flags & MV_HP_PCIE) {
		hpriv->irq_cause_ofs	= PCIE_IRQ_CAUSE_OFS;
		hpriv->irq_mask_ofs	= PCIE_IRQ_MASK_OFS;
		hpriv->unmask_all_irqs	= PCIE_UNMASK_ALL_IRQS;
	} else {
		hpriv->irq_cause_ofs	= PCI_IRQ_CAUSE_OFS;
		hpriv->irq_mask_ofs	= PCI_IRQ_MASK_OFS;
		hpriv->unmask_all_irqs	= PCI_UNMASK_ALL_IRQS;
	}

	return 0;
}

/**
 *      mv_init_host - Perform some early initialization of the host.
 *	@host: ATA host to initialize
 *      @board_idx: controller index
 *
 *      If possible, do an early global reset of the host.  Then do
 *      our port init and clear/unmask all/relevant host interrupts.
 *
 *      LOCKING:
 *      Inherited from caller.
 */
static int mv_init_host(struct ata_host *host, unsigned int board_idx)
{
	int rc = 0, n_hc, port, hc;
	struct mv_host_priv *hpriv = host->private_data;
	void __iomem *mmio = hpriv->base;

	rc = mv_chip_id(host, board_idx);
	if (rc)
		goto done;

	if (HAS_PCI(host)) {
		hpriv->main_cause_reg_addr = mmio + HC_MAIN_IRQ_CAUSE_OFS;
		hpriv->main_mask_reg_addr  = mmio + HC_MAIN_IRQ_MASK_OFS;
	} else {
		hpriv->main_cause_reg_addr = mmio + HC_SOC_MAIN_IRQ_CAUSE_OFS;
		hpriv->main_mask_reg_addr  = mmio + HC_SOC_MAIN_IRQ_MASK_OFS;
	}

	/* global interrupt mask: 0 == mask everything */
	writel(0, hpriv->main_mask_reg_addr);

	n_hc = mv_get_hc_count(host->ports[0]->flags);

	for (port = 0; port < host->n_ports; port++)
		hpriv->ops->read_preamp(hpriv, port, mmio);

	rc = hpriv->ops->reset_hc(hpriv, mmio, n_hc);
	if (rc)
		goto done;

	hpriv->ops->reset_flash(hpriv, mmio);
	hpriv->ops->reset_bus(host, mmio);
	hpriv->ops->enable_leds(hpriv, mmio);

	for (port = 0; port < host->n_ports; port++) {
		struct ata_port *ap = host->ports[port];
		void __iomem *port_mmio = mv_port_base(mmio, port);

		mv_port_init(&ap->ioaddr, port_mmio);

#ifdef CONFIG_PCI
		if (HAS_PCI(host)) {
			unsigned int offset = port_mmio - mmio;
			ata_port_pbar_desc(ap, MV_PRIMARY_BAR, -1, "mmio");
			ata_port_pbar_desc(ap, MV_PRIMARY_BAR, offset, "port");
		}
#endif
	}

	for (hc = 0; hc < n_hc; hc++) {
		void __iomem *hc_mmio = mv_hc_base(mmio, hc);

		VPRINTK("HC%i: HC config=0x%08x HC IRQ cause "
			"(before clear)=0x%08x\n", hc,
			readl(hc_mmio + HC_CFG_OFS),
			readl(hc_mmio + HC_IRQ_CAUSE_OFS));

		/* Clear any currently outstanding hc interrupt conditions */
		writelfl(0, hc_mmio + HC_IRQ_CAUSE_OFS);
	}

	if (HAS_PCI(host)) {
		/* Clear any currently outstanding host interrupt conditions */
		writelfl(0, mmio + hpriv->irq_cause_ofs);

		/* and unmask interrupt generation for host regs */
		writelfl(hpriv->unmask_all_irqs, mmio + hpriv->irq_mask_ofs);
		if (IS_GEN_I(hpriv))
			writelfl(~HC_MAIN_MASKED_IRQS_5,
				 hpriv->main_mask_reg_addr);
		else
			writelfl(~HC_MAIN_MASKED_IRQS,
				 hpriv->main_mask_reg_addr);

		VPRINTK("HC MAIN IRQ cause/mask=0x%08x/0x%08x "
			"PCI int cause/mask=0x%08x/0x%08x\n",
			readl(hpriv->main_cause_reg_addr),
			readl(hpriv->main_mask_reg_addr),
			readl(mmio + hpriv->irq_cause_ofs),
			readl(mmio + hpriv->irq_mask_ofs));
	} else {
		writelfl(~HC_MAIN_MASKED_IRQS_SOC,
			 hpriv->main_mask_reg_addr);
		VPRINTK("HC MAIN IRQ cause/mask=0x%08x/0x%08x\n",
			readl(hpriv->main_cause_reg_addr),
			readl(hpriv->main_mask_reg_addr));
	}
done:
	return rc;
}

static int mv_create_dma_pools(struct mv_host_priv *hpriv, struct device *dev)
{
	hpriv->crqb_pool   = dmam_pool_create("crqb_q", dev, MV_CRQB_Q_SZ,
							     MV_CRQB_Q_SZ, 0);
	if (!hpriv->crqb_pool)
		return -ENOMEM;

	hpriv->crpb_pool   = dmam_pool_create("crpb_q", dev, MV_CRPB_Q_SZ,
							     MV_CRPB_Q_SZ, 0);
	if (!hpriv->crpb_pool)
		return -ENOMEM;

	hpriv->sg_tbl_pool = dmam_pool_create("sg_tbl", dev, MV_SG_TBL_SZ,
							     MV_SG_TBL_SZ, 0);
	if (!hpriv->sg_tbl_pool)
		return -ENOMEM;

	return 0;
}

static void mv_conf_mbus_windows(struct mv_host_priv *hpriv,
				 struct mbus_dram_target_info *dram)
{
	int i;

	for (i = 0; i < 4; i++) {
		writel(0, hpriv->base + WINDOW_CTRL(i));
		writel(0, hpriv->base + WINDOW_BASE(i));
	}

	for (i = 0; i < dram->num_cs; i++) {
		struct mbus_dram_window *cs = dram->cs + i;

		writel(((cs->size - 1) & 0xffff0000) |
			(cs->mbus_attr << 8) |
			(dram->mbus_dram_target_id << 4) | 1,
			hpriv->base + WINDOW_CTRL(i));
		writel(cs->base, hpriv->base + WINDOW_BASE(i));
	}
}

/**
 *      mv_platform_probe - handle a positive probe of an soc Marvell
 *      host
 *      @pdev: platform device found
 *
 *      LOCKING:
 *      Inherited from caller.
 */
static int mv_platform_probe(struct platform_device *pdev)
{
	static int printed_version;
	const struct mv_sata_platform_data *mv_platform_data;
	const struct ata_port_info *ppi[] =
	    { &mv_port_info[chip_soc], NULL };
	struct ata_host *host;
	struct mv_host_priv *hpriv;
	struct resource *res;
	int n_ports, rc;

	if (!printed_version++)
		dev_printk(KERN_INFO, &pdev->dev, "version " DRV_VERSION "\n");

	/*
	 * Simple resource validation ..
	 */
	if (unlikely(pdev->num_resources != 2)) {
		dev_err(&pdev->dev, "invalid number of resources\n");
		return -EINVAL;
	}

	/*
	 * Get the register base first
	 */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (res == NULL)
		return -EINVAL;

	/* allocate host */
	mv_platform_data = pdev->dev.platform_data;
	n_ports = mv_platform_data->n_ports;

	host = ata_host_alloc_pinfo(&pdev->dev, ppi, n_ports);
	hpriv = devm_kzalloc(&pdev->dev, sizeof(*hpriv), GFP_KERNEL);

	if (!host || !hpriv)
		return -ENOMEM;
	host->private_data = hpriv;
	hpriv->n_ports = n_ports;

	host->iomap = NULL;
	hpriv->base = devm_ioremap(&pdev->dev, res->start,
				   res->end - res->start + 1);
	hpriv->base -= MV_SATAHC0_REG_BASE;

	/*
	 * (Re-)program MBUS remapping windows if we are asked to.
	 */
	if (mv_platform_data->dram != NULL)
		mv_conf_mbus_windows(hpriv, mv_platform_data->dram);

	rc = mv_create_dma_pools(hpriv, &pdev->dev);
	if (rc)
		return rc;

	/* initialize adapter */
	rc = mv_init_host(host, chip_soc);
	if (rc)
		return rc;

	dev_printk(KERN_INFO, &pdev->dev,
		   "slots %u ports %d\n", (unsigned)MV_MAX_Q_DEPTH,
		   host->n_ports);

	return ata_host_activate(host, platform_get_irq(pdev, 0), mv_interrupt,
				 IRQF_SHARED, &mv6_sht);
}

/*
 *
 *      mv_platform_remove    -       unplug a platform interface
 *      @pdev: platform device
 *
 *      A platform bus SATA device has been unplugged. Perform the needed
 *      cleanup. Also called on module unload for any active devices.
 */
static int __devexit mv_platform_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct ata_host *host = dev_get_drvdata(dev);

	ata_host_detach(host);
	return 0;
}

static struct platform_driver mv_platform_driver = {
	.probe			= mv_platform_probe,
	.remove			= __devexit_p(mv_platform_remove),
	.driver			= {
				   .name = DRV_NAME,
				   .owner = THIS_MODULE,
				  },
};


#ifdef CONFIG_PCI
static int mv_pci_init_one(struct pci_dev *pdev,
			   const struct pci_device_id *ent);


static struct pci_driver mv_pci_driver = {
	.name			= DRV_NAME,
	.id_table		= mv_pci_tbl,
	.probe			= mv_pci_init_one,
	.remove			= ata_pci_remove_one,
};

/*
 * module options
 */
static int msi;	      /* Use PCI msi; either zero (off, default) or non-zero */


/* move to PCI layer or libata core? */
static int pci_go_64(struct pci_dev *pdev)
{
	int rc;

	if (!pci_set_dma_mask(pdev, DMA_64BIT_MASK)) {
		rc = pci_set_consistent_dma_mask(pdev, DMA_64BIT_MASK);
		if (rc) {
			rc = pci_set_consistent_dma_mask(pdev, DMA_32BIT_MASK);
			if (rc) {
				dev_printk(KERN_ERR, &pdev->dev,
					   "64-bit DMA enable failed\n");
				return rc;
			}
		}
	} else {
		rc = pci_set_dma_mask(pdev, DMA_32BIT_MASK);
		if (rc) {
			dev_printk(KERN_ERR, &pdev->dev,
				   "32-bit DMA enable failed\n");
			return rc;
		}
		rc = pci_set_consistent_dma_mask(pdev, DMA_32BIT_MASK);
		if (rc) {
			dev_printk(KERN_ERR, &pdev->dev,
				   "32-bit consistent DMA enable failed\n");
			return rc;
		}
	}

	return rc;
}

/**
 *      mv_print_info - Dump key info to kernel log for perusal.
 *      @host: ATA host to print info about
 *
 *      FIXME: complete this.
 *
 *      LOCKING:
 *      Inherited from caller.
 */
static void mv_print_info(struct ata_host *host)
{
	struct pci_dev *pdev = to_pci_dev(host->dev);
	struct mv_host_priv *hpriv = host->private_data;
	u8 scc;
	const char *scc_s, *gen;

	/* Use this to determine the HW stepping of the chip so we know
	 * what errata to workaround
	 */
	pci_read_config_byte(pdev, PCI_CLASS_DEVICE, &scc);
	if (scc == 0)
		scc_s = "SCSI";
	else if (scc == 0x01)
		scc_s = "RAID";
	else
		scc_s = "?";

	if (IS_GEN_I(hpriv))
		gen = "I";
	else if (IS_GEN_II(hpriv))
		gen = "II";
	else if (IS_GEN_IIE(hpriv))
		gen = "IIE";
	else
		gen = "?";

	dev_printk(KERN_INFO, &pdev->dev,
	       "Gen-%s %u slots %u ports %s mode IRQ via %s\n",
	       gen, (unsigned)MV_MAX_Q_DEPTH, host->n_ports,
	       scc_s, (MV_HP_FLAG_MSI & hpriv->hp_flags) ? "MSI" : "INTx");
}

/**
 *      mv_pci_init_one - handle a positive probe of a PCI Marvell host
 *      @pdev: PCI device found
 *      @ent: PCI device ID entry for the matched host
 *
 *      LOCKING:
 *      Inherited from caller.
 */
static int mv_pci_init_one(struct pci_dev *pdev,
			   const struct pci_device_id *ent)
{
	static int printed_version;
	unsigned int board_idx = (unsigned int)ent->driver_data;
	const struct ata_port_info *ppi[] = { &mv_port_info[board_idx], NULL };
	struct ata_host *host;
	struct mv_host_priv *hpriv;
	int n_ports, rc;

	if (!printed_version++)
		dev_printk(KERN_INFO, &pdev->dev, "version " DRV_VERSION "\n");

	/* allocate host */
	n_ports = mv_get_hc_count(ppi[0]->flags) * MV_PORTS_PER_HC;

	host = ata_host_alloc_pinfo(&pdev->dev, ppi, n_ports);
	hpriv = devm_kzalloc(&pdev->dev, sizeof(*hpriv), GFP_KERNEL);
	if (!host || !hpriv)
		return -ENOMEM;
	host->private_data = hpriv;
	hpriv->n_ports = n_ports;

	/* acquire resources */
	rc = pcim_enable_device(pdev);
	if (rc)
		return rc;

	rc = pcim_iomap_regions(pdev, 1 << MV_PRIMARY_BAR, DRV_NAME);
	if (rc == -EBUSY)
		pcim_pin_device(pdev);
	if (rc)
		return rc;
	host->iomap = pcim_iomap_table(pdev);
	hpriv->base = host->iomap[MV_PRIMARY_BAR];

	rc = pci_go_64(pdev);
	if (rc)
		return rc;

	rc = mv_create_dma_pools(hpriv, &pdev->dev);
	if (rc)
		return rc;

	/* initialize adapter */
	rc = mv_init_host(host, board_idx);
	if (rc)
		return rc;

	/* Enable interrupts */
	if (msi && pci_enable_msi(pdev))
		pci_intx(pdev, 1);

	mv_dump_pci_cfg(pdev, 0x68);
	mv_print_info(host);

	pci_set_master(pdev);
	pci_try_set_mwi(pdev);
	return ata_host_activate(host, pdev->irq, mv_interrupt, IRQF_SHARED,
				 IS_GEN_I(hpriv) ? &mv5_sht : &mv6_sht);
}
#endif

static int mv_platform_probe(struct platform_device *pdev);
static int __devexit mv_platform_remove(struct platform_device *pdev);

static int __init mv_init(void)
{
	int rc = -ENODEV;
#ifdef CONFIG_PCI
	rc = pci_register_driver(&mv_pci_driver);
	if (rc < 0)
		return rc;
#endif
	rc = platform_driver_register(&mv_platform_driver);

#ifdef CONFIG_PCI
	if (rc < 0)
		pci_unregister_driver(&mv_pci_driver);
#endif
	return rc;
}

static void __exit mv_exit(void)
{
#ifdef CONFIG_PCI
	pci_unregister_driver(&mv_pci_driver);
#endif
	platform_driver_unregister(&mv_platform_driver);
}

MODULE_AUTHOR("Brett Russ");
MODULE_DESCRIPTION("SCSI low-level driver for Marvell SATA controllers");
MODULE_LICENSE("GPL");
MODULE_DEVICE_TABLE(pci, mv_pci_tbl);
MODULE_VERSION(DRV_VERSION);
MODULE_ALIAS("platform:" DRV_NAME);

#ifdef CONFIG_PCI
module_param(msi, int, 0444);
MODULE_PARM_DESC(msi, "Enable use of PCI MSI (0=off, 1=on)");
#endif

module_init(mv_init);
module_exit(mv_exit);
