/*******************************************************************************

  
  Copyright(c) 1999 - 2005 Intel Corporation. All rights reserved.
  
  This program is free software; you can redistribute it and/or modify it 
  under the terms of the GNU General Public License as published by the Free 
  Software Foundation; either version 2 of the License, or (at your option) 
  any later version.
  
  This program is distributed in the hope that it will be useful, but WITHOUT 
  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or 
  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for 
  more details.
  
  You should have received a copy of the GNU General Public License along with
  this program; if not, write to the Free Software Foundation, Inc., 59 
  Temple Place - Suite 330, Boston, MA  02111-1307, USA.
  
  The full GNU General Public License is included in this distribution in the
  file called LICENSE.
  
  Contact Information:
  Linux NICS <linux.nics@intel.com>
  Intel Corporation, 5200 N.E. Elam Young Parkway, Hillsboro, OR 97124-6497

*******************************************************************************/

/* ethtool support for ixgb */

#include "ixgb.h"

#include <asm/uaccess.h>

extern char ixgb_driver_name[];
extern char ixgb_driver_version[];

extern int ixgb_up(struct ixgb_adapter *adapter);
extern void ixgb_down(struct ixgb_adapter *adapter, boolean_t kill_watchdog);
extern void ixgb_reset(struct ixgb_adapter *adapter);
extern int ixgb_setup_rx_resources(struct ixgb_adapter *adapter);
extern int ixgb_setup_tx_resources(struct ixgb_adapter *adapter);
extern void ixgb_free_rx_resources(struct ixgb_adapter *adapter);
extern void ixgb_free_tx_resources(struct ixgb_adapter *adapter);
extern void ixgb_update_stats(struct ixgb_adapter *adapter);

struct ixgb_stats {
	char stat_string[ETH_GSTRING_LEN];
	int sizeof_stat;
	int stat_offset;
};

#define IXGB_STAT(m) sizeof(((struct ixgb_adapter *)0)->m), \
		      offsetof(struct ixgb_adapter, m)
static struct ixgb_stats ixgb_gstrings_stats[] = {
	{"rx_packets", IXGB_STAT(net_stats.rx_packets)},
	{"tx_packets", IXGB_STAT(net_stats.tx_packets)},
	{"rx_bytes", IXGB_STAT(net_stats.rx_bytes)},
	{"tx_bytes", IXGB_STAT(net_stats.tx_bytes)},
	{"rx_errors", IXGB_STAT(net_stats.rx_errors)},
	{"tx_errors", IXGB_STAT(net_stats.tx_errors)},
	{"rx_dropped", IXGB_STAT(net_stats.rx_dropped)},
	{"tx_dropped", IXGB_STAT(net_stats.tx_dropped)},
	{"multicast", IXGB_STAT(net_stats.multicast)},
	{"collisions", IXGB_STAT(net_stats.collisions)},

/*	{ "rx_length_errors", IXGB_STAT(net_stats.rx_length_errors) },	*/
	{"rx_over_errors", IXGB_STAT(net_stats.rx_over_errors)},
	{"rx_crc_errors", IXGB_STAT(net_stats.rx_crc_errors)},
	{"rx_frame_errors", IXGB_STAT(net_stats.rx_frame_errors)},
	{"rx_fifo_errors", IXGB_STAT(net_stats.rx_fifo_errors)},
	{"rx_missed_errors", IXGB_STAT(net_stats.rx_missed_errors)},
	{"tx_aborted_errors", IXGB_STAT(net_stats.tx_aborted_errors)},
	{"tx_carrier_errors", IXGB_STAT(net_stats.tx_carrier_errors)},
	{"tx_fifo_errors", IXGB_STAT(net_stats.tx_fifo_errors)},
	{"tx_heartbeat_errors", IXGB_STAT(net_stats.tx_heartbeat_errors)},
	{"tx_window_errors", IXGB_STAT(net_stats.tx_window_errors)},
	{"tx_deferred_ok", IXGB_STAT(stats.dc)},
	{"rx_long_length_errors", IXGB_STAT(stats.roc)},
	{"rx_short_length_errors", IXGB_STAT(stats.ruc)},
#ifdef NETIF_F_TSO
	{"tx_tcp_seg_good", IXGB_STAT(stats.tsctc)},
	{"tx_tcp_seg_failed", IXGB_STAT(stats.tsctfc)},
#endif
	{"rx_flow_control_xon", IXGB_STAT(stats.xonrxc)},
	{"rx_flow_control_xoff", IXGB_STAT(stats.xoffrxc)},
	{"tx_flow_control_xon", IXGB_STAT(stats.xontxc)},
	{"tx_flow_control_xoff", IXGB_STAT(stats.xofftxc)},
	{"rx_csum_offload_good", IXGB_STAT(hw_csum_rx_good)},
	{"rx_csum_offload_errors", IXGB_STAT(hw_csum_rx_error)},
	{"tx_csum_offload_good", IXGB_STAT(hw_csum_tx_good)},
	{"tx_csum_offload_errors", IXGB_STAT(hw_csum_tx_error)}
};

