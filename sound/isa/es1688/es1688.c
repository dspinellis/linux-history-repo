/*
 *  Driver for generic ESS AudioDrive ESx688 soundcards
 *  Copyright (c) by Jaroslav Kysela <perex@suse.cz>
 *
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 *
 */

#include <sound/driver.h>
#include <linux/init.h>
#include <linux/err.h>
#include <linux/isa.h>
#include <linux/time.h>
#include <linux/wait.h>
#include <linux/moduleparam.h>
#include <asm/dma.h>
#include <sound/core.h>
#include <sound/es1688.h>
#include <sound/mpu401.h>
#include <sound/opl3.h>
#define SNDRV_LEGACY_FIND_FREE_IRQ
#define SNDRV_LEGACY_FIND_FREE_DMA
#include <sound/initval.h>

#define CRD_NAME "Generic ESS ES1688/ES688 AudioDrive"
#define DEV_NAME "es1688"

MODULE_DESCRIPTION(CRD_NAME);
MODULE_AUTHOR("Jaroslav Kysela <perex@suse.cz>");
MODULE_LICENSE("GPL");
MODULE_SUPPORTED_DEVICE("{{ESS,ES688 PnP AudioDrive,pnp:ESS0100},"
	        "{ESS,ES1688 PnP AudioDrive,pnp:ESS0102},"
	        "{ESS,ES688 AudioDrive,pnp:ESS6881},"
	        "{ESS,ES1688 AudioDrive,pnp:ESS1681}}");

static int index[SNDRV_CARDS] = SNDRV_DEFAULT_IDX;	/* Index 0-MAX */
static char *id[SNDRV_CARDS] = SNDRV_DEFAULT_STR;	/* ID for this card */
static int enable[SNDRV_CARDS] = SNDRV_DEFAULT_ENABLE;	/* Enable this card */
static long port[SNDRV_CARDS] = SNDRV_DEFAULT_PORT;	/* 0x220,0x240,0x260 */
static long mpu_port[SNDRV_CARDS] = {[0 ... (SNDRV_CARDS - 1)] = -1};
static int irq[SNDRV_CARDS] = SNDRV_DEFAULT_IRQ;	/* 5,7,9,10 */
static int mpu_irq[SNDRV_CARDS] = SNDRV_DEFAULT_IRQ;	/* 5,7,9,10 */
static int dma8[SNDRV_CARDS] = SNDRV_DEFAULT_DMA;	/* 0,1,3 */

module_param_array(index, int, NULL, 0444);
MODULE_PARM_DESC(index, "Index value for " CRD_NAME " soundcard.");
module_param_array(id, charp, NULL, 0444);
MODULE_PARM_DESC(id, "ID string for " CRD_NAME " soundcard.");
module_param_array(enable, bool, NULL, 0444);
MODULE_PARM_DESC(enable, "Enable " CRD_NAME " soundcard.");
module_param_array(port, long, NULL, 0444);
MODULE_PARM_DESC(port, "Port # for " CRD_NAME " driver.");
module_param_array(mpu_port, long, NULL, 0444);
MODULE_PARM_DESC(mpu_port, "MPU-401 port # for " CRD_NAME " driver.");
module_param_array(irq, int, NULL, 0444);
MODULE_PARM_DESC(irq, "IRQ # for " CRD_NAME " driver.");
module_param_array(mpu_irq, int, NULL, 0444);
MODULE_PARM_DESC(mpu_irq, "MPU-401 IRQ # for " CRD_NAME " driver.");
module_param_array(dma8, int, NULL, 0444);
MODULE_PARM_DESC(dma8, "8-bit DMA # for " CRD_NAME " driver.");

static int __devinit snd_es1688_match(struct device *dev, unsigned int n)
{
	return enable[n];
}

static int __devinit snd_es1688_legacy_create(struct snd_card *card, 
		struct device *dev, unsigned int n, struct snd_es1688 **rchip)
{
	static long possible_ports[] = {0x220, 0x240, 0x260};
	static int possible_irqs[] = {5, 9, 10, 7, -1};
	static int possible_dmas[] = {1, 3, 0, -1};

