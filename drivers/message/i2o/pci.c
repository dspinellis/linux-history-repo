/*
 *	PCI handling of I2O controller
 *
 * 	Copyright (C) 1999-2002	Red Hat Software
 *
 *	Written by Alan Cox, Building Number Three Ltd
 *
 *	This program is free software; you can redistribute it and/or modify it
 *	under the terms of the GNU General Public License as published by the
 *	Free Software Foundation; either version 2 of the License, or (at your
 *	option) any later version.
 *
 *	A lot of the I2O message side code from this is taken from the Red
 *	Creek RCPCI45 adapter driver by Red Creek Communications
 *
 *	Fixes/additions:
 *		Philipp Rumpf
 *		Juha Siev�nen <Juha.Sievanen@cs.Helsinki.FI>
 *		Auvo H�kkinen <Auvo.Hakkinen@cs.Helsinki.FI>
 *		Deepak Saxena <deepak@plexity.net>
 *		Boji T Kannanthanam <boji.t.kannanthanam@intel.com>
 *		Alan Cox <alan@redhat.com>:
 *			Ported to Linux 2.5.
 *		Markus Lidel <Markus.Lidel@shadowconnect.com>:
 *			Minor fixes for 2.6.
 *		Markus Lidel <Markus.Lidel@shadowconnect.com>:
 *			Support for sysfs included.
 */

#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/i2o.h>

/* Module internal functions from other sources */
extern struct i2o_controller *i2o_iop_alloc(void);
extern void i2o_iop_free(struct i2o_controller *);

extern int i2o_iop_add(struct i2o_controller *);
extern void i2o_iop_remove(struct i2o_controller *);

extern int i2o_driver_dispatch(struct i2o_controller *, u32);

/* PCI device id table for all I2O controllers */
static struct pci_device_id __devinitdata i2o_pci_ids[] = {
	{PCI_DEVICE_CLASS(PCI_CLASS_INTELLIGENT_I2O << 8, 0xffff00)},
	{PCI_DEVICE(PCI_VENDOR_ID_DPT, 0xa511)},
	{.vendor = PCI_VENDOR_ID_INTEL,.device = 0x1962,
	 .subvendor = PCI_VENDOR_ID_PROMISE,.subdevice = PCI_ANY_ID},
	{0}
};

/**
 *	i2o_dma_realloc - Realloc DMA memory
 *	@dev: struct device pointer to the PCI device of the I2O controller
 *	@addr: pointer to a i2o_dma struct DMA buffer
 *	@len: new length of memory
 *	@gfp_mask: GFP mask
 *
 *	If there was something allocated in the addr, free it first. If len > 0
 *	than try to allocate it and write the addresses back to the addr
 *	structure. If len == 0 set the virtual address to NULL.
 *
 *	Returns the 0 on success or negative error code on failure.
 */
int i2o_dma_realloc(struct device *dev, struct i2o_dma *addr, size_t len,
		    unsigned int gfp_mask)
{
	i2o_dma_free(dev, addr);

	if (len)
		return i2o_dma_alloc(dev, addr, len, gfp_mask);

	return 0;
};

/**
 *	i2o_pci_free - Frees the DMA memory for the I2O controller
 *	@c: I2O controller to free
 *
 *	Remove all allocated DMA memory and unmap memory IO regions. If MTRR
 *	is enabled, also remove it again.
 */
static void i2o_pci_free(struct i2o_controller *c)
{
	struct device *dev;

	dev = &c->pdev->dev;

	i2o_dma_free(dev, &c->out_queue);
	i2o_dma_free(dev, &c->status_block);
	kfree(c->lct);
	i2o_dma_free(dev, &c->dlct);
	i2o_dma_free(dev, &c->hrt);
	i2o_dma_free(dev, &c->status);

	if (c->raptor && c->in_queue.virt)
		iounmap(c->in_queue.virt);

	if (c->base.virt)
		iounmap(c->base.virt);
}

