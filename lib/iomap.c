/*
 * Implement the default iomap interfaces
 *
 * (C) Copyright 2004 Linus Torvalds
 */
#include <linux/pci.h>
#include <linux/module.h>
#include <asm/io.h>

/*
 * Read/write from/to an (offsettable) iomem cookie. It might be a PIO
 * access or a MMIO access, these functions don't care. The info is
 * encoded in the hardware mapping set up by the mapping functions
 * (or the cookie itself, depending on implementation and hw).
 *
 * The generic routines don't assume any hardware mappings, and just
 * encode the PIO/MMIO as part of the cookie. They coldly assume that
 * the MMIO IO mappings are not in the low address range.
 *
 * Architectures for which this is not true can't use this generic
 * implementation and should do their own copy.
 */

#ifndef HAVE_ARCH_PIO_SIZE
/*
 * We encode the physical PIO addresses (0-0xffff) into the
 * pointer by offsetting them with a constant (0x10000) and
 * assuming that all the low addresses are always PIO. That means
 * we can do some sanity checks on the low bits, and don't
 * need to just take things for granted.
 */
#define PIO_OFFSET	0x10000UL
#define PIO_MASK	0x0ffffUL
#define PIO_RESERVED	0x40000UL
#endif

/*
 * Ugly macros are a way of life.
 */
#define VERIFY_PIO(port) BUG_ON((port & ~PIO_MASK) != PIO_OFFSET)

#define IO_COND(addr, is_pio, is_mmio) do {			\
	unsigned long port = (unsigned long __force)addr;	\
	if (port < PIO_RESERVED) {				\
		VERIFY_PIO(port);				\
		port &= PIO_MASK;				\
		is_pio;						\
	} else {						\
		is_mmio;					\
	}							\
} while (0)

#ifndef pio_read16be
#define pio_read16be(port) swab16(inw(port))
#define pio_read32be(port) swab32(inl(port))
#endif

#ifndef mmio_read16be
#define mmio_read16be(addr) be16_to_cpu(__raw_readw(addr))
#define mmio_read32be(addr) be32_to_cpu(__raw_readl(addr))
#endif

unsigned int fastcall ioread8(void __iomem *addr)
{
	IO_COND(addr, return inb(port), return readb(addr));
}
unsigned int fastcall ioread16(void __iomem *addr)
{
	IO_COND(addr, return inw(port), return readw(addr));
}
unsigned int fastcall ioread16be(void __iomem *addr)
{
	IO_COND(addr, return pio_read16be(port), return mmio_read16be(addr));
}
unsigned int fastcall ioread32(void __iomem *addr)
{
	IO_COND(addr, return inl(port), return readl(addr));
}
unsigned int fastcall ioread32be(void __iomem *addr)
{
	IO_COND(addr, return pio_read32be(port), return mmio_read32be(addr));
}
EXPORT_SYMBOL(ioread8);
EXPORT_SYMBOL(ioread16);
EXPORT_SYMBOL(ioread16be);
EXPORT_SYMBOL(ioread32);
EXPORT_SYMBOL(ioread32be);

#ifndef pio_write16be
#define pio_write16be(val,port) outw(swab16(val),port)
#define pio_write32be(val,port) outl(swab32(val),port)
#endif

#ifndef mmio_write16be
#define mmio_write16be(val,port) __raw_writew(be16_to_cpu(val),port)
#define mmio_write32be(val,port) __raw_writel(be32_to_cpu(val),port)
#endif

void fastcall iowrite8(u8 val, void __iomem *addr)
{
	IO_COND(addr, outb(val,port), writeb(val, addr));
}
void fastcall iowrite16(u16 val, void __iomem *addr)
{
	IO_COND(addr, outw(val,port), writew(val, addr));
}
void fastcall iowrite16be(u16 val, void __iomem *addr)
{
	IO_COND(addr, pio_write16be(val,port), mmio_write16be(val, addr));
}
void fastcall iowrite32(u32 val, void __iomem *addr)
{
	IO_COND(addr, outl(val,port), writel(val, addr));
}
void fastcall iowrite32be(u32 val, void __iomem *addr)
{
	IO_COND(addr, pio_write32be(val,port), mmio_write32be(val, addr));
}
EXPORT_SYMBOL(iowrite8);
EXPORT_SYMBOL(iowrite16);
EXPORT_SYMBOL(iowrite16be);
EXPORT_SYMBOL(iowrite32);
EXPORT_SYMBOL(iowrite32be);