#define IXGB_STATS_LEN	\
	sizeof(ixgb_gstrings_stats) / sizeof(struct ixgb_stats)

static int
ixgb_get_settings(struct net_device *netdev, struct ethtool_cmd *ecmd)
{
	struct ixgb_adapter *adapter = netdev_priv(netdev);

	ecmd->supported = (SUPPORTED_10000baseT_Full | SUPPORTED_FIBRE);
	ecmd->advertising = (SUPPORTED_10000baseT_Full | SUPPORTED_FIBRE);
	ecmd->port = PORT_FIBRE;
	ecmd->transceiver = XCVR_EXTERNAL;

	if(netif_carrier_ok(adapter->netdev)) {
		ecmd->speed = SPEED_10000;
		ecmd->duplex = DUPLEX_FULL;
	} else {
		ecmd->speed = -1;
		ecmd->duplex = -1;
	}

	ecmd->autoneg = AUTONEG_DISABLE;
	return 0;
}

static int
ixgb_set_settings(struct net_device *netdev, struct ethtool_cmd *ecmd)
{
	struct ixgb_adapter *adapter = netdev_priv(netdev);

	if(ecmd->autoneg == AUTONEG_ENABLE ||
	   ecmd->speed + ecmd->duplex != SPEED_10000 + DUPLEX_FULL)
		return -EINVAL;
	
	if(netif_running(adapter->netdev)) {
		ixgb_down(adapter, TRUE);
		ixgb_reset(adapter);
		ixgb_up(adapter);
		/* be optimistic about our link, since we were up before */
		adapter->link_speed = 10000;
		adapter->link_duplex = FULL_DUPLEX;
		netif_carrier_on(netdev);
		netif_wake_queue(netdev);
		
	} else
		ixgb_reset(adapter);

	return 0;
}

static void
ixgb_get_pauseparam(struct net_device *netdev,
			 struct ethtool_pauseparam *pause)
{
	struct ixgb_adapter *adapter = netdev_priv(netdev);
	struct ixgb_hw *hw = &adapter->hw;
	
	pause->autoneg = AUTONEG_DISABLE;
		
	if(hw->fc.type == ixgb_fc_rx_pause)
		pause->rx_pause = 1;
	else if(hw->fc.type == ixgb_fc_tx_pause)
		pause->tx_pause = 1;
	else if(hw->fc.type == ixgb_fc_full) {
		pause->rx_pause = 1;
		pause->tx_pause = 1;
	}
}

static int
ixgb_set_pauseparam(struct net_device *netdev,
			 struct ethtool_pauseparam *pause)
{
	struct ixgb_adapter *adapter = netdev_priv(netdev);
	struct ixgb_hw *hw = &adapter->hw;
	
	if(pause->autoneg == AUTONEG_ENABLE)
		return -EINVAL;

	if(pause->rx_pause && pause->tx_pause)
		hw->fc.type = ixgb_fc_full;
	else if(pause->rx_pause && !pause->tx_pause)
		hw->fc.type = ixgb_fc_rx_pause;
	else if(!pause->rx_pause && pause->tx_pause)
		hw->fc.type = ixgb_fc_tx_pause;
	else if(!pause->rx_pause && !pause->tx_pause)
		hw->fc.type = ixgb_fc_none;

