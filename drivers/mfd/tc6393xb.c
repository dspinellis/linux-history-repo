/*
 * Toshiba TC6393XB SoC support
 *
 * Copyright(c) 2005-2006 Chris Humbert
 * Copyright(c) 2005 Dirk Opfer
 * Copyright(c) 2005 Ian Molton <spyro@f2s.com>
 * Copyright(c) 2007 Dmitry Baryshkov
 *
 * Based on code written by Sharp/Lineo for 2.4 kernels
 * Based on locomo.c
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/platform_device.h>
#include <linux/fb.h>
#include <linux/clk.h>
#include <linux/mfd/tc6393xb.h>
#include <linux/gpio.h>

#define SCR_REVID	0x08		/* b Revision ID	*/
#define SCR_ISR		0x50		/* b Interrupt Status	*/
#define SCR_IMR		0x52		/* b Interrupt Mask	*/
#define SCR_IRR		0x54		/* b Interrupt Routing	*/
#define SCR_GPER	0x60		/* w GP Enable		*/
#define SCR_GPI_SR(i)	(0x64 + (i))	/* b3 GPI Status	*/
#define SCR_GPI_IMR(i)	(0x68 + (i))	/* b3 GPI INT Mask	*/
#define SCR_GPI_EDER(i)	(0x6c + (i))	/* b3 GPI Edge Detect Enable */
#define SCR_GPI_LIR(i)	(0x70 + (i))	/* b3 GPI Level Invert	*/
#define SCR_GPO_DSR(i)	(0x78 + (i))	/* b3 GPO Data Set	*/
#define SCR_GPO_DOECR(i) (0x7c + (i))	/* b3 GPO Data OE Control */
#define SCR_GP_IARCR(i)	(0x80 + (i))	/* b3 GP Internal Active Register Control */
#define SCR_GP_IARLCR(i) (0x84 + (i))	/* b3 GP INTERNAL Active Register Level Control */
#define SCR_GPI_BCR(i)	(0x88 + (i))	/* b3 GPI Buffer Control */
#define SCR_GPA_IARCR	0x8c		/* w GPa Internal Active Register Control */
#define SCR_GPA_IARLCR	0x90		/* w GPa Internal Active Register Level Control */
#define SCR_GPA_BCR	0x94		/* w GPa Buffer Control */
#define SCR_CCR		0x98		/* w Clock Control	*/
#define SCR_PLL2CR	0x9a		/* w PLL2 Control	*/
#define SCR_PLL1CR	0x9c		/* l PLL1 Control	*/
#define SCR_DIARCR	0xa0		/* b Device Internal Active Register Control */
#define SCR_DBOCR	0xa1		/* b Device Buffer Off Control */
#define SCR_FER		0xe0		/* b Function Enable	*/
#define SCR_MCR		0xe4		/* w Mode Control	*/
#define SCR_CONFIG	0xfc		/* b Configuration Control */
#define SCR_DEBUG	0xff		/* b Debug		*/

#define SCR_CCR_CK32K	BIT(0)
#define SCR_CCR_USBCK	BIT(1)
#define SCR_CCR_UNK1	BIT(4)
#define SCR_CCR_MCLK_MASK	(7 << 8)
#define SCR_CCR_MCLK_OFF	(0 << 8)
#define SCR_CCR_MCLK_12	(1 << 8)
#define SCR_CCR_MCLK_24	(2 << 8)
#define SCR_CCR_MCLK_48	(3 << 8)
#define SCR_CCR_HCLK_MASK	(3 << 12)
#define SCR_CCR_HCLK_24	(0 << 12)
#define SCR_CCR_HCLK_48	(1 << 12)

#define SCR_FER_USBEN		BIT(0)	/* USB host enable */
#define SCR_FER_LCDCVEN		BIT(1)	/* polysilicon TFT enable */
#define SCR_FER_SLCDEN		BIT(2)	/* SLCD enable */

