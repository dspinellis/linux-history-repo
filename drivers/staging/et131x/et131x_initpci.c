/*
 * Agere Systems Inc.
 * 10/100/1000 Base-T Ethernet Driver for the ET1301 and ET131x series MACs
 *
 * Copyright © 2005 Agere Systems Inc.
 * All rights reserved.
 *   http://www.agere.com
 *
 *------------------------------------------------------------------------------
 *
 * et131x_initpci.c - Routines and data used to register the driver with the
 *                    PCI (and PCI Express) subsystem, as well as basic driver
 *                    init and startup.
 *
 *------------------------------------------------------------------------------
 *
 * SOFTWARE LICENSE
 *
 * This software is provided subject to the following terms and conditions,
 * which you should read carefully before using the software.  Using this
 * software indicates your acceptance of these terms and conditions.  If you do
 * not agree with these terms and conditions, do not use the software.
 *
 * Copyright © 2005 Agere Systems Inc.
 * All rights reserved.
 *
 * Redistribution and use in source or binary forms, with or without
 * modifications, are permitted provided that the following conditions are met:
 *
 * . Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following Disclaimer as comments in the code as
 *    well as in the documentation and/or other materials provided with the
 *    distribution.
 *
 * . Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following Disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * . Neither the name of Agere Systems Inc. nor the names of the contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * Disclaimer
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, INFRINGEMENT AND THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  ANY
 * USE, MODIFICATION OR DISTRIBUTION OF THIS SOFTWARE IS SOLELY AT THE USERS OWN
 * RISK. IN NO EVENT SHALL AGERE SYSTEMS INC. OR CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, INCLUDING, BUT NOT LIMITED TO, CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 *
 */

#include "et131x_version.h"
#include "et131x_defs.h"

#include <linux/pci.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>

#include <linux/sched.h>
#include <linux/ptrace.h>
#include <linux/slab.h>
#include <linux/ctype.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/interrupt.h>
#include <linux/in.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/bitops.h>
#include <asm/system.h>

#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/if_arp.h>
#include <linux/ioport.h>
#include <linux/random.h>

#include "et1310_phy.h"
#include "et1310_pm.h"
#include "et1310_jagcore.h"

#include "et131x_adapter.h"
#include "et131x_netdev.h"
#include "et131x_config.h"
#include "et131x_isr.h"

#include "et1310_address_map.h"
#include "et1310_tx.h"
#include "et1310_rx.h"
#include "et1310_mac.h"
#include "et1310_eeprom.h"


/* Defines for Parameter Default/Min/Max vaules */
#define PARM_SPEED_DUPLEX_MIN   0
#define PARM_SPEED_DUPLEX_MAX   5

/* Module parameter for disabling NMI
 * et131x_nmi_disable :
 * Disable NMI (0-2) [0]
 *  0 :
 *  1 :
 *  2 :
 */
static u32 et131x_nmi_disable;	/* 0-2 */
module_param(et131x_nmi_disable, uint, 0);
MODULE_PARM_DESC(et131x_nmi_disable, "Disable NMI (0-2) [0]");

/* Module parameter for manual speed setting
 * Set Link speed and dublex manually (0-5)  [0]
 *  1 : 10Mb   Half-Duplex
 *  2 : 10Mb   Full-Duplex
 *  3 : 100Mb  Half-Duplex
 *  4 : 100Mb  Full-Duplex
 *  5 : 1000Mb Full-Duplex
 *  0 : Auto Speed Auto Duplex // default
 */
static u32 et131x_speed_set;
module_param(et131x_speed_set, uint, 0);
MODULE_PARM_DESC(et131x_speed_set,
		"Set Link speed and dublex manually (0-5)  [0] \n  1 : 10Mb   Half-Duplex \n  2 : 10Mb   Full-Duplex \n  3 : 100Mb  Half-Duplex \n  4 : 100Mb  Full-Duplex \n  5 : 1000Mb Full-Duplex \n 0 : Auto Speed Auto Dublex");

/**
 * et131x_find_adapter - Find the adapter and get all the assigned resources
 * @adapter: pointer to our private adapter structure
 *
 * Returns 0 on success, errno on failure (as defined in errno.h)
 */