	if(netif_running(adapter->netdev)) {
		ixgb_down(adapter, TRUE);
		ixgb_up(adapter);
		/* be optimistic about our link, since we were up before */
		adapter->link_speed = 10000;
		adapter->link_duplex = FULL_DUPLEX;
		netif_carrier_on(netdev);
		netif_wake_queue(netdev);
	} else
		ixgb_reset(adapter);
		
	return 0;
}

static uint32_t
ixgb_get_rx_csum(struct net_device *netdev)
{
	struct ixgb_adapter *adapter = netdev_priv(netdev);

	return adapter->rx_csum;
}

static int
ixgb_set_rx_csum(struct net_device *netdev, uint32_t data)
{
	struct ixgb_adapter *adapter = netdev_priv(netdev);

	adapter->rx_csum = data;

	if(netif_running(netdev)) {
		ixgb_down(adapter,TRUE);
		ixgb_up(adapter);
		/* be optimistic about our link, since we were up before */
		adapter->link_speed = 10000;
		adapter->link_duplex = FULL_DUPLEX;
		netif_carrier_on(netdev);
		netif_wake_queue(netdev);
	} else
		ixgb_reset(adapter);
	return 0;
}
	
static uint32_t
ixgb_get_tx_csum(struct net_device *netdev)
{
	return (netdev->features & NETIF_F_HW_CSUM) != 0;
}

static int
ixgb_set_tx_csum(struct net_device *netdev, uint32_t data)
{
	if (data)
		netdev->features |= NETIF_F_HW_CSUM;
	else
		netdev->features &= ~NETIF_F_HW_CSUM;

	return 0;
}

#ifdef NETIF_F_TSO
static int
ixgb_set_tso(struct net_device *netdev, uint32_t data)
{
	if(data)
		netdev->features |= NETIF_F_TSO;
	else
		netdev->features &= ~NETIF_F_TSO;
	return 0;
} 
#endif /* NETIF_F_TSO */

#define IXGB_GET_STAT(_A_, _R_) _A_->stats._R_

static int 
ixgb_get_regs_len(struct net_device *netdev)
{
#define IXGB_REG_DUMP_LEN  136*sizeof(uint32_t)
	return IXGB_REG_DUMP_LEN;
}

static void
ixgb_get_regs(struct net_device *netdev,
		   struct ethtool_regs *regs, void *p)
{
	struct ixgb_adapter *adapter = netdev_priv(netdev);
	struct ixgb_hw *hw = &adapter->hw;
	uint32_t *reg = p;
	uint32_t *reg_start = reg;
	uint8_t i;

	/* the 1 (one) below indicates an attempt at versioning, if the
	 * interface in ethtool or the driver this 1 should be incremented */
	regs->version = (1<<24) | hw->revision_id << 16 | hw->device_id;

	/* General Registers */
	*reg++ = IXGB_READ_REG(hw, CTRL0);	/*   0 */
	*reg++ = IXGB_READ_REG(hw, CTRL1);	/*   1 */
	*reg++ = IXGB_READ_REG(hw, STATUS);	/*   2 */
	*reg++ = IXGB_READ_REG(hw, EECD);	/*   3 */
	*reg++ = IXGB_READ_REG(hw, MFS);	/*   4 */

	/* Interrupt */
	*reg++ = IXGB_READ_REG(hw, ICR);	/*   5 */
	*reg++ = IXGB_READ_REG(hw, ICS);	/*   6 */
	*reg++ = IXGB_READ_REG(hw, IMS);	/*   7 */
	*reg++ = IXGB_READ_REG(hw, IMC);	/*   8 */