#define SCR_MCR_RDY_MASK		(3 << 0)
#define SCR_MCR_RDY_OPENDRAIN	(0 << 0)
#define SCR_MCR_RDY_TRISTATE	(1 << 0)
#define SCR_MCR_RDY_PUSHPULL	(2 << 0)
#define SCR_MCR_RDY_UNK		BIT(2)
#define SCR_MCR_RDY_EN		BIT(3)
#define SCR_MCR_INT_MASK		(3 << 4)
#define SCR_MCR_INT_OPENDRAIN	(0 << 4)
#define SCR_MCR_INT_TRISTATE	(1 << 4)
#define SCR_MCR_INT_PUSHPULL	(2 << 4)
#define SCR_MCR_INT_UNK		BIT(6)
#define SCR_MCR_INT_EN		BIT(7)
/* bits 8 - 16 are unknown */

#define TC_GPIO_BIT(i)		(1 << (i & 0x7))

/*--------------------------------------------------------------------------*/

struct tc6393xb {
	void __iomem		*scr;

	struct gpio_chip	gpio;

	struct clk		*clk; /* 3,6 Mhz */

	spinlock_t		lock; /* protects RMW cycles */

	struct {
		u8		fer;
		u16		ccr;
		u8		gpi_bcr[3];
		u8		gpo_dsr[3];
		u8		gpo_doecr[3];
	} suspend_state;

	struct resource		rscr;
	struct resource		*iomem;
	int			irq;
	int			irq_base;
};

/*--------------------------------------------------------------------------*/

static int tc6393xb_gpio_get(struct gpio_chip *chip,
		unsigned offset)
{
	struct tc6393xb *tc6393xb = container_of(chip, struct tc6393xb, gpio);

	/* XXX: does dsr also represent inputs? */
	return ioread8(tc6393xb->scr + SCR_GPO_DSR(offset / 8))
		& TC_GPIO_BIT(offset);
}

static void __tc6393xb_gpio_set(struct gpio_chip *chip,
		unsigned offset, int value)
{
	struct tc6393xb *tc6393xb = container_of(chip, struct tc6393xb, gpio);
	u8  dsr;

	dsr = ioread8(tc6393xb->scr + SCR_GPO_DSR(offset / 8));
	if (value)
		dsr |= TC_GPIO_BIT(offset);
	else
		dsr &= ~TC_GPIO_BIT(offset);

	iowrite8(dsr, tc6393xb->scr + SCR_GPO_DSR(offset / 8));
}

static void tc6393xb_gpio_set(struct gpio_chip *chip,
		unsigned offset, int value)
{
	struct tc6393xb *tc6393xb = container_of(chip, struct tc6393xb, gpio);
	unsigned long flags;

	spin_lock_irqsave(&tc6393xb->lock, flags);

	__tc6393xb_gpio_set(chip, offset, value);

	spin_unlock_irqrestore(&tc6393xb->lock, flags);
}

static int tc6393xb_gpio_direction_input(struct gpio_chip *chip,
			unsigned offset)
{
	struct tc6393xb *tc6393xb = container_of(chip, struct tc6393xb, gpio);
	unsigned long flags;
	u8 doecr;

	spin_lock_irqsave(&tc6393xb->lock, flags);

	doecr = ioread8(tc6393xb->scr + SCR_GPO_DOECR(offset / 8));
	doecr &= ~TC_GPIO_BIT(offset);
	iowrite8(doecr, tc6393xb->scr + SCR_GPO_DOECR(offset / 8));

	spin_unlock_irqrestore(&tc6393xb->lock, flags);

	return 0;
}

static int tc6393xb_gpio_direction_output(struct gpio_chip *chip,
			unsigned offset, int value)
{
	struct tc6393xb *tc6393xb = container_of(chip, struct tc6393xb, gpio);
	unsigned long flags;
	u8 doecr;

	spin_lock_irqsave(&tc6393xb->lock, flags);

	__tc6393xb_gpio_set(chip, offset, value);

	doecr = ioread8(tc6393xb->scr + SCR_GPO_DOECR(offset / 8));
	doecr |= TC_GPIO_BIT(offset);
	iowrite8(doecr, tc6393xb->scr + SCR_GPO_DOECR(offset / 8));

	spin_unlock_irqrestore(&tc6393xb->lock, flags);

	return 0;
}

