/*
 * linux/arch/sh/boards/se/770x/irq.c
 *
 * Copyright (C) 2000  Kazumoto Kojima
 * Copyright (C) 2006  Nobuhiro Iwamatsu
 *
 * Hitachi SolutionEngine Support.
 *
 */

#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <asm/irq.h>
#include <asm/io.h>
#include <asm/se.h>

/* 
 * If the problem of make_ipr_irq is solved, 
 * this code will become unnecessary. :-) 
 */
static void se770x_disable_ipr_irq(unsigned int irq)
{
	struct ipr_data *p = get_irq_chip_data(irq);

	ctrl_outw(ctrl_inw(p->addr) & (0xffff ^ (0xf << p->shift)), p->addr);
}

static void se770x_enable_ipr_irq(unsigned int irq)
{
	struct ipr_data *p = get_irq_chip_data(irq);

	ctrl_outw(ctrl_inw(p->addr) | (p->priority << p->shift), p->addr);
}

static struct irq_chip se770x_irq_chip = {
	.name           = "MS770xSE-FPGA",
	.mask           = se770x_disable_ipr_irq,
	.unmask         = se770x_enable_ipr_irq,
	.mask_ack       = se770x_disable_ipr_irq,
};

void make_se770x_irq(struct ipr_data *table, unsigned int nr_irqs)
{
	int i;

	for (i = 0; i < nr_irqs; i++) {
		unsigned int irq = table[i].irq;
		disable_irq_nosync(irq);
		set_irq_chip_and_handler_name(irq, &se770x_irq_chip,
			handle_level_irq, "level");
		set_irq_chip_data(irq, &table[i]);
		se770x_enable_ipr_irq(irq);
	}
}

static struct ipr_data se770x_ipr_map[] = {
#if defined(CONFIG_CPU_SUBTYPE_SH7705)
	/* This is default value */
	{ 0xf-0x2, 0, 8,  0x2 , BCR_ILCRA},
	{ 0xf-0xa, 0, 4,  0xa , BCR_ILCRA},
	{ 0xf-0x5, 0, 0,  0x5 , BCR_ILCRB},
	{ 0xf-0x8, 0, 4,  0x8 , BCR_ILCRC},
	{ 0xf-0xc, 0, 0,  0xc , BCR_ILCRC},
	{ 0xf-0xe, 0, 12, 0xe , BCR_ILCRD},
	{ 0xf-0x3, 0, 4,  0x3 , BCR_ILCRD}, /* LAN */
	{ 0xf-0xd, 0, 8,  0xd , BCR_ILCRE},
	{ 0xf-0x9, 0, 4,  0x9 , BCR_ILCRE},
	{ 0xf-0x1, 0, 0,  0x1 , BCR_ILCRE},
	{ 0xf-0xf, 0, 12, 0xf , BCR_ILCRF},
	{ 0xf-0xb, 0, 4,  0xb , BCR_ILCRF},
	{ 0xf-0x7, 0, 12, 0x7 , BCR_ILCRG},
	{ 0xf-0x6, 0, 8,  0x6 , BCR_ILCRG},
	{ 0xf-0x4, 0, 4,  0x4 , BCR_ILCRG},
#else
	{ 14, 0,  8, 0x0f-14 ,BCR_ILCRA},
	{ 12, 0,  4, 0x0f-12 ,BCR_ILCRA},
	{  8, 0,  4, 0x0f- 8 ,BCR_ILCRB},
	{  6, 0, 12, 0x0f- 6 ,BCR_ILCRC},
	{  5, 0,  8, 0x0f- 5 ,BCR_ILCRC},
	{  4, 0,  4, 0x0f- 4 ,BCR_ILCRC},
	{  3, 0,  0, 0x0f- 3 ,BCR_ILCRC},
	{  1, 0, 12, 0x0f- 1 ,BCR_ILCRD},
	/* ST NIC */
	{ 10, 0,  4, 0x0f-10 ,BCR_ILCRD}, 	/* LAN */
	/* MRSHPC IRQs setting */
	{  0, 0, 12, 0x0f- 0 ,BCR_ILCRE},	/* PCIRQ3 */
	{ 11, 0,  8, 0x0f-11 ,BCR_ILCRE}, 	/* PCIRQ2 */
	{  9, 0,  4, 0x0f- 9 ,BCR_ILCRE}, 	/* PCIRQ1 */
	{  7, 0,  0, 0x0f- 7 ,BCR_ILCRE}, 	/* PCIRQ0 */
	/* #2, #13 are allocated for SLOT IRQ #1 and #2 (for now) */
	/* NOTE: #2 and #13 are not used on PC */
	{ 13, 0,  4, 0x0f-13 ,BCR_ILCRG}, 	/* SLOTIRQ2 */
	{  2, 0,  0, 0x0f- 2 ,BCR_ILCRG}, 	/* SLOTIRQ1 */
#endif
};

/*
 * Initialize IRQ setting
 */
void __init init_se_IRQ(void)
{
        /*
         * Super I/O (Just mimic PC):
         *  1: keyboard
         *  3: serial 0
         *  4: serial 1
         *  5: printer
         *  6: floppy
         *  8: rtc
         * 12: mouse
         * 14: ide0
         */
#if defined(CONFIG_CPU_SUBTYPE_SH7705)
	/* Disable all interrupts */
	ctrl_outw(0, BCR_ILCRA);
	ctrl_outw(0, BCR_ILCRB);
	ctrl_outw(0, BCR_ILCRC);
	ctrl_outw(0, BCR_ILCRD);
	ctrl_outw(0, BCR_ILCRE);
	ctrl_outw(0, BCR_ILCRF);
	ctrl_outw(0, BCR_ILCRG);
#endif
	make_se770x_irq(se770x_ipr_map, ARRAY_SIZE(se770x_ipr_map));
}