	/* Receive */
	*reg++ = IXGB_READ_REG(hw, RCTL);	/*   9 */
	*reg++ = IXGB_READ_REG(hw, FCRTL);	/*  10 */
	*reg++ = IXGB_READ_REG(hw, FCRTH);	/*  11 */
	*reg++ = IXGB_READ_REG(hw, RDBAL);	/*  12 */
	*reg++ = IXGB_READ_REG(hw, RDBAH);	/*  13 */
	*reg++ = IXGB_READ_REG(hw, RDLEN);	/*  14 */
	*reg++ = IXGB_READ_REG(hw, RDH);	/*  15 */
	*reg++ = IXGB_READ_REG(hw, RDT);	/*  16 */
	*reg++ = IXGB_READ_REG(hw, RDTR);	/*  17 */
	*reg++ = IXGB_READ_REG(hw, RXDCTL);	/*  18 */
	*reg++ = IXGB_READ_REG(hw, RAIDC);	/*  19 */
	*reg++ = IXGB_READ_REG(hw, RXCSUM);	/*  20 */

	/* there are 16 RAR entries in hardware, we only use 3 */
	for(i = 0; i < 16; i++) {
		*reg++ = IXGB_READ_REG_ARRAY(hw, RAL, (i << 1)); /*21,...,51 */
		*reg++ = IXGB_READ_REG_ARRAY(hw, RAH, (i << 1)); /*22,...,52 */
	}

	/* Transmit */
	*reg++ = IXGB_READ_REG(hw, TCTL);	/*  53 */
	*reg++ = IXGB_READ_REG(hw, TDBAL);	/*  54 */
	*reg++ = IXGB_READ_REG(hw, TDBAH);	/*  55 */
	*reg++ = IXGB_READ_REG(hw, TDLEN);	/*  56 */
	*reg++ = IXGB_READ_REG(hw, TDH);	/*  57 */
	*reg++ = IXGB_READ_REG(hw, TDT);	/*  58 */
	*reg++ = IXGB_READ_REG(hw, TIDV);	/*  59 */
	*reg++ = IXGB_READ_REG(hw, TXDCTL);	/*  60 */
	*reg++ = IXGB_READ_REG(hw, TSPMT);	/*  61 */
	*reg++ = IXGB_READ_REG(hw, PAP);	/*  62 */

	/* Physical */
	*reg++ = IXGB_READ_REG(hw, PCSC1);	/*  63 */
	*reg++ = IXGB_READ_REG(hw, PCSC2);	/*  64 */
	*reg++ = IXGB_READ_REG(hw, PCSS1);	/*  65 */
	*reg++ = IXGB_READ_REG(hw, PCSS2);	/*  66 */
	*reg++ = IXGB_READ_REG(hw, XPCSS);	/*  67 */
	*reg++ = IXGB_READ_REG(hw, UCCR);	/*  68 */
	*reg++ = IXGB_READ_REG(hw, XPCSTC);	/*  69 */
	*reg++ = IXGB_READ_REG(hw, MACA);	/*  70 */
	*reg++ = IXGB_READ_REG(hw, APAE);	/*  71 */
	*reg++ = IXGB_READ_REG(hw, ARD);	/*  72 */
	*reg++ = IXGB_READ_REG(hw, AIS);	/*  73 */
	*reg++ = IXGB_READ_REG(hw, MSCA);	/*  74 */
	*reg++ = IXGB_READ_REG(hw, MSRWD);	/*  75 */