/**
 *	i2o_pci_alloc - Allocate DMA memory, map IO memory for I2O controller
 *	@c: I2O controller
 *
 *	Allocate DMA memory for a PCI (or in theory AGP) I2O controller. All
 *	IO mappings are also done here. If MTRR is enabled, also do add memory
 *	regions here.
 *
 *	Returns 0 on success or negative error code on failure.
 */
static int __devinit i2o_pci_alloc(struct i2o_controller *c)
{
	struct pci_dev *pdev = c->pdev;
	struct device *dev = &pdev->dev;
	int i;

	for (i = 0; i < 6; i++) {
		/* Skip I/O spaces */
		if (!(pci_resource_flags(pdev, i) & IORESOURCE_IO)) {
			if (!c->base.phys) {
				c->base.phys = pci_resource_start(pdev, i);
				c->base.len = pci_resource_len(pdev, i);

				/*
				 * If we know what card it is, set the size
				 * correctly. Code is taken from dpt_i2o.c
				 */
				if (pdev->device == 0xa501) {
					if (pdev->subsystem_device >= 0xc032 &&
					    pdev->subsystem_device <= 0xc03b) {
						if (c->base.len > 0x400000)
							c->base.len = 0x400000;
					} else {
						if (c->base.len > 0x100000)
							c->base.len = 0x100000;
					}
				}
				if (!c->raptor)
					break;
			} else {
				c->in_queue.phys = pci_resource_start(pdev, i);
				c->in_queue.len = pci_resource_len(pdev, i);
				break;
			}
		}
	}

	if (i == 6) {
		printk(KERN_ERR "%s: I2O controller has no memory regions"
		       " defined.\n", c->name);
		i2o_pci_free(c);
		return -EINVAL;
	}

	/* Map the I2O controller */
	if (c->raptor) {
		printk(KERN_INFO "%s: PCI I2O controller\n", c->name);
		printk(KERN_INFO "     BAR0 at 0x%08lX size=%ld\n",
		       (unsigned long)c->base.phys, (unsigned long)c->base.len);
		printk(KERN_INFO "     BAR1 at 0x%08lX size=%ld\n",
		       (unsigned long)c->in_queue.phys,
		       (unsigned long)c->in_queue.len);
	} else
		printk(KERN_INFO "%s: PCI I2O controller at %08lX size=%ld\n",
		       c->name, (unsigned long)c->base.phys,
		       (unsigned long)c->base.len);

	c->base.virt = ioremap_nocache(c->base.phys, c->base.len);
	if (!c->base.virt) {
		printk(KERN_ERR "%s: Unable to map controller.\n", c->name);
		return -ENOMEM;
	}

	if (c->raptor) {
		c->in_queue.virt =
		    ioremap_nocache(c->in_queue.phys, c->in_queue.len);
		if (!c->in_queue.virt) {
			printk(KERN_ERR "%s: Unable to map controller.\n",
			       c->name);
			i2o_pci_free(c);
			return -ENOMEM;
		}
	} else
		c->in_queue = c->base;

	c->irq_mask = c->base.virt + I2O_IRQ_MASK;
	c->in_port = c->base.virt + I2O_IN_PORT;
	c->out_port = c->base.virt + I2O_OUT_PORT;

	if (i2o_dma_alloc(dev, &c->status, 8, GFP_KERNEL)) {
		i2o_pci_free(c);
		return -ENOMEM;
	}

	if (i2o_dma_alloc(dev, &c->hrt, sizeof(i2o_hrt), GFP_KERNEL)) {
		i2o_pci_free(c);
		return -ENOMEM;
	}

	if (i2o_dma_alloc(dev, &c->dlct, 8192, GFP_KERNEL)) {
		i2o_pci_free(c);
		return -ENOMEM;
	}

	if (i2o_dma_alloc(dev, &c->status_block, sizeof(i2o_status_block),
			  GFP_KERNEL)) {
		i2o_pci_free(c);
		return -ENOMEM;
	}

	if (i2o_dma_alloc(dev, &c->out_queue, MSG_POOL_SIZE, GFP_KERNEL)) {
		i2o_pci_free(c);
		return -ENOMEM;
	}

	pci_set_drvdata(pdev, c);

	return 0;
}

