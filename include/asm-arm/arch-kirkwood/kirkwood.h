/*
 * include/asm-arm/arch-kirkwood/kirkwood.h
 *
 * Generic definitions for Marvell Kirkwood SoC flavors:
 *  88F6180, 88F6192 and 88F6281.
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2.  This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#ifndef __ASM_ARCH_KIRKWOOD_H
#define __ASM_ARCH_KIRKWOOD_H

/*
 * Marvell Kirkwood address maps.
 *
 * phys
 * e0000000	PCIe Memory space
 * f1000000	on-chip peripheral registers
 * f2000000	PCIe I/O space
 * f3000000	NAND controller address window
 *
 * virt		phys		size
 * fee00000	f1000000	1M	on-chip peripheral registers
 * fef00000	f2000000	1M	PCIe I/O space
 */

#define KIRKWOOD_NAND_MEM_PHYS_BASE	0xf3000000
#define KIRKWOOD_NAND_MEM_SIZE		SZ_64K /* 1K is sufficient, but 64K
						* is the minimal window size
						*/

#define KIRKWOOD_PCIE_IO_PHYS_BASE	0xf2000000
#define KIRKWOOD_PCIE_IO_VIRT_BASE	0xfef00000
#define KIRKWOOD_PCIE_IO_BUS_BASE	0x00000000
#define KIRKWOOD_PCIE_IO_SIZE		SZ_1M

#define KIRKWOOD_REGS_PHYS_BASE		0xf1000000
#define KIRKWOOD_REGS_VIRT_BASE		0xfee00000
#define KIRKWOOD_REGS_SIZE		SZ_1M

#define KIRKWOOD_PCIE_MEM_PHYS_BASE	0xe0000000
#define KIRKWOOD_PCIE_MEM_SIZE		SZ_128M

/*
 * MBUS bridge registers.
 */
#define BRIDGE_VIRT_BASE	(KIRKWOOD_REGS_VIRT_BASE | 0x20000)
#define  CPU_CONTROL		(BRIDGE_VIRT_BASE | 0x0104)
#define   CPU_RESET		0x00000002
//#define   L2_WRITETHROUGH	0x00020000
#define  RSTOUTn_MASK		(BRIDGE_VIRT_BASE | 0x0108)
#define   SOFT_RESET_OUT_EN	0x00000004
#define  SYSTEM_SOFT_RESET	(BRIDGE_VIRT_BASE | 0x010c)
#define   SOFT_RESET		0x00000001
#define  BRIDGE_CAUSE		(BRIDGE_VIRT_BASE | 0x0110)
#define  BRIDGE_MASK		(BRIDGE_VIRT_BASE | 0x0114)
#define   BRIDGE_INT_TIMER0	0x0002
#define   BRIDGE_INT_TIMER1	0x0004
#define   BRIDGE_INT_TIMER1_CLR	(~0x0004)
#define  IRQ_VIRT_BASE		(BRIDGE_VIRT_BASE | 0x0200)
#define   IRQ_CAUSE_LOW_OFF	0x0000
#define   IRQ_MASK_LOW_OFF	0x0004
#define   IRQ_CAUSE_HIGH_OFF	0x0010
#define   IRQ_MASK_HIGH_OFF	0x0014
#define  TIMER_VIRT_BASE	(BRIDGE_VIRT_BASE | 0x0300)

/*
 * Register Map
 */
#define DDR_VIRT_BASE		(KIRKWOOD_REGS_VIRT_BASE | 0x00000)
#define  DDR_WINDOW_CPU_BASE	(DDR_VIRT_BASE | 0x1500)

#define DEV_BUS_PHYS_BASE	(KIRKWOOD_REGS_PHYS_BASE | 0x10000)
#define DEV_BUS_VIRT_BASE	(KIRKWOOD_REGS_VIRT_BASE | 0x10000)
#define  SAMPLE_AT_RESET	(DEV_BUS_VIRT_BASE | 0x0030)
#define  DEVICE_ID		(DEV_BUS_VIRT_BASE | 0x0034)
#define  RTC_PHYS_BASE		(DEV_BUS_PHYS_BASE | 0x0300)
#define  SPI_PHYS_BASE		(DEV_BUS_PHYS_BASE | 0x0600)
#define  UART0_PHYS_BASE	(DEV_BUS_PHYS_BASE | 0x2000)
#define  UART0_VIRT_BASE	(DEV_BUS_VIRT_BASE | 0x2000)
#define  UART1_PHYS_BASE	(DEV_BUS_PHYS_BASE | 0x2100)
#define  UART1_VIRT_BASE	(DEV_BUS_VIRT_BASE | 0x2100)

#define PCIE_VIRT_BASE		(KIRKWOOD_REGS_VIRT_BASE | 0x40000)

#define USB_PHYS_BASE		(KIRKWOOD_REGS_PHYS_BASE | 0x50000)

#define GE00_PHYS_BASE		(KIRKWOOD_REGS_PHYS_BASE | 0x70000)
#define GE01_PHYS_BASE		(KIRKWOOD_REGS_PHYS_BASE | 0x74000)

#define SATA_PHYS_BASE		(KIRKWOOD_REGS_PHYS_BASE | 0x80000)


#define GPIO_MAX		50


#endif
