#ifndef __SPARC_PCI_H
#define __SPARC_PCI_H

#ifdef __KERNEL__

#include <linux/dma-mapping.h>

/* Can be used to override the logic in pci_scan_bus for skipping
 * already-configured bus numbers - to be used for buggy BIOSes
 * or architectures with incomplete PCI setup by the loader.
 */
#define pcibios_assign_all_busses()	0
#define pcibios_scan_all_fns(a, b)	0

#define PCIBIOS_MIN_IO		0UL
#define PCIBIOS_MIN_MEM		0UL

#define PCI_IRQ_NONE		0xffffffff

static inline void pcibios_set_master(struct pci_dev *dev)
{
	/* No special bus mastering setup handling */
}

static inline void pcibios_penalize_isa_irq(int irq, int active)
{
	/* We don't do dynamic PCI IRQ allocation */
}

/* Dynamic DMA mapping stuff.
 */
#define PCI_DMA_BUS_IS_PHYS	(0)

struct pci_dev;

/* pci_unmap_{single,page} is not a nop, thus... */
#define DECLARE_PCI_UNMAP_ADDR(ADDR_NAME)	\
	dma_addr_t ADDR_NAME;
#define DECLARE_PCI_UNMAP_LEN(LEN_NAME)		\
	__u32 LEN_NAME;
#define pci_unmap_addr(PTR, ADDR_NAME)			\
	((PTR)->ADDR_NAME)
#define pci_unmap_addr_set(PTR, ADDR_NAME, VAL)		\
	(((PTR)->ADDR_NAME) = (VAL))
#define pci_unmap_len(PTR, LEN_NAME)			\
	((PTR)->LEN_NAME)
#define pci_unmap_len_set(PTR, LEN_NAME, VAL)		\
	(((PTR)->LEN_NAME) = (VAL))

#ifdef CONFIG_PCI
static inline void pci_dma_burst_advice(struct pci_dev *pdev,
					enum pci_dma_burst_strategy *strat,
					unsigned long *strategy_parameter)
{
	*strat = PCI_DMA_BURST_INFINITY;
	*strategy_parameter = ~0UL;
}
#endif

struct device_node;
extern struct device_node *pci_device_to_OF_node(struct pci_dev *pdev);

#endif /* __KERNEL__ */

/* generic pci stuff */
#include <asm-generic/pci.h>

#endif /* __SPARC_PCI_H */