int et131x_find_adapter(struct et131x_adapter *adapter, struct pci_dev *pdev)
{
	int result;
	uint8_t maxPayload = 0;
	uint8_t read_size_reg;

	/* Allow disabling of Non-Maskable Interrupts in I/O space, to
	 * support validation.
	 */
	if (adapter->RegistryNMIDisable) {
		uint8_t RegisterVal;

		RegisterVal = inb(ET1310_NMI_DISABLE);
		RegisterVal &= 0xf3;

		if (adapter->RegistryNMIDisable == 2)
			RegisterVal |= 0xc;

		outb(ET1310_NMI_DISABLE, RegisterVal);
	}

	if (et131x_init_eeprom(adapter) < 0)
		return -EIO;

	/* Let's set up the PORT LOGIC Register.  First we need to know what
	 * the max_payload_size is
	 */
	result = pci_read_config_byte(pdev, ET1310_PCI_MAX_PYLD, &maxPayload);
	if (result != PCIBIOS_SUCCESSFUL) {
		dev_err(&pdev->dev,
		    "Could not read PCI config space for Max Payload Size\n");
		return -EIO;
	}

	/* Program the Ack/Nak latency and replay timers */
	maxPayload &= 0x07;	/* Only the lower 3 bits are valid */

	if (maxPayload < 2) {
		const uint16_t AckNak[2] = { 0x76, 0xD0 };
		const uint16_t Replay[2] = { 0x1E0, 0x2ED };

		result = pci_write_config_word(pdev, ET1310_PCI_ACK_NACK,
					       AckNak[maxPayload]);
		if (result != PCIBIOS_SUCCESSFUL) {
			dev_err(&pdev->dev,
			  "Could not write PCI config space for ACK/NAK\n");
			return -EIO;
		}

		result = pci_write_config_word(pdev, ET1310_PCI_REPLAY,
					       Replay[maxPayload]);
		if (result != PCIBIOS_SUCCESSFUL) {
			dev_err(&pdev->dev,
			  "Could not write PCI config space for Replay Timer\n");
			return -EIO;
		}
	}

	/* l0s and l1 latency timers.  We are using default values.
	 * Representing 001 for L0s and 010 for L1
	 */
	result = pci_write_config_byte(pdev, ET1310_PCI_L0L1LATENCY, 0x11);
	if (result != PCIBIOS_SUCCESSFUL) {
		dev_err(&pdev->dev,
		  "Could not write PCI config space for Latency Timers\n");
		return -EIO;
	}

	/* Change the max read size to 2k */
	result = pci_read_config_byte(pdev, 0x51, &read_size_reg);
	if (result != PCIBIOS_SUCCESSFUL) {
		dev_err(&pdev->dev,
			"Could not read PCI config space for Max read size\n");
		return -EIO;
	}

	read_size_reg &= 0x8f;
	read_size_reg |= 0x40;

	result = pci_write_config_byte(pdev, 0x51, read_size_reg);
	if (result != PCIBIOS_SUCCESSFUL) {
		dev_err(&pdev->dev,
		      "Could not write PCI config space for Max read size\n");
		return -EIO;
	}

	/* Get MAC address from config space if an eeprom exists, otherwise
	 * the MAC address there will not be valid
	 */
	if (adapter->has_eeprom) {
		int i;

		for (i = 0; i < ETH_ALEN; i++) {
			result = pci_read_config_byte(
					pdev, ET1310_PCI_MAC_ADDRESS + i,
					adapter->PermanentAddress + i);
			if (result != PCIBIOS_SUCCESSFUL) {
				dev_err(&pdev->dev, ";Could not read PCI config space for MAC address\n");
				return -EIO;
			}
		}
	}
	return 0;
}

/**
 * et131x_error_timer_handler
 * @data: timer-specific variable; here a pointer to our adapter structure
 *
 * The routine called when the error timer expires, to track the number of
 * recurring errors.
 */