/**
 *	i2o_pci_interrupt - Interrupt handler for I2O controller
 *	@irq: interrupt line
 *	@dev_id: pointer to the I2O controller
 *	@r: pointer to registers
 *
 *	Handle an interrupt from a PCI based I2O controller. This turns out
 *	to be rather simple. We keep the controller pointer in the cookie.
 */
static irqreturn_t i2o_pci_interrupt(int irq, void *dev_id, struct pt_regs *r)
{
	struct i2o_controller *c = dev_id;
	struct device *dev = &c->pdev->dev;
	u32 mv = readl(c->out_port);

	/*
	 * Old 960 steppings had a bug in the I2O unit that caused
	 * the queue to appear empty when it wasn't.
	 */
	if (mv == I2O_QUEUE_EMPTY) {
		mv = readl(c->out_port);
		if (unlikely(mv == I2O_QUEUE_EMPTY))
			return IRQ_NONE;
		else
			pr_debug("%s: 960 bug detected\n", c->name);
	}

	while (mv != I2O_QUEUE_EMPTY) {
		/* dispatch it */
		if (i2o_driver_dispatch(c, mv))
			/* flush it if result != 0 */
			i2o_flush_reply(c, mv);

		/*
		 * That 960 bug again...
		 */
		mv = readl(c->out_port);
		if (mv == I2O_QUEUE_EMPTY)
			mv = readl(c->out_port);
	}

	return IRQ_HANDLED;
}

/**
 *	i2o_pci_irq_enable - Allocate interrupt for I2O controller
 *
 *	Allocate an interrupt for the I2O controller, and activate interrupts
 *	on the I2O controller.
 *
 *	Returns 0 on success or negative error code on failure.
 */
static int i2o_pci_irq_enable(struct i2o_controller *c)
{
	struct pci_dev *pdev = c->pdev;
	int rc;

	wmb();
	writel(0xffffffff, c->irq_mask);
	wmb();

	if (pdev->irq) {
		rc = request_irq(pdev->irq, i2o_pci_interrupt, SA_SHIRQ,
				 c->name, c);
		if (rc < 0) {
			printk(KERN_ERR "%s: unable to allocate interrupt %d."
			       "\n", c->name, pdev->irq);
			return rc;
		}
	}

	writel(0x00000000, c->irq_mask);
	wmb();

	printk(KERN_INFO "%s: Installed at IRQ %d\n", c->name, pdev->irq);

	return 0;
}

/**
 *	i2o_pci_irq_disable - Free interrupt for I2O controller
 *	@c: I2O controller
 *
 *	Disable interrupts in I2O controller and then free interrupt.
 */
static void i2o_pci_irq_disable(struct i2o_controller *c)
{
	wmb();
	writel(0xffffffff, c->irq_mask);
	wmb();

	if (c->pdev->irq > 0)
		free_irq(c->pdev->irq, c);
}

/**
 *	i2o_pci_probe - Probe the PCI device for an I2O controller
 *	@dev: PCI device to test
 *	@id: id which matched with the PCI device id table
 *
 *	Probe the PCI device for any device which is a memory of the
 *	Intelligent, I2O class or an Adaptec Zero Channel Controller. We
 *	attempt to set up each such device and register it with the core.
 *
 *	Returns 0 on success or negative error code on failure.
 */