	/* Statistics */
	*reg++ = IXGB_GET_STAT(adapter, tprl);	/*  76 */
	*reg++ = IXGB_GET_STAT(adapter, tprh);	/*  77 */
	*reg++ = IXGB_GET_STAT(adapter, gprcl);	/*  78 */
	*reg++ = IXGB_GET_STAT(adapter, gprch);	/*  79 */
	*reg++ = IXGB_GET_STAT(adapter, bprcl);	/*  80 */
	*reg++ = IXGB_GET_STAT(adapter, bprch);	/*  81 */
	*reg++ = IXGB_GET_STAT(adapter, mprcl);	/*  82 */
	*reg++ = IXGB_GET_STAT(adapter, mprch);	/*  83 */
	*reg++ = IXGB_GET_STAT(adapter, uprcl);	/*  84 */
	*reg++ = IXGB_GET_STAT(adapter, uprch);	/*  85 */
	*reg++ = IXGB_GET_STAT(adapter, vprcl);	/*  86 */
	*reg++ = IXGB_GET_STAT(adapter, vprch);	/*  87 */
	*reg++ = IXGB_GET_STAT(adapter, jprcl);	/*  88 */
	*reg++ = IXGB_GET_STAT(adapter, jprch);	/*  89 */
	*reg++ = IXGB_GET_STAT(adapter, gorcl);	/*  90 */
	*reg++ = IXGB_GET_STAT(adapter, gorch);	/*  91 */
	*reg++ = IXGB_GET_STAT(adapter, torl);	/*  92 */
	*reg++ = IXGB_GET_STAT(adapter, torh);	/*  93 */
	*reg++ = IXGB_GET_STAT(adapter, rnbc);	/*  94 */
	*reg++ = IXGB_GET_STAT(adapter, ruc);	/*  95 */
	*reg++ = IXGB_GET_STAT(adapter, roc);	/*  96 */
	*reg++ = IXGB_GET_STAT(adapter, rlec);	/*  97 */
	*reg++ = IXGB_GET_STAT(adapter, crcerrs);	/*  98 */
	*reg++ = IXGB_GET_STAT(adapter, icbc);	/*  99 */
	*reg++ = IXGB_GET_STAT(adapter, ecbc);	/* 100 */
	*reg++ = IXGB_GET_STAT(adapter, mpc);	/* 101 */
	*reg++ = IXGB_GET_STAT(adapter, tptl);	/* 102 */
	*reg++ = IXGB_GET_STAT(adapter, tpth);	/* 103 */
	*reg++ = IXGB_GET_STAT(adapter, gptcl);	/* 104 */
	*reg++ = IXGB_GET_STAT(adapter, gptch);	/* 105 */
	*reg++ = IXGB_GET_STAT(adapter, bptcl);	/* 106 */
	*reg++ = IXGB_GET_STAT(adapter, bptch);	/* 107 */
	*reg++ = IXGB_GET_STAT(adapter, mptcl);	/* 108 */
	*reg++ = IXGB_GET_STAT(adapter, mptch);	/* 109 */
	*reg++ = IXGB_GET_STAT(adapter, uptcl);	/* 110 */
	*reg++ = IXGB_GET_STAT(adapter, uptch);	/* 111 */
	*reg++ = IXGB_GET_STAT(adapter, vptcl);	/* 112 */
	*reg++ = IXGB_GET_STAT(adapter, vptch);	/* 113 */
	*reg++ = IXGB_GET_STAT(adapter, jptcl);	/* 114 */
	*reg++ = IXGB_GET_STAT(adapter, jptch);	/* 115 */
	*reg++ = IXGB_GET_STAT(adapter, gotcl);	/* 116 */
	*reg++ = IXGB_GET_STAT(adapter, gotch);	/* 117 */
	*reg++ = IXGB_GET_STAT(adapter, totl);	/* 118 */
	*reg++ = IXGB_GET_STAT(adapter, toth);	/* 119 */
	*reg++ = IXGB_GET_STAT(adapter, dc);	/* 120 */
	*reg++ = IXGB_GET_STAT(adapter, plt64c);	/* 121 */
	*reg++ = IXGB_GET_STAT(adapter, tsctc);	/* 122 */
	*reg++ = IXGB_GET_STAT(adapter, tsctfc);	/* 123 */
	*reg++ = IXGB_GET_STAT(adapter, ibic);	/* 124 */
	*reg++ = IXGB_GET_STAT(adapter, rfc);	/* 125 */
	*reg++ = IXGB_GET_STAT(adapter, lfc);	/* 126 */
	*reg++ = IXGB_GET_STAT(adapter, pfrc);	/* 127 */
	*reg++ = IXGB_GET_STAT(adapter, pftc);	/* 128 */
	*reg++ = IXGB_GET_STAT(adapter, mcfrc);	/* 129 */
	*reg++ = IXGB_GET_STAT(adapter, mcftc);	/* 130 */
	*reg++ = IXGB_GET_STAT(adapter, xonrxc);	/* 131 */
	*reg++ = IXGB_GET_STAT(adapter, xontxc);	/* 132 */
	*reg++ = IXGB_GET_STAT(adapter, xoffrxc);	/* 133 */
	*reg++ = IXGB_GET_STAT(adapter, xofftxc);	/* 134 */
	*reg++ = IXGB_GET_STAT(adapter, rjc);	/* 135 */