void et131x_error_timer_handler(unsigned long data)
{
	struct et131x_adapter *etdev = (struct et131x_adapter *) data;
	u32 pm_csr;

	pm_csr = readl(&etdev->regs->global.pm_csr);

	if ((pm_csr & ET_PM_PHY_SW_COMA) == 0)
		UpdateMacStatHostCounters(etdev);
	else
		dev_err(&etdev->pdev->dev,
		    "No interrupts, in PHY coma, pm_csr = 0x%x\n", pm_csr);

	if (!etdev->Bmsr.bits.link_status &&
	    etdev->RegistryPhyComa &&
	    etdev->PoMgmt.TransPhyComaModeOnBoot < 11) {
		etdev->PoMgmt.TransPhyComaModeOnBoot++;
	}

	if (etdev->PoMgmt.TransPhyComaModeOnBoot == 10) {
		if (!etdev->Bmsr.bits.link_status
		    && etdev->RegistryPhyComa) {
			if ((pm_csr & ET_PM_PHY_SW_COMA) == 0) {
				/* NOTE - This was originally a 'sync with
				 *  interrupt'. How to do that under Linux?
				 */
				et131x_enable_interrupts(etdev);
				EnablePhyComa(etdev);
			}
		}
	}

	/* This is a periodic timer, so reschedule */
	mod_timer(&etdev->ErrorTimer, jiffies +
					  TX_ERROR_PERIOD * HZ / 1000);
}

/**
 * et131x_link_detection_handler
 *
 * Timer function for link up at driver load time
 */
void et131x_link_detection_handler(unsigned long data)
{
	struct et131x_adapter *etdev = (struct et131x_adapter *) data;
	unsigned long flags;

	if (etdev->MediaState == 0) {
		spin_lock_irqsave(&etdev->Lock, flags);

		etdev->MediaState = NETIF_STATUS_MEDIA_DISCONNECT;
		etdev->Flags &= ~fMP_ADAPTER_LINK_DETECTION;

		spin_unlock_irqrestore(&etdev->Lock, flags);

		netif_carrier_off(etdev->netdev);
	}
}

/**
 * et131x_configure_global_regs	-	configure JAGCore global regs
 * @etdev: pointer to our adapter structure
 *
 * Used to configure the global registers on the JAGCore
 */
void ConfigGlobalRegs(struct et131x_adapter *etdev)
{
	struct _GLOBAL_t __iomem *regs = &etdev->regs->global;

	if (etdev->RegistryPhyLoopbk == false) {
		if (etdev->RegistryJumboPacket < 2048) {
			/* Tx / RxDMA and Tx/Rx MAC interfaces have a 1k word
			 * block of RAM that the driver can split between Tx
			 * and Rx as it desires.  Our default is to split it
			 * 50/50:
			 */
			writel(0, &regs->rxq_start_addr);
			writel(PARM_RX_MEM_END_DEF, &regs->rxq_end_addr);
			writel(PARM_RX_MEM_END_DEF + 1, &regs->txq_start_addr);
			writel(INTERNAL_MEM_SIZE - 1, &regs->txq_end_addr);
		} else if (etdev->RegistryJumboPacket < 8192) {
			/* For jumbo packets > 2k but < 8k, split 50-50. */
			writel(0, &regs->rxq_start_addr);
			writel(INTERNAL_MEM_RX_OFFSET, &regs->rxq_end_addr);
			writel(INTERNAL_MEM_RX_OFFSET + 1, &regs->txq_start_addr);
			writel(INTERNAL_MEM_SIZE - 1, &regs->txq_end_addr);
		} else {
			/* 9216 is the only packet size greater than 8k that
			 * is available. The Tx buffer has to be big enough
			 * for one whole packet on the Tx side. We'll make
			 * the Tx 9408, and give the rest to Rx
			 */
			writel(0x0000, &regs->rxq_start_addr);
			writel(0x01b3, &regs->rxq_end_addr);
			writel(0x01b4, &regs->txq_start_addr);
			writel(INTERNAL_MEM_SIZE - 1,&regs->txq_end_addr);
		}

		/* Initialize the loopback register. Disable all loopbacks. */
		writel(0, &regs->loopback);
	} else {
		/* For PHY Line loopback, the memory is configured as if Tx
		 * and Rx both have all the memory.  This is because the
		 * RxMAC will write data into the space, and the TxMAC will
		 * read it out.
		 */
		writel(0, &regs->rxq_start_addr);
		writel(INTERNAL_MEM_SIZE - 1, &regs->rxq_end_addr);
		writel(0, &regs->txq_start_addr);
		writel(INTERNAL_MEM_SIZE - 1, &regs->txq_end_addr);

		/* Initialize the loopback register (MAC loopback). */
		writel(ET_LOOP_MAC, &regs->loopback);
	}

	/* MSI Register */
	writel(0, &regs->msi_config);

	/* By default, disable the watchdog timer.  It will be enabled when
	 * a packet is queued.
	 */
	writel(0, &regs->watchdog_timer);
}