static int tc6393xb_register_gpio(struct tc6393xb *tc6393xb, int gpio_base)
{
	tc6393xb->gpio.label = "tc6393xb";
	tc6393xb->gpio.base = gpio_base;
	tc6393xb->gpio.ngpio = 16;
	tc6393xb->gpio.set = tc6393xb_gpio_set;
	tc6393xb->gpio.get = tc6393xb_gpio_get;
	tc6393xb->gpio.direction_input = tc6393xb_gpio_direction_input;
	tc6393xb->gpio.direction_output = tc6393xb_gpio_direction_output;

	return gpiochip_add(&tc6393xb->gpio);
}

/*--------------------------------------------------------------------------*/

static void
tc6393xb_irq(unsigned int irq, struct irq_desc *desc)
{
	struct tc6393xb *tc6393xb = get_irq_data(irq);
	unsigned int isr;
	unsigned int i, irq_base;

	irq_base = tc6393xb->irq_base;

	while ((isr = ioread8(tc6393xb->scr + SCR_ISR) &
				~ioread8(tc6393xb->scr + SCR_IMR)))
		for (i = 0; i < TC6393XB_NR_IRQS; i++) {
			if (isr & (1 << i))
				generic_handle_irq(irq_base + i);
		}
}

static void tc6393xb_irq_ack(unsigned int irq)
{
}

static void tc6393xb_irq_mask(unsigned int irq)
{
	struct tc6393xb *tc6393xb = get_irq_chip_data(irq);
	unsigned long flags;
	u8 imr;

	spin_lock_irqsave(&tc6393xb->lock, flags);
	imr = ioread8(tc6393xb->scr + SCR_IMR);
	imr |= 1 << (irq - tc6393xb->irq_base);
	iowrite8(imr, tc6393xb->scr + SCR_IMR);
	spin_unlock_irqrestore(&tc6393xb->lock, flags);
}

static void tc6393xb_irq_unmask(unsigned int irq)
{
	struct tc6393xb *tc6393xb = get_irq_chip_data(irq);
	unsigned long flags;
	u8 imr;

	spin_lock_irqsave(&tc6393xb->lock, flags);
	imr = ioread8(tc6393xb->scr + SCR_IMR);
	imr &= ~(1 << (irq - tc6393xb->irq_base));
	iowrite8(imr, tc6393xb->scr + SCR_IMR);
	spin_unlock_irqrestore(&tc6393xb->lock, flags);
}

static struct irq_chip tc6393xb_chip = {
	.name	= "tc6393xb",
	.ack	= tc6393xb_irq_ack,
	.mask	= tc6393xb_irq_mask,
	.unmask	= tc6393xb_irq_unmask,
};

static void tc6393xb_attach_irq(struct platform_device *dev)
{
	struct tc6393xb *tc6393xb = platform_get_drvdata(dev);
	unsigned int irq, irq_base;

	irq_base = tc6393xb->irq_base;

	for (irq = irq_base; irq < irq_base + TC6393XB_NR_IRQS; irq++) {
		set_irq_chip(irq, &tc6393xb_chip);
		set_irq_chip_data(irq, tc6393xb);
		set_irq_handler(irq, handle_edge_irq);
		set_irq_flags(irq, IRQF_VALID | IRQF_PROBE);
	}

	set_irq_type(tc6393xb->irq, IRQT_FALLING);
	set_irq_data(tc6393xb->irq, tc6393xb);
	set_irq_chained_handler(tc6393xb->irq, tc6393xb_irq);
}

static void tc6393xb_detach_irq(struct platform_device *dev)
{
	struct tc6393xb *tc6393xb = platform_get_drvdata(dev);
	unsigned int irq, irq_base;

	set_irq_chained_handler(tc6393xb->irq, NULL);
	set_irq_data(tc6393xb->irq, NULL);

	irq_base = tc6393xb->irq_base;

	for (irq = irq_base; irq < irq_base + TC6393XB_NR_IRQS; irq++) {
		set_irq_flags(irq, 0);
		set_irq_chip(irq, NULL);
		set_irq_chip_data(irq, NULL);
	}
}

/*--------------------------------------------------------------------------*/