	regs->len = (reg - reg_start) * sizeof(uint32_t);
}

static int
ixgb_get_eeprom_len(struct net_device *netdev)
{
	/* return size in bytes */
	return (IXGB_EEPROM_SIZE << 1);
}

static int
ixgb_get_eeprom(struct net_device *netdev,
		  struct ethtool_eeprom *eeprom, uint8_t *bytes)
{
	struct ixgb_adapter *adapter = netdev_priv(netdev);
	struct ixgb_hw *hw = &adapter->hw;
	uint16_t *eeprom_buff;
	int i, max_len, first_word, last_word;
	int ret_val = 0;

	if(eeprom->len == 0) {
		ret_val = -EINVAL;
		goto geeprom_error;
	}

	eeprom->magic = hw->vendor_id | (hw->device_id << 16);

	max_len = ixgb_get_eeprom_len(netdev);

	if(eeprom->offset > eeprom->offset + eeprom->len) {
		ret_val = -EINVAL;
		goto geeprom_error;
	}

	if((eeprom->offset + eeprom->len) > max_len)
		eeprom->len = (max_len - eeprom->offset);

	first_word = eeprom->offset >> 1;
	last_word = (eeprom->offset + eeprom->len - 1) >> 1;

	eeprom_buff = kmalloc(sizeof(uint16_t) *
			(last_word - first_word + 1), GFP_KERNEL);
	if(!eeprom_buff)
		return -ENOMEM;

	/* note the eeprom was good because the driver loaded */
	for(i = 0; i <= (last_word - first_word); i++) {
		eeprom_buff[i] = ixgb_get_eeprom_word(hw, (first_word + i));
	}

	memcpy(bytes, (uint8_t *)eeprom_buff + (eeprom->offset & 1),
			eeprom->len);
	kfree(eeprom_buff);

geeprom_error:
	return ret_val;
}

static int
ixgb_set_eeprom(struct net_device *netdev,
		  struct ethtool_eeprom *eeprom, uint8_t *bytes)
{
	struct ixgb_adapter *adapter = netdev_priv(netdev);
	struct ixgb_hw *hw = &adapter->hw;
	uint16_t *eeprom_buff;
	void *ptr;
	int max_len, first_word, last_word;
	uint16_t i;

	if(eeprom->len == 0)
		return -EINVAL;

	if(eeprom->magic != (hw->vendor_id | (hw->device_id << 16)))
		return -EFAULT;

	max_len = ixgb_get_eeprom_len(netdev);

	if(eeprom->offset > eeprom->offset + eeprom->len)
		return -EINVAL;

	if((eeprom->offset + eeprom->len) > max_len)
		eeprom->len = (max_len - eeprom->offset);

	first_word = eeprom->offset >> 1;
	last_word = (eeprom->offset + eeprom->len - 1) >> 1;
	eeprom_buff = kmalloc(max_len, GFP_KERNEL);
	if(!eeprom_buff)
		return -ENOMEM;

	ptr = (void *)eeprom_buff;

	if(eeprom->offset & 1) {
		/* need read/modify/write of first changed EEPROM word */
		/* only the second byte of the word is being modified */
		eeprom_buff[0] = ixgb_read_eeprom(hw, first_word);
		ptr++;
	}
	if((eeprom->offset + eeprom->len) & 1) {
		/* need read/modify/write of last changed EEPROM word */
		/* only the first byte of the word is being modified */
		eeprom_buff[last_word - first_word] 
			= ixgb_read_eeprom(hw, last_word);
	}

	memcpy(ptr, bytes, eeprom->len);
	for(i = 0; i <= (last_word - first_word); i++)
		ixgb_write_eeprom(hw, first_word + i, eeprom_buff[i]);

	/* Update the checksum over the first part of the EEPROM if needed */
	if(first_word <= EEPROM_CHECKSUM_REG)
		ixgb_update_eeprom_checksum(hw);

	kfree(eeprom_buff);
	return 0;
}

static void
ixgb_get_drvinfo(struct net_device *netdev,
		   struct ethtool_drvinfo *drvinfo)
{
	struct ixgb_adapter *adapter = netdev_priv(netdev);