/**
 * et131x_adapter_setup - Set the adapter up as per cassini+ documentation
 * @adapter: pointer to our private adapter structure
 *
 * Returns 0 on success, errno on failure (as defined in errno.h)
 */
int et131x_adapter_setup(struct et131x_adapter *etdev)
{
	int status = 0;

	/* Configure the JAGCore */
	ConfigGlobalRegs(etdev);

	ConfigMACRegs1(etdev);

	/* Configure the MMC registers */
	/* All we need to do is initialize the Memory Control Register */
	writel(ET_MMC_ENABLE, &etdev->regs->mmc.mmc_ctrl);

	ConfigRxMacRegs(etdev);
	ConfigTxMacRegs(etdev);

	ConfigRxDmaRegs(etdev);
	ConfigTxDmaRegs(etdev);

	ConfigMacStatRegs(etdev);

	/* Move the following code to Timer function?? */
	status = et131x_xcvr_find(etdev);

	if (status != 0)
		dev_warn(&etdev->pdev->dev, "Could not find the xcvr\n");

	/* Prepare the TRUEPHY library. */
	ET1310_PhyInit(etdev);

	/* Reset the phy now so changes take place */
	ET1310_PhyReset(etdev);

	/* Power down PHY */
	ET1310_PhyPowerDown(etdev, 1);

	/*
	 * We need to turn off 1000 base half dulplex, the mac does not
	 * support it. For the 10/100 part, turn off all gig advertisement
	 */
	if (etdev->pdev->device != ET131X_PCI_DEVICE_ID_FAST)
		ET1310_PhyAdvertise1000BaseT(etdev, TRUEPHY_ADV_DUPLEX_FULL);
	else
		ET1310_PhyAdvertise1000BaseT(etdev, TRUEPHY_ADV_DUPLEX_NONE);

	/* Power up PHY */
	ET1310_PhyPowerDown(etdev, 0);

	et131x_setphy_normal(etdev);
;	return status;
}

/**
 * et131x_setup_hardware_properties - set up the MAC Address on the ET1310
 * @adapter: pointer to our private adapter structure
 */
void et131x_setup_hardware_properties(struct et131x_adapter *adapter)
{
	/* If have our default mac from registry and no mac address from
	 * EEPROM then we need to generate the last octet and set it on the
	 * device
	 */
	if (adapter->PermanentAddress[0] == 0x00 &&
	    adapter->PermanentAddress[1] == 0x00 &&
	    adapter->PermanentAddress[2] == 0x00 &&
	    adapter->PermanentAddress[3] == 0x00 &&
	    adapter->PermanentAddress[4] == 0x00 &&
	    adapter->PermanentAddress[5] == 0x00) {
		/*
		 * We need to randomly generate the last octet so we
		 * decrease our chances of setting the mac address to
		 * same as another one of our cards in the system
		 */
		get_random_bytes(&adapter->CurrentAddress[5], 1);
		/*
		 * We have the default value in the register we are
		 * working with so we need to copy the current
		 * address into the permanent address
		 */
		memcpy(adapter->PermanentAddress,
			adapter->CurrentAddress, ETH_ALEN);
	} else {
		/* We do not have an override address, so set the
		 * current address to the permanent address and add
		 * it to the device
		 */
		memcpy(adapter->CurrentAddress,
		       adapter->PermanentAddress, ETH_ALEN);
	}
}