static int tc6393xb_hw_init(struct platform_device *dev)
{
	struct tc6393xb_platform_data *tcpd = dev->dev.platform_data;
	struct tc6393xb *tc6393xb = platform_get_drvdata(dev);
	int i;

	iowrite8(tc6393xb->suspend_state.fer,	tc6393xb->scr + SCR_FER);
	iowrite16(tcpd->scr_pll2cr,		tc6393xb->scr + SCR_PLL2CR);
	iowrite16(tc6393xb->suspend_state.ccr,	tc6393xb->scr + SCR_CCR);
	iowrite16(SCR_MCR_RDY_OPENDRAIN | SCR_MCR_RDY_UNK | SCR_MCR_RDY_EN |
		  SCR_MCR_INT_OPENDRAIN | SCR_MCR_INT_UNK | SCR_MCR_INT_EN |
		  BIT(15),			tc6393xb->scr + SCR_MCR);
	iowrite16(tcpd->scr_gper,		tc6393xb->scr + SCR_GPER);
	iowrite8(0,				tc6393xb->scr + SCR_IRR);
	iowrite8(0xbf,				tc6393xb->scr + SCR_IMR);

	for (i = 0; i < 3; i++) {
		iowrite8(tc6393xb->suspend_state.gpo_dsr[i],
					tc6393xb->scr + SCR_GPO_DSR(i));
		iowrite8(tc6393xb->suspend_state.gpo_doecr[i],
					tc6393xb->scr + SCR_GPO_DOECR(i));
		iowrite8(tc6393xb->suspend_state.gpi_bcr[i],
					tc6393xb->scr + SCR_GPI_BCR(i));
	}

	return 0;
}

static int __devinit tc6393xb_probe(struct platform_device *dev)
{
	struct tc6393xb_platform_data *tcpd = dev->dev.platform_data;
	struct tc6393xb *tc6393xb;
	struct resource *iomem;
	struct resource *rscr;
	int retval, temp;
	int i;

	iomem = platform_get_resource(dev, IORESOURCE_MEM, 0);
	if (!iomem)
		return -EINVAL;

	tc6393xb = kzalloc(sizeof *tc6393xb, GFP_KERNEL);
	if (!tc6393xb) {
		retval = -ENOMEM;
		goto err_kzalloc;
	}

	spin_lock_init(&tc6393xb->lock);

	platform_set_drvdata(dev, tc6393xb);
	tc6393xb->iomem = iomem;
	tc6393xb->irq = platform_get_irq(dev, 0);
	tc6393xb->irq_base = tcpd->irq_base;

	tc6393xb->clk = clk_get(&dev->dev, "GPIO27_CLK" /* "CK3P6MI" */);
	if (IS_ERR(tc6393xb->clk)) {
		retval = PTR_ERR(tc6393xb->clk);
		goto err_clk_get;
	}

	rscr = &tc6393xb->rscr;
	rscr->name = "tc6393xb-core";
	rscr->start = iomem->start;
	rscr->end = iomem->start + 0xff;
	rscr->flags = IORESOURCE_MEM;

	retval = request_resource(iomem, rscr);
	if (retval)
		goto err_request_scr;

	tc6393xb->scr = ioremap(rscr->start, rscr->end - rscr->start + 1);
	if (!tc6393xb->scr) {
		retval = -ENOMEM;
		goto err_ioremap;
	}

	retval = clk_enable(tc6393xb->clk);
	if (retval)
		goto err_clk_enable;

	retval = tcpd->enable(dev);
	if (retval)
		goto err_enable;

	tc6393xb->suspend_state.fer = 0;
	for (i = 0; i < 3; i++) {
		tc6393xb->suspend_state.gpo_dsr[i] =
			(tcpd->scr_gpo_dsr >> (8 * i)) & 0xff;
		tc6393xb->suspend_state.gpo_doecr[i] =
			(tcpd->scr_gpo_doecr >> (8 * i)) & 0xff;
	}
	/*
	 * It may be necessary to change this back to
	 * platform-dependant code
	 */
	tc6393xb->suspend_state.ccr = SCR_CCR_UNK1 |
					SCR_CCR_HCLK_48;

	retval = tc6393xb_hw_init(dev);
	if (retval)
		goto err_hw_init;

	printk(KERN_INFO "Toshiba tc6393xb revision %d at 0x%08lx, irq %d\n",
			ioread8(tc6393xb->scr + SCR_REVID),
			(unsigned long) iomem->start, tc6393xb->irq);

	tc6393xb->gpio.base = -1;

	if (tcpd->gpio_base >= 0) {
		retval = tc6393xb_register_gpio(tc6393xb, tcpd->gpio_base);
		if (retval)
			goto err_gpio_add;
	}

	if (tc6393xb->irq)
		tc6393xb_attach_irq(dev);

	return 0;

	if (tc6393xb->irq)
		tc6393xb_detach_irq(dev);

err_gpio_add:
	if (tc6393xb->gpio.base != -1)
		temp = gpiochip_remove(&tc6393xb->gpio);
err_hw_init:
	tcpd->disable(dev);
err_clk_enable:
	clk_disable(tc6393xb->clk);
err_enable:
	iounmap(tc6393xb->scr);
err_ioremap:
	release_resource(&tc6393xb->rscr);
err_request_scr:
	clk_put(tc6393xb->clk);
err_clk_get:
	kfree(tc6393xb);
err_kzalloc:
	return retval;
}