	strncpy(drvinfo->driver,  ixgb_driver_name, 32);
	strncpy(drvinfo->version, ixgb_driver_version, 32);
	strncpy(drvinfo->fw_version, "N/A", 32);
	strncpy(drvinfo->bus_info, pci_name(adapter->pdev), 32);
	drvinfo->n_stats = IXGB_STATS_LEN;
	drvinfo->regdump_len = ixgb_get_regs_len(netdev);
	drvinfo->eedump_len = ixgb_get_eeprom_len(netdev);
}

static void
ixgb_get_ringparam(struct net_device *netdev,
		struct ethtool_ringparam *ring)
{
	struct ixgb_adapter *adapter = netdev_priv(netdev);
	struct ixgb_desc_ring *txdr = &adapter->tx_ring;
	struct ixgb_desc_ring *rxdr = &adapter->rx_ring;

	ring->rx_max_pending = MAX_RXD; 
	ring->tx_max_pending = MAX_TXD;
	ring->rx_mini_max_pending = 0;
	ring->rx_jumbo_max_pending = 0;
	ring->rx_pending = rxdr->count;
	ring->tx_pending = txdr->count;
	ring->rx_mini_pending = 0;
	ring->rx_jumbo_pending = 0;
}

static int 
ixgb_set_ringparam(struct net_device *netdev,
		struct ethtool_ringparam *ring)
{
	struct ixgb_adapter *adapter = netdev_priv(netdev);
	struct ixgb_desc_ring *txdr = &adapter->tx_ring;
	struct ixgb_desc_ring *rxdr = &adapter->rx_ring;
	struct ixgb_desc_ring tx_old, tx_new, rx_old, rx_new;
	int err;

	tx_old = adapter->tx_ring;
	rx_old = adapter->rx_ring;

	if((ring->rx_mini_pending) || (ring->rx_jumbo_pending)) 
		return -EINVAL;

	if(netif_running(adapter->netdev))
		ixgb_down(adapter,TRUE);

	rxdr->count = max(ring->rx_pending,(uint32_t)MIN_RXD);
	rxdr->count = min(rxdr->count,(uint32_t)MAX_RXD);
	IXGB_ROUNDUP(rxdr->count, IXGB_REQ_RX_DESCRIPTOR_MULTIPLE); 

	txdr->count = max(ring->tx_pending,(uint32_t)MIN_TXD);
	txdr->count = min(txdr->count,(uint32_t)MAX_TXD);
	IXGB_ROUNDUP(txdr->count, IXGB_REQ_TX_DESCRIPTOR_MULTIPLE); 

	if(netif_running(adapter->netdev)) {
		/* Try to get new resources before deleting old */
		if((err = ixgb_setup_rx_resources(adapter)))
			goto err_setup_rx;
		if((err = ixgb_setup_tx_resources(adapter)))
			goto err_setup_tx;

		/* save the new, restore the old in order to free it,
		 * then restore the new back again */

		rx_new = adapter->rx_ring;
		tx_new = adapter->tx_ring;
		adapter->rx_ring = rx_old;
		adapter->tx_ring = tx_old;
		ixgb_free_rx_resources(adapter);
		ixgb_free_tx_resources(adapter);
		adapter->rx_ring = rx_new;
		adapter->tx_ring = tx_new;
		if((err = ixgb_up(adapter)))
			return err;
		/* be optimistic about our link, since we were up before */
		adapter->link_speed = 10000;
		adapter->link_duplex = FULL_DUPLEX;
		netif_carrier_on(netdev);
		netif_wake_queue(netdev);
	}

	return 0;
err_setup_tx:
	ixgb_free_rx_resources(adapter);
err_setup_rx:
	adapter->rx_ring = rx_old;
	adapter->tx_ring = tx_old;
	ixgb_up(adapter);
	return err;
}

/* toggle LED 4 times per second = 2 "blinks" per second */
#define IXGB_ID_INTERVAL	(HZ/4)

/* bit defines for adapter->led_status */
#define IXGB_LED_ON		0