/**
 * et131x_soft_reset - Issue a soft reset to the hardware, complete for ET1310
 * @adapter: pointer to our private adapter structure
 */
void et131x_soft_reset(struct et131x_adapter *adapter)
{
	/* Disable MAC Core */
	writel(0xc00f0000, &adapter->regs->mac.cfg1.value);

	/* Set everything to a reset value */
	writel(0x7F, &adapter->regs->global.sw_reset);
	writel(0x000f0000, &adapter->regs->mac.cfg1.value);
	writel(0x00000000, &adapter->regs->mac.cfg1.value);
}

/**
 * et131x_align_allocated_memory - Align allocated memory on a given boundary
 * @adapter: pointer to our adapter structure
 * @phys_addr: pointer to Physical address
 * @offset: pointer to the offset variable
 * @mask: correct mask
 */
void et131x_align_allocated_memory(struct et131x_adapter *adapter,
				   uint64_t *phys_addr,
				   uint64_t *offset, uint64_t mask)
{
	uint64_t new_addr;

	*offset = 0;

	new_addr = *phys_addr & ~mask;

	if (new_addr != *phys_addr) {
		/* Move to next aligned block */
		new_addr += mask + 1;
		/* Return offset for adjusting virt addr */
		*offset = new_addr - *phys_addr;
		/* Return new physical address */
		*phys_addr = new_addr;
	}
}

/**
 * et131x_adapter_memory_alloc
 * @adapter: pointer to our private adapter structure
 *
 * Returns 0 on success, errno on failure (as defined in errno.h).
 *
 * Allocate all the memory blocks for send, receive and others.
 */
int et131x_adapter_memory_alloc(struct et131x_adapter *adapter)
{
	int status = 0;

	do {
		/* Allocate memory for the Tx Ring */
		status = et131x_tx_dma_memory_alloc(adapter);
		if (status != 0) {
			dev_err(&adapter->pdev->dev,
				  "et131x_tx_dma_memory_alloc FAILED\n");
			break;
		}

		/* Receive buffer memory allocation */
		status = et131x_rx_dma_memory_alloc(adapter);
		if (status != 0) {
			dev_err(&adapter->pdev->dev,
				  "et131x_rx_dma_memory_alloc FAILED\n");
			et131x_tx_dma_memory_free(adapter);
			break;
		}

		/* Init receive data structures */
		status = et131x_init_recv(adapter);
		if (status != 0) {
			dev_err(&adapter->pdev->dev,
				"et131x_init_recv FAILED\n");
			et131x_tx_dma_memory_free(adapter);
			et131x_rx_dma_memory_free(adapter);
			break;
		}
	} while (0);
	return status;
}

/**
 * et131x_adapter_memory_free - Free all memory allocated for use by Tx & Rx
 * @adapter: pointer to our private adapter structure
 */
void et131x_adapter_memory_free(struct et131x_adapter *adapter)
{
	/* Free DMA memory */
	et131x_tx_dma_memory_free(adapter);
	et131x_rx_dma_memory_free(adapter);
}

/**
 * et131x_config_parse
 * @etdev: pointer to the private adapter struct
 *
 * Parses a configuration from some location (module parameters, for example)
 * into the private adapter struct. This really has no sensible analogy in
 * Linux as sysfs parameters are dynamic. Several things that were hee could
 * go into sysfs, but other stuff like speed handling is part of the mii
 * interfaces/ethtool.
 */