static int __devexit tc6393xb_remove(struct platform_device *dev)
{
	struct tc6393xb_platform_data *tcpd = dev->dev.platform_data;
	struct tc6393xb *tc6393xb = platform_get_drvdata(dev);
	int ret;

	if (tc6393xb->irq)
		tc6393xb_detach_irq(dev);

	if (tc6393xb->gpio.base != -1) {
		ret = gpiochip_remove(&tc6393xb->gpio);
		if (ret) {
			dev_err(&dev->dev, "Can't remove gpio chip: %d\n", ret);
			return ret;
		}
	}

	ret = tcpd->disable(dev);

	clk_disable(tc6393xb->clk);

	iounmap(tc6393xb->scr);

	release_resource(&tc6393xb->rscr);

	platform_set_drvdata(dev, NULL);

	clk_put(tc6393xb->clk);

	kfree(tc6393xb);

	return ret;
}

#ifdef CONFIG_PM
static int tc6393xb_suspend(struct platform_device *dev, pm_message_t state)
{
	struct tc6393xb_platform_data *tcpd = dev->dev.platform_data;
	struct tc6393xb *tc6393xb = platform_get_drvdata(dev);
	int i;


	tc6393xb->suspend_state.ccr = ioread16(tc6393xb->scr + SCR_CCR);
	tc6393xb->suspend_state.fer = ioread8(tc6393xb->scr + SCR_FER);

	for (i = 0; i < 3; i++) {
		tc6393xb->suspend_state.gpo_dsr[i] =
			ioread8(tc6393xb->scr + SCR_GPO_DSR(i));
		tc6393xb->suspend_state.gpo_doecr[i] =
			ioread8(tc6393xb->scr + SCR_GPO_DOECR(i));
		tc6393xb->suspend_state.gpi_bcr[i] =
			ioread8(tc6393xb->scr + SCR_GPI_BCR(i));
	}

	return tcpd->suspend(dev);
}

static int tc6393xb_resume(struct platform_device *dev)
{
	struct tc6393xb_platform_data *tcpd = dev->dev.platform_data;
	int ret = tcpd->resume(dev);

	if (ret)
		return ret;

	return tc6393xb_hw_init(dev);
}
#else
#define tc6393xb_suspend NULL
#define tc6393xb_resume NULL
#endif

static struct platform_driver tc6393xb_driver = {
	.probe = tc6393xb_probe,
	.remove = __devexit_p(tc6393xb_remove),
	.suspend = tc6393xb_suspend,
	.resume = tc6393xb_resume,

	.driver = {
		.name = "tc6393xb",
		.owner = THIS_MODULE,
	},
};

static int __init tc6393xb_init(void)
{
	return platform_driver_register(&tc6393xb_driver);
}

static void __exit tc6393xb_exit(void)
{
	platform_driver_unregister(&tc6393xb_driver);
}

subsys_initcall(tc6393xb_init);
module_exit(tc6393xb_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ian Molton, Dmitry Baryshkov and Dirk Opfer");
MODULE_DESCRIPTION("tc6393xb Toshiba Mobile IO Controller");
MODULE_ALIAS("platform:tc6393xb");