static void
ixgb_led_blink_callback(unsigned long data)
{
	struct ixgb_adapter *adapter = (struct ixgb_adapter *)data;

	if(test_and_change_bit(IXGB_LED_ON, &adapter->led_status))
		ixgb_led_off(&adapter->hw);
	else
		ixgb_led_on(&adapter->hw);

	mod_timer(&adapter->blink_timer, jiffies + IXGB_ID_INTERVAL);
}

static int
ixgb_phys_id(struct net_device *netdev, uint32_t data)
{
	struct ixgb_adapter *adapter = netdev_priv(netdev);

	if(!data || data > (uint32_t)(MAX_SCHEDULE_TIMEOUT / HZ))
		data = (uint32_t)(MAX_SCHEDULE_TIMEOUT / HZ);

	if(!adapter->blink_timer.function) {
		init_timer(&adapter->blink_timer);
		adapter->blink_timer.function = ixgb_led_blink_callback;
		adapter->blink_timer.data = (unsigned long)adapter;
	}

	mod_timer(&adapter->blink_timer, jiffies);

	set_current_state(TASK_INTERRUPTIBLE);
	if(data)
		schedule_timeout(data * HZ);
	else
		schedule_timeout(MAX_SCHEDULE_TIMEOUT);

	del_timer_sync(&adapter->blink_timer);
	ixgb_led_off(&adapter->hw);
	clear_bit(IXGB_LED_ON, &adapter->led_status);

	return 0;
}

static int 
ixgb_get_stats_count(struct net_device *netdev)
{
	return IXGB_STATS_LEN;
}

static void 
ixgb_get_ethtool_stats(struct net_device *netdev, 
		struct ethtool_stats *stats, uint64_t *data)
{
	struct ixgb_adapter *adapter = netdev_priv(netdev);
	int i;

	ixgb_update_stats(adapter);
	for(i = 0; i < IXGB_STATS_LEN; i++) {
		char *p = (char *)adapter+ixgb_gstrings_stats[i].stat_offset;	
		data[i] = (ixgb_gstrings_stats[i].sizeof_stat == 
			sizeof(uint64_t)) ? *(uint64_t *)p : *(uint32_t *)p;
	}
}

static void 
ixgb_get_strings(struct net_device *netdev, uint32_t stringset, uint8_t *data)
{
	int i;

	switch(stringset) {
	case ETH_SS_STATS:
		for(i=0; i < IXGB_STATS_LEN; i++) {
			memcpy(data + i * ETH_GSTRING_LEN, 
			ixgb_gstrings_stats[i].stat_string,
			ETH_GSTRING_LEN);
		}
		break;
	}
}

struct ethtool_ops ixgb_ethtool_ops = {
	.get_settings = ixgb_get_settings,
	.set_settings = ixgb_set_settings,
	.get_drvinfo = ixgb_get_drvinfo,
	.get_regs_len = ixgb_get_regs_len,
	.get_regs = ixgb_get_regs,
	.get_link = ethtool_op_get_link,
	.get_eeprom_len = ixgb_get_eeprom_len,
	.get_eeprom = ixgb_get_eeprom,
	.set_eeprom = ixgb_set_eeprom,
	.get_ringparam = ixgb_get_ringparam,
	.set_ringparam = ixgb_set_ringparam,
	.get_pauseparam	= ixgb_get_pauseparam,
	.set_pauseparam	= ixgb_set_pauseparam,
	.get_rx_csum = ixgb_get_rx_csum,
	.set_rx_csum = ixgb_set_rx_csum,
	.get_tx_csum = ixgb_get_tx_csum,
	.set_tx_csum = ixgb_set_tx_csum,
	.get_sg	= ethtool_op_get_sg,
	.set_sg	= ethtool_op_set_sg,
#ifdef NETIF_F_TSO
	.get_tso = ethtool_op_get_tso,
	.set_tso = ixgb_set_tso,
#endif
	.get_strings = ixgb_get_strings,
	.phys_id = ixgb_phys_id,
	.get_stats_count = ixgb_get_stats_count,
	.get_ethtool_stats = ixgb_get_ethtool_stats,
};

void ixgb_set_ethtool_ops(struct net_device *netdev)
{
	SET_ETHTOOL_OPS(netdev, &ixgb_ethtool_ops);
}