void et131x_config_parse(struct et131x_adapter *etdev)
{
	static const u8 default_mac[] = { 0x00, 0x05, 0x3d, 0x00, 0x02, 0x00 };
	static const u8 duplex[] = { 0, 1, 2, 1, 2, 2 };
	static const u16 speed[] = { 0, 10, 10, 100, 100, 1000 };

	if (et131x_speed_set)
		dev_info(&etdev->pdev->dev,
			"Speed set manually to : %d \n", et131x_speed_set);

	etdev->SpeedDuplex = et131x_speed_set;
	etdev->RegistryJumboPacket = 1514;	/* 1514-9216 */

	etdev->RegistryNMIDisable = et131x_nmi_disable;

	/* Set the MAC address to a default */
	memcpy(etdev->CurrentAddress, default_mac, ETH_ALEN);

	/* Decode SpeedDuplex
	 *
	 * Set up as if we are auto negotiating always and then change if we
	 * go into force mode
	 *
	 * If we are the 10/100 device, and gigabit is somehow requested then
	 * knock it down to 100 full.
	 */
	if (etdev->pdev->device == ET131X_PCI_DEVICE_ID_FAST &&
	    etdev->SpeedDuplex == 5)
		etdev->SpeedDuplex = 4;

	etdev->AiForceSpeed = speed[etdev->SpeedDuplex];
	etdev->AiForceDpx = duplex[etdev->SpeedDuplex];	/* Auto FDX */
}


/**
 * et131x_pci_remove
 * @pdev: a pointer to the device's pci_dev structure
 *
 * Registered in the pci_driver structure, this function is called when the
 * PCI subsystem detects that a PCI device which matches the information
 * contained in the pci_device_id table has been removed.
 */

void __devexit et131x_pci_remove(struct pci_dev *pdev)
{
	struct net_device *netdev;
	struct et131x_adapter *adapter;

	/* Retrieve the net_device pointer from the pci_dev struct, as well
	 * as the private adapter struct
	 */
	netdev = (struct net_device *) pci_get_drvdata(pdev);
	adapter = netdev_priv(netdev);

	/* Perform device cleanup */
	unregister_netdev(netdev);
	et131x_adapter_memory_free(adapter);
	iounmap(adapter->regs);
	pci_dev_put(adapter->pdev);
	free_netdev(netdev);
	pci_release_regions(pdev);
	pci_disable_device(pdev);
}


/**
 * et131x_pci_setup - Perform device initialization
 * @pdev: a pointer to the device's pci_dev structure
 * @ent: this device's entry in the pci_device_id table
 *
 * Returns 0 on success, errno on failure (as defined in errno.h)
 *
 * Registered in the pci_driver structure, this function is called when the
 * PCI subsystem finds a new PCI device which matches the information
 * contained in the pci_device_id table. This routine is the equivalent to
 * a device insertion routine.
 */