static int __devinit i2o_pci_probe(struct pci_dev *pdev,
				   const struct pci_device_id *id)
{
	struct i2o_controller *c;
	int rc;
	struct pci_dev *i960 = NULL;

	printk(KERN_INFO "i2o: Checking for PCI I2O controllers...\n");

	if ((pdev->class & 0xff) > 1) {
		printk(KERN_WARNING "i2o: %s does not support I2O 1.5 "
		       "(skipping).\n", pci_name(pdev));
		return -ENODEV;
	}

	if ((rc = pci_enable_device(pdev))) {
		printk(KERN_WARNING "i2o: couldn't enable device %s\n",
		       pci_name(pdev));
		return rc;
	}

	if (pci_set_dma_mask(pdev, DMA_32BIT_MASK)) {
		printk(KERN_WARNING "i2o: no suitable DMA found for %s\n",
		       pci_name(pdev));
		rc = -ENODEV;
		goto disable;
	}

	pci_set_master(pdev);

	c = i2o_iop_alloc();
	if (IS_ERR(c)) {
		printk(KERN_ERR "i2o: couldn't allocate memory for %s\n",
		       pci_name(pdev));
		rc = PTR_ERR(c);
		goto disable;
	} else
		printk(KERN_INFO "%s: controller found (%s)\n", c->name,
		       pci_name(pdev));

	c->pdev = pdev;
	c->device.parent = get_device(&pdev->dev);

	/* Cards that fall apart if you hit them with large I/O loads... */
	if (pdev->vendor == PCI_VENDOR_ID_NCR && pdev->device == 0x0630) {
		c->short_req = 1;
		printk(KERN_INFO "%s: Symbios FC920 workarounds activated.\n",
		       c->name);
	}

	if (pdev->subsystem_vendor == PCI_VENDOR_ID_PROMISE) {
		/*
		 * Expose the ship behind i960 for initialization, or it will
		 * failed
		 */
		i960 =
		    pci_find_slot(c->pdev->bus->number,
				  PCI_DEVFN(PCI_SLOT(c->pdev->devfn), 0));

		if (i960)
			pci_write_config_word(i960, 0x42, 0);

		c->promise = 1;
	}

	/* Cards that go bananas if you quiesce them before you reset them. */
	if (pdev->vendor == PCI_VENDOR_ID_DPT) {
		c->no_quiesce = 1;
		if (pdev->device == 0xa511)
			c->raptor = 1;
	}

	if ((rc = i2o_pci_alloc(c))) {
		printk(KERN_ERR "%s: DMA / IO allocation for I2O controller "
		       " failed\n", c->name);
		goto free_controller;
	}

	if (i2o_pci_irq_enable(c)) {
		printk(KERN_ERR "%s: unable to enable interrupts for I2O "
		       "controller\n", c->name);
		goto free_pci;
	}

	if ((rc = i2o_iop_add(c)))
		goto uninstall;

	if (i960)
		pci_write_config_word(i960, 0x42, 0x03ff);

	get_device(&c->device);

	return 0;

      uninstall:
	i2o_pci_irq_disable(c);

      free_pci:
	i2o_pci_free(c);

      free_controller:
	i2o_iop_free(c);
	put_device(c->device.parent);

      disable:
	pci_disable_device(pdev);

	return rc;
}

/**
 *	i2o_pci_remove - Removes a I2O controller from the system
 *	pdev: I2O controller which should be removed
 *
 *	Reset the I2O controller, disable interrupts and remove all allocated
 *	resources.
 */
static void __devexit i2o_pci_remove(struct pci_dev *pdev)
{
	struct i2o_controller *c;
	c = pci_get_drvdata(pdev);

	i2o_iop_remove(c);
	i2o_pci_irq_disable(c);
	i2o_pci_free(c);

	pci_disable_device(pdev);

	printk(KERN_INFO "%s: Controller removed.\n", c->name);

	put_device(c->device.parent);
	put_device(&c->device);
};

/* PCI driver for I2O controller */
static struct pci_driver i2o_pci_driver = {
	.name = "PCI_I2O",
	.id_table = i2o_pci_ids,
	.probe = i2o_pci_probe,
	.remove = __devexit_p(i2o_pci_remove),
};

/**
 *	i2o_pci_init - registers I2O PCI driver in PCI subsystem
 *
 *	Returns > 0 on success or negative error code on failure.
 */
int __init i2o_pci_init(void)
{
	return pci_register_driver(&i2o_pci_driver);
};

/**
 *	i2o_pci_exit - unregisters I2O PCI driver from PCI subsystem
 */
void __exit i2o_pci_exit(void)
{
	pci_unregister_driver(&i2o_pci_driver);
};

EXPORT_SYMBOL(i2o_dma_realloc);
MODULE_DEVICE_TABLE(pci, i2o_pci_ids);