/*
 * These are the "repeat MMIO read/write" functions.
 * Note the "__raw" accesses, since we don't want to
 * convert to CPU byte order. We write in "IO byte
 * order" (we also don't have IO barriers).
 */
#ifndef mmio_insb
static inline void mmio_insb(void __iomem *addr, u8 *dst, int count)
{
	while (--count >= 0) {
		u8 data = __raw_readb(addr);
		*dst = data;
		dst++;
	}
}
static inline void mmio_insw(void __iomem *addr, u16 *dst, int count)
{
	while (--count >= 0) {
		u16 data = __raw_readw(addr);
		*dst = data;
		dst++;
	}
}
static inline void mmio_insl(void __iomem *addr, u32 *dst, int count)
{
	while (--count >= 0) {
		u32 data = __raw_readl(addr);
		*dst = data;
		dst++;
	}
}
#endif

#ifndef mmio_outsb
static inline void mmio_outsb(void __iomem *addr, const u8 *src, int count)
{
	while (--count >= 0) {
		__raw_writeb(*src, addr);
		src++;
	}
}
static inline void mmio_outsw(void __iomem *addr, const u16 *src, int count)
{
	while (--count >= 0) {
		__raw_writew(*src, addr);
		src++;
	}
}
static inline void mmio_outsl(void __iomem *addr, const u32 *src, int count)
{
	while (--count >= 0) {
		__raw_writel(*src, addr);
		src++;
	}
}
#endif

void fastcall ioread8_rep(void __iomem *addr, void *dst, unsigned long count)
{
	IO_COND(addr, insb(port,dst,count), mmio_insb(addr, dst, count));
}
void fastcall ioread16_rep(void __iomem *addr, void *dst, unsigned long count)
{
	IO_COND(addr, insw(port,dst,count), mmio_insw(addr, dst, count));
}
void fastcall ioread32_rep(void __iomem *addr, void *dst, unsigned long count)
{
	IO_COND(addr, insl(port,dst,count), mmio_insl(addr, dst, count));
}
EXPORT_SYMBOL(ioread8_rep);
EXPORT_SYMBOL(ioread16_rep);
EXPORT_SYMBOL(ioread32_rep);

void fastcall iowrite8_rep(void __iomem *addr, const void *src, unsigned long count)
{
	IO_COND(addr, outsb(port, src, count), mmio_outsb(addr, src, count));
}
void fastcall iowrite16_rep(void __iomem *addr, const void *src, unsigned long count)
{
	IO_COND(addr, outsw(port, src, count), mmio_outsw(addr, src, count));
}
void fastcall iowrite32_rep(void __iomem *addr, const void *src, unsigned long count)
{
	IO_COND(addr, outsl(port, src,count), mmio_outsl(addr, src, count));
}
EXPORT_SYMBOL(iowrite8_rep);
EXPORT_SYMBOL(iowrite16_rep);
EXPORT_SYMBOL(iowrite32_rep);

/* Create a virtual mapping cookie for an IO port range */
void __iomem *ioport_map(unsigned long port, unsigned int nr)
{
	if (port > PIO_MASK)
		return NULL;
	return (void __iomem *) (unsigned long) (port + PIO_OFFSET);
}

void ioport_unmap(void __iomem *addr)
{
	/* Nothing to do */
}
EXPORT_SYMBOL(ioport_map);
EXPORT_SYMBOL(ioport_unmap);

/* Create a virtual mapping cookie for a PCI BAR (memory or IO) */
void __iomem *pci_iomap(struct pci_dev *dev, int bar, unsigned long maxlen)
{
	unsigned long start = pci_resource_start(dev, bar);
	unsigned long len = pci_resource_len(dev, bar);
	unsigned long flags = pci_resource_flags(dev, bar);

	if (!len || !start)
		return NULL;
	if (maxlen && len > maxlen)
		len = maxlen;
	if (flags & IORESOURCE_IO)
		return ioport_map(start, len);
	if (flags & IORESOURCE_MEM) {
		if (flags & IORESOURCE_CACHEABLE)
			return ioremap(start, len);
		return ioremap_nocache(start, len);
	}
	/* What? */
	return NULL;
}

void pci_iounmap(struct pci_dev *dev, void __iomem * addr)
{
	IO_COND(addr, /* nothing */, iounmap(addr));
}
EXPORT_SYMBOL(pci_iomap);
EXPORT_SYMBOL(pci_iounmap);