int __devinit et131x_pci_setup(struct pci_dev *pdev,
			       const struct pci_device_id *ent)
{
	int result = 0;
	int pm_cap;
	bool pci_using_dac;
	struct net_device *netdev = NULL;
	struct et131x_adapter *adapter = NULL;

	/* Enable the device via the PCI subsystem */
	result = pci_enable_device(pdev);
	if (result != 0) {
		dev_err(&adapter->pdev->dev,
			"pci_enable_device() failed\n");
		goto out;
	}

	/* Perform some basic PCI checks */
	if (!(pci_resource_flags(pdev, 0) & IORESOURCE_MEM)) {
		dev_err(&adapter->pdev->dev,
			  "Can't find PCI device's base address\n");
		result = -ENODEV;
		goto out;
	}

	result = pci_request_regions(pdev, DRIVER_NAME);
	if (result != 0) {
		dev_err(&adapter->pdev->dev,
			"Can't get PCI resources\n");
		goto err_disable;
	}

	/* Enable PCI bus mastering */
	pci_set_master(pdev);

	/* Query PCI for Power Mgmt Capabilities
	 *
	 * NOTE: Now reading PowerMgmt in another location; is this still
	 * needed?
	 */
	pm_cap = pci_find_capability(pdev, PCI_CAP_ID_PM);
	if (pm_cap == 0) {
		dev_err(&adapter->pdev->dev,
			  "Cannot find Power Management capabilities\n");
		result = -EIO;
		goto err_release_res;
	}

	/* Check the DMA addressing support of this device */
	if (!pci_set_dma_mask(pdev, 0xffffffffffffffffULL)) {
		pci_using_dac = true;

		result =
		    pci_set_consistent_dma_mask(pdev, 0xffffffffffffffffULL);
		if (result != 0) {
			dev_err(&pdev->dev,
				  "Unable to obtain 64 bit DMA for consistent allocations\n");
			goto err_release_res;
		}
	} else if (!pci_set_dma_mask(pdev, 0xffffffffULL)) {
		pci_using_dac = false;
	} else {
		dev_err(&adapter->pdev->dev,
			"No usable DMA addressing method\n");
		result = -EIO;
		goto err_release_res;
	}

	/* Allocate netdev and private adapter structs */
	netdev = et131x_device_alloc();
	if (netdev == NULL) {
		dev_err(&adapter->pdev->dev,
			"Couldn't alloc netdev struct\n");
		result = -ENOMEM;
		goto err_release_res;
	}

	/* Setup the fundamental net_device and private adapter structure elements  */
	SET_NETDEV_DEV(netdev, &pdev->dev);
	/*
	if (pci_using_dac) {
		netdev->features |= NETIF_F_HIGHDMA;
	}
	*/

	/*
	 * NOTE - Turn this on when we're ready to deal with SG-DMA
	 *
	 * NOTE: According to "Linux Device Drivers", 3rd ed, Rubini et al,
	 * if checksumming is not performed in HW, then the kernel will not
	 * use SG.
	 * From pp 510-511:
	 *
	 * "Note that the kernel does not perform scatter/gather I/O to your
	 * device if it does not also provide some form of checksumming as
	 * well. The reason is that, if the kernel has to make a pass over a
	 * fragmented ("nonlinear") packet to calculate the checksum, it
	 * might as well copy the data and coalesce the packet at the same
	 * time."
	 *
	 * This has been verified by setting the flags below and still not
	 * receiving a scattered buffer from the network stack, so leave it
	 * off until checksums are calculated in HW.
	 */
	/* netdev->features |= NETIF_F_SG; */
	/* netdev->features |= NETIF_F_NO_CSUM; */
	/* netdev->features |= NETIF_F_LLTX; */

	/* Allocate private adapter struct and copy in relevant information */
	adapter = netdev_priv(netdev);
	adapter->pdev = pci_dev_get(pdev);
	adapter->netdev = netdev;

	/* Do the same for the netdev struct */
	netdev->irq = pdev->irq;
	netdev->base_addr = pdev->resource[0].start;

	/* Initialize spinlocks here */
	spin_lock_init(&adapter->Lock);
	spin_lock_init(&adapter->TCBSendQLock);
	spin_lock_init(&adapter->TCBReadyQLock);
	spin_lock_init(&adapter->SendHWLock);
	spin_lock_init(&adapter->SendWaitLock);
	spin_lock_init(&adapter->RcvLock);
	spin_lock_init(&adapter->RcvPendLock);
	spin_lock_init(&adapter->FbrLock);
	spin_lock_init(&adapter->PHYLock);

	/* Parse configuration parameters into the private adapter struct */
	et131x_config_parse(adapter);

	/* Find the physical adapter
	 *
	 * NOTE: This is the equivalent of the MpFindAdapter() routine; can we
	 *       lump it's init with the device specific init below into a
	 *       single init function?
	 */
	/* while (et131x_find_adapter(adapter, pdev) != 0); */
	et131x_find_adapter(adapter, pdev);

	/* Map the bus-relative registers to system virtual memory */

	adapter->regs = ioremap_nocache(pci_resource_start(pdev, 0),
					      pci_resource_len(pdev, 0));
	if (adapter->regs == NULL) {
		dev_err(&pdev->dev, "Cannot map device registers\n");
		result = -ENOMEM;
		goto err_free_dev;
	}

	/* Perform device-specific initialization here (See code below) */

	/* If Phy COMA mode was enabled when we went down, disable it here. */
	writel(ET_PMCSR_INIT,  &adapter->regs->global.pm_csr);

	/* Issue a global reset to the et1310 */
	et131x_soft_reset(adapter);

	/* Disable all interrupts (paranoid) */
	et131x_disable_interrupts(adapter);

	/* Allocate DMA memory */
	result = et131x_adapter_memory_alloc(adapter);
	if (result != 0) {
		dev_err(&pdev->dev, "Could not alloc adapater memory (DMA)\n");
		goto err_iounmap;
	}

	/* Init send data structures */
	et131x_init_send(adapter);

	/* Register the interrupt
	 *
	 * NOTE - This is being done in the open routine, where most other
	 *         Linux drivers setup IRQ handlers. Make sure device
	 *         interrupts are not turned on before the IRQ is registered!!
	 *
	 *         What we will do here is setup the task structure for the
	 *         ISR's deferred handler
	 */
	INIT_WORK(&adapter->task, et131x_isr_handler);

	/* Determine MAC Address, and copy into the net_device struct */
	et131x_setup_hardware_properties(adapter);

	memcpy(netdev->dev_addr, adapter->CurrentAddress, ETH_ALEN);

	/* Setup et1310 as per the documentation */
	et131x_adapter_setup(adapter);

	/* Create a timer to count errors received by the NIC */
	init_timer(&adapter->ErrorTimer);

	adapter->ErrorTimer.expires = jiffies + TX_ERROR_PERIOD * HZ / 1000;
	adapter->ErrorTimer.function = et131x_error_timer_handler;
	adapter->ErrorTimer.data = (unsigned long)adapter;

	/* Initialize link state */
	et131x_link_detection_handler((unsigned long)adapter);

	/* Intialize variable for counting how long we do not have
							link status */
	adapter->PoMgmt.TransPhyComaModeOnBoot = 0;

	/* We can enable interrupts now
	 *
	 *  NOTE - Because registration of interrupt handler is done in the
	 *         device's open(), defer enabling device interrupts to that
	 *         point
	 */

	/* Register the net_device struct with the Linux network layer */
	result = register_netdev(netdev);
	if (result != 0) {
		dev_err(&pdev->dev, "register_netdev() failed\n");
		goto err_mem_free;
	}

	/* Register the net_device struct with the PCI subsystem. Save a copy
	 * of the PCI config space for this device now that the device has
	 * been initialized, just in case it needs to be quickly restored.
	 */
	pci_set_drvdata(pdev, netdev);

	pci_save_state(adapter->pdev);

out:
	return result;

err_mem_free:
	et131x_adapter_memory_free(adapter);
err_iounmap:
	iounmap(adapter->regs);
err_free_dev:
	pci_dev_put(pdev);
	free_netdev(netdev);
err_release_res:
	pci_release_regions(pdev);
err_disable:
	pci_disable_device(pdev);
	goto out;
}