	int i, error;

	if (irq[n] == SNDRV_AUTO_IRQ) {
		irq[n] = snd_legacy_find_free_irq(possible_irqs);
		if (irq[n] < 0) {
			snd_printk(KERN_ERR "%s: unable to find a free IRQ\n",
				dev->bus_id);
			return -EBUSY;
		}
	}
	if (dma8[n] == SNDRV_AUTO_DMA) {
		dma8[n] = snd_legacy_find_free_dma(possible_dmas);
		if (dma8[n] < 0) {
			snd_printk(KERN_ERR "%s: unable to find a free DMA\n",
				dev->bus_id);
			return -EBUSY;
		}
	}

	if (port[n] != SNDRV_AUTO_PORT)
		return snd_es1688_create(card, port[n], mpu_port[n], irq[n],
				mpu_irq[n], dma8[n], ES1688_HW_AUTO, rchip);

	i = 0;
	do {
		port[n] = possible_ports[i];
		error = snd_es1688_create(card, port[n], mpu_port[n], irq[n],
				mpu_irq[n], dma8[n], ES1688_HW_AUTO, rchip);
	} while (error < 0 && ++i < ARRAY_SIZE(possible_ports));

	return error;
}

static int __devinit snd_es1688_probe(struct device *dev, unsigned int n)
{
	struct snd_card *card;
	struct snd_es1688 *chip;
	struct snd_opl3 *opl3;
	struct snd_pcm *pcm;
	int error;

	card = snd_card_new(index[n], id[n], THIS_MODULE, 0);
	if (!card)
		return -EINVAL;

	error = snd_es1688_legacy_create(card, dev, n, &chip);
	if (error < 0)
		goto out;

	error = snd_es1688_pcm(chip, 0, &pcm);
	if (error < 0)
		goto out;

	error = snd_es1688_mixer(chip);
	if (error < 0)
		goto out;

	strcpy(card->driver, "ES1688");
	strcpy(card->shortname, pcm->name);
	sprintf(card->longname, "%s at 0x%lx, irq %i, dma %i", pcm->name,
		chip->port, chip->irq, chip->dma8);

	if (snd_opl3_create(card, chip->port, chip->port + 2,
			OPL3_HW_OPL3, 0, &opl3) < 0)
		printk(KERN_WARNING "%s: opl3 not detected at 0x%lx\n",
			dev->bus_id, chip->port);
	else {
		error =	snd_opl3_hwdep_new(opl3, 0, 1, NULL);
		if (error < 0)
			goto out;
	}

	if (mpu_irq[n] >= 0 && mpu_irq[n] != SNDRV_AUTO_IRQ &&
			chip->mpu_port > 0) {
		error = snd_mpu401_uart_new(card, 0, MPU401_HW_ES1688,
				chip->mpu_port, 0,
				mpu_irq[n], IRQF_DISABLED, NULL);
		if (error < 0)
			goto out;
	}

	snd_card_set_dev(card, dev);

	error = snd_card_register(card);
	if (error < 0)
		goto out;

	dev_set_drvdata(dev, card);
	return 0;

out:	snd_card_free(card);
	return error;
}

static int __devexit snd_es1688_remove(struct device *dev, unsigned int n)
{
	snd_card_free(dev_get_drvdata(dev));
	dev_set_drvdata(dev, NULL);
	return 0;
}

static struct isa_driver snd_es1688_driver = {
	.match		= snd_es1688_match,
	.probe		= snd_es1688_probe,
	.remove		= snd_es1688_remove,
#if 0	/* FIXME */
	.suspend	= snd_es1688_suspend,
	.resume		= snd_es1688_resume,
#endif
	.driver		= {
		.name	= DEV_NAME
	}
};

static int __init alsa_card_es1688_init(void)
{
	return isa_register_driver(&snd_es1688_driver, SNDRV_CARDS);
}

static void __exit alsa_card_es1688_exit(void)
{
	isa_unregister_driver(&snd_es1688_driver);
}

module_init(alsa_card_es1688_init);
module_exit(alsa_card_es1688_exit);