static struct pci_device_id et131x_pci_table[] __devinitdata = {
	{ET131X_PCI_VENDOR_ID, ET131X_PCI_DEVICE_ID_GIG, PCI_ANY_ID,
	 PCI_ANY_ID, 0, 0, 0UL},
	{ET131X_PCI_VENDOR_ID, ET131X_PCI_DEVICE_ID_FAST, PCI_ANY_ID,
	 PCI_ANY_ID, 0, 0, 0UL},
	{0,}
};

MODULE_DEVICE_TABLE(pci, et131x_pci_table);

static struct pci_driver et131x_driver = {
      .name	= DRIVER_NAME,
      .id_table	= et131x_pci_table,
      .probe	= et131x_pci_setup,
      .remove	= __devexit_p(et131x_pci_remove),
      .suspend	= NULL,		/* et131x_pci_suspend */
      .resume	= NULL,		/* et131x_pci_resume */
};


/**
 * et131x_init_module - The "main" entry point called on driver initialization
 *
 * Returns 0 on success, errno on failure (as defined in errno.h)
 */
static int __init et131x_init_module(void)
{
	if (et131x_speed_set < PARM_SPEED_DUPLEX_MIN ||
	    et131x_speed_set > PARM_SPEED_DUPLEX_MAX) {
		printk(KERN_WARNING "et131x: invalid speed setting ignored.\n");
	    	et131x_speed_set = 0;
	}
	return pci_register_driver(&et131x_driver);
}

/**
 * et131x_cleanup_module - The entry point called on driver cleanup
 */
static void __exit et131x_cleanup_module(void)
{
	pci_unregister_driver(&et131x_driver);
}

module_init(et131x_init_module);
module_exit(et131x_cleanup_module);


/* Modinfo parameters (filled out using defines from et131x_version.h) */
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_INFO);
MODULE_LICENSE(DRIVER_LICENSE);
