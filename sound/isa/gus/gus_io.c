/*
 *  Copyright (c) by Jaroslav Kysela <perex@suse.cz>
 *  I/O routines for GF1/InterWave synthesizer chips
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
#include <linux/delay.h>
#include <linux/time.h>
#include <sound/core.h>
#include <sound/gus.h>

void snd_gf1_delay(snd_gus_card_t * gus)
{
	int i;

	for (i = 0; i < 6; i++) {
		mb();
		inb(GUSP(gus, DRAM));
	}
}

/*
 *  =======================================================================
 */

/*
 *  ok.. stop of control registers (wave & ramp) need some special things..
 *       big UltraClick (tm) elimination...
 */

static inline void __snd_gf1_ctrl_stop(snd_gus_card_t * gus, unsigned char reg)
{
	unsigned char value;

	outb(reg | 0x80, gus->gf1.reg_regsel);
	mb();
	value = inb(gus->gf1.reg_data8);
	mb();
	outb(reg, gus->gf1.reg_regsel);
	mb();
	outb((value | 0x03) & ~(0x80 | 0x20), gus->gf1.reg_data8);
	mb();
}

static inline void __snd_gf1_write8(snd_gus_card_t * gus,
				    unsigned char reg,
				    unsigned char data)
{
	outb(reg, gus->gf1.reg_regsel);
	mb();
	outb(data, gus->gf1.reg_data8);
	mb();
}

static inline unsigned char __snd_gf1_look8(snd_gus_card_t * gus,
					    unsigned char reg)
{
	outb(reg, gus->gf1.reg_regsel);
	mb();
	return inb(gus->gf1.reg_data8);
}

static inline void __snd_gf1_write16(snd_gus_card_t * gus,
				     unsigned char reg, unsigned int data)
{
	outb(reg, gus->gf1.reg_regsel);
	mb();
	outw((unsigned short) data, gus->gf1.reg_data16);
	mb();
}

static inline unsigned short __snd_gf1_look16(snd_gus_card_t * gus,
					      unsigned char reg)
{
	outb(reg, gus->gf1.reg_regsel);
	mb();
	return inw(gus->gf1.reg_data16);
}

static inline void __snd_gf1_adlib_write(snd_gus_card_t * gus,
					 unsigned char reg, unsigned char data)
{
	outb(reg, gus->gf1.reg_timerctrl);
	inb(gus->gf1.reg_timerctrl);
	inb(gus->gf1.reg_timerctrl);
	outb(data, gus->gf1.reg_timerdata);
	inb(gus->gf1.reg_timerctrl);
	inb(gus->gf1.reg_timerctrl);
}

static inline void __snd_gf1_write_addr(snd_gus_card_t * gus, unsigned char reg,
                                        unsigned int addr, int w_16bit)
{
	if (gus->gf1.enh_mode) {
		if (w_16bit)
			addr = ((addr >> 1) & ~0x0000000f) | (addr & 0x0000000f);
		__snd_gf1_write8(gus, SNDRV_GF1_VB_UPPER_ADDRESS, (unsigned char) ((addr >> 26) & 0x03));
	} else if (w_16bit)
		addr = (addr & 0x00c0000f) | ((addr & 0x003ffff0) >> 1);
	__snd_gf1_write16(gus, reg, (unsigned short) (addr >> 11));
	__snd_gf1_write16(gus, reg + 1, (unsigned short) (addr << 5));
}

static inline unsigned int __snd_gf1_read_addr(snd_gus_card_t * gus,
					       unsigned char reg, short w_16bit)
{
	unsigned int res;

	res = ((unsigned int) __snd_gf1_look16(gus, reg | 0x80) << 11) & 0xfff800;
	res |= ((unsigned int) __snd_gf1_look16(gus, (reg + 1) | 0x80) >> 5) & 0x0007ff;
	if (gus->gf1.enh_mode) {
		res |= (unsigned int) __snd_gf1_look8(gus, SNDRV_GF1_VB_UPPER_ADDRESS | 0x80) << 26;
		if (w_16bit)
			res = ((res << 1) & 0xffffffe0) | (res & 0x0000000f);
	} else if (w_16bit)
		res = ((res & 0x001ffff0) << 1) | (res & 0x00c0000f);
	return res;
}


/*
 *  =======================================================================
 */

void snd_gf1_ctrl_stop(snd_gus_card_t * gus, unsigned char reg)
{
	__snd_gf1_ctrl_stop(gus, reg);
}

void snd_gf1_write8(snd_gus_card_t * gus,
		    unsigned char reg,
		    unsigned char data)
{
	__snd_gf1_write8(gus, reg, data);
}

unsigned char snd_gf1_look8(snd_gus_card_t * gus, unsigned char reg)
{
	return __snd_gf1_look8(gus, reg);
}

void snd_gf1_write16(snd_gus_card_t * gus,
		     unsigned char reg,
		     unsigned int data)
{
	__snd_gf1_write16(gus, reg, data);
}

unsigned short snd_gf1_look16(snd_gus_card_t * gus, unsigned char reg)
{
	return __snd_gf1_look16(gus, reg);
}

void snd_gf1_adlib_write(snd_gus_card_t * gus,
                         unsigned char reg,
                         unsigned char data)
{
	__snd_gf1_adlib_write(gus, reg, data);
}

void snd_gf1_write_addr(snd_gus_card_t * gus, unsigned char reg,
                        unsigned int addr, short w_16bit)
{
	__snd_gf1_write_addr(gus, reg, addr, w_16bit);
}

unsigned int snd_gf1_read_addr(snd_gus_card_t * gus,
                               unsigned char reg,
                               short w_16bit)
{
	return __snd_gf1_read_addr(gus, reg, w_16bit);
}

/*

 */

void snd_gf1_i_ctrl_stop(snd_gus_card_t * gus, unsigned char reg)
{
	unsigned long flags;

	spin_lock_irqsave(&gus->reg_lock, flags);
	__snd_gf1_ctrl_stop(gus, reg);
	spin_unlock_irqrestore(&gus->reg_lock, flags);
}

void snd_gf1_i_write8(snd_gus_card_t * gus,
		      unsigned char reg,
                      unsigned char data)
{
	unsigned long flags;

	spin_lock_irqsave(&gus->reg_lock, flags);
	__snd_gf1_write8(gus, reg, data);
	spin_unlock_irqrestore(&gus->reg_lock, flags);
}

unsigned char snd_gf1_i_look8(snd_gus_card_t * gus, unsigned char reg)
{
	unsigned long flags;
	unsigned char res;

	spin_lock_irqsave(&gus->reg_lock, flags);
	res = __snd_gf1_look8(gus, reg);
	spin_unlock_irqrestore(&gus->reg_lock, flags);
	return res;
}

void snd_gf1_i_write16(snd_gus_card_t * gus,
		       unsigned char reg,
		       unsigned int data)
{
	unsigned long flags;

	spin_lock_irqsave(&gus->reg_lock, flags);
	__snd_gf1_write16(gus, reg, data);
	spin_unlock_irqrestore(&gus->reg_lock, flags);
}

unsigned short snd_gf1_i_look16(snd_gus_card_t * gus, unsigned char reg)
{
	unsigned long flags;
	unsigned short res;

	spin_lock_irqsave(&gus->reg_lock, flags);
	res = __snd_gf1_look16(gus, reg);
	spin_unlock_irqrestore(&gus->reg_lock, flags);
	return res;
}

#if 0

void snd_gf1_i_adlib_write(snd_gus_card_t * gus,
		           unsigned char reg,
		           unsigned char data)
{
	unsigned long flags;

	spin_lock_irqsave(&gus->reg_lock, flags);
	__snd_gf1_adlib_write(gus, reg, data);
	spin_unlock_irqrestore(&gus->reg_lock, flags);
}

void snd_gf1_i_write_addr(snd_gus_card_t * gus, unsigned char reg,
			  unsigned int addr, short w_16bit)
{
	unsigned long flags;

	spin_lock_irqsave(&gus->reg_lock, flags);
	__snd_gf1_write_addr(gus, reg, addr, w_16bit);
	spin_unlock_irqrestore(&gus->reg_lock, flags);
}

#endif  /*  0  */

unsigned int snd_gf1_i_read_addr(snd_gus_card_t * gus,
				 unsigned char reg, short w_16bit)
{
	unsigned int res;
	unsigned long flags;

	spin_lock_irqsave(&gus->reg_lock, flags);
	res = __snd_gf1_read_addr(gus, reg, w_16bit);
	spin_unlock_irqrestore(&gus->reg_lock, flags);
	return res;
}

/*

 */

void snd_gf1_dram_addr(snd_gus_card_t * gus, unsigned int addr)
{
	outb(0x43, gus->gf1.reg_regsel);
	mb();
	outw((unsigned short) addr, gus->gf1.reg_data16);
	mb();
	outb(0x44, gus->gf1.reg_regsel);
	mb();
	outb((unsigned char) (addr >> 16), gus->gf1.reg_data8);
	mb();
}

void snd_gf1_poke(snd_gus_card_t * gus, unsigned int addr, unsigned char data)
{
	unsigned long flags;

	spin_lock_irqsave(&gus->reg_lock, flags);
	outb(SNDRV_GF1_GW_DRAM_IO_LOW, gus->gf1.reg_regsel);
	mb();
	outw((unsigned short) addr, gus->gf1.reg_data16);
	mb();
	outb(SNDRV_GF1_GB_DRAM_IO_HIGH, gus->gf1.reg_regsel);
	mb();
	outb((unsigned char) (addr >> 16), gus->gf1.reg_data8);
	mb();
	outb(data, gus->gf1.reg_dram);
	spin_unlock_irqrestore(&gus->reg_lock, flags);
}

unsigned char snd_gf1_peek(snd_gus_card_t * gus, unsigned int addr)
{
	unsigned long flags;
	unsigned char res;

	spin_lock_irqsave(&gus->reg_lock, flags);
	outb(SNDRV_GF1_GW_DRAM_IO_LOW, gus->gf1.reg_regsel);
	mb();
	outw((unsigned short) addr, gus->gf1.reg_data16);
	mb();
	outb(SNDRV_GF1_GB_DRAM_IO_HIGH, gus->gf1.reg_regsel);
	mb();
	outb((unsigned char) (addr >> 16), gus->gf1.reg_data8);
	mb();
	res = inb(gus->gf1.reg_dram);
	spin_unlock_irqrestore(&gus->reg_lock, flags);
	return res;
}

#if 0

void snd_gf1_pokew(snd_gus_card_t * gus, unsigned int addr, unsigned short data)
{
	unsigned long flags;

#ifdef CONFIG_SND_DEBUG
	if (!gus->interwave)
		snd_printk("snd_gf1_pokew - GF1!!!\n");
#endif
	spin_lock_irqsave(&gus->reg_lock, flags);
	outb(SNDRV_GF1_GW_DRAM_IO_LOW, gus->gf1.reg_regsel);
	mb();
	outw((unsigned short) addr, gus->gf1.reg_data16);
	mb();
	outb(SNDRV_GF1_GB_DRAM_IO_HIGH, gus->gf1.reg_regsel);
	mb();
	outb((unsigned char) (addr >> 16), gus->gf1.reg_data8);
	mb();
	outb(SNDRV_GF1_GW_DRAM_IO16, gus->gf1.reg_regsel);
	mb();
	outw(data, gus->gf1.reg_data16);
	spin_unlock_irqrestore(&gus->reg_lock, flags);
}

unsigned short snd_gf1_peekw(snd_gus_card_t * gus, unsigned int addr)
{
	unsigned long flags;
	unsigned short res;

#ifdef CONFIG_SND_DEBUG
	if (!gus->interwave)
		snd_printk("snd_gf1_peekw - GF1!!!\n");
#endif
	spin_lock_irqsave(&gus->reg_lock, flags);
	outb(SNDRV_GF1_GW_DRAM_IO_LOW, gus->gf1.reg_regsel);
	mb();
	outw((unsigned short) addr, gus->gf1.reg_data16);
	mb();
	outb(SNDRV_GF1_GB_DRAM_IO_HIGH, gus->gf1.reg_regsel);
	mb();
	outb((unsigned char) (addr >> 16), gus->gf1.reg_data8);
	mb();
	outb(SNDRV_GF1_GW_DRAM_IO16, gus->gf1.reg_regsel);
	mb();
	res = inw(gus->gf1.reg_data16);
	spin_unlock_irqrestore(&gus->reg_lock, flags);
	return res;
}

void snd_gf1_dram_setmem(snd_gus_card_t * gus, unsigned int addr,
			 unsigned short value, unsigned int count)
{
	unsigned long port;
	unsigned long flags;

#ifdef CONFIG_SND_DEBUG
	if (!gus->interwave)
		snd_printk("snd_gf1_dram_setmem - GF1!!!\n");
#endif
	addr &= ~1;
	count >>= 1;
	port = GUSP(gus, GF1DATALOW);
	spin_lock_irqsave(&gus->reg_lock, flags);
	outb(SNDRV_GF1_GW_DRAM_IO_LOW, gus->gf1.reg_regsel);
	mb();
	outw((unsigned short) addr, gus->gf1.reg_data16);
	mb();
	outb(SNDRV_GF1_GB_DRAM_IO_HIGH, gus->gf1.reg_regsel);
	mb();
	outb((unsigned char) (addr >> 16), gus->gf1.reg_data8);
	mb();
	outb(SNDRV_GF1_GW_DRAM_IO16, gus->gf1.reg_regsel);
	while (count--)
		outw(value, port);
	spin_unlock_irqrestore(&gus->reg_lock, flags);
}

#endif  /*  0  */

void snd_gf1_select_active_voices(snd_gus_card_t * gus)
{
	unsigned short voices;

	static unsigned short voices_tbl[32 - 14 + 1] =
	{
	    44100, 41160, 38587, 36317, 34300, 32494, 30870, 29400, 28063, 26843,
	    25725, 24696, 23746, 22866, 22050, 21289, 20580, 19916, 19293
	};

	voices = gus->gf1.active_voices;
	if (voices > 32)
		voices = 32;
	if (voices < 14)
		voices = 14;
	if (gus->gf1.enh_mode)
		voices = 32;
	gus->gf1.active_voices = voices;
	gus->gf1.playback_freq =
	    gus->gf1.enh_mode ? 44100 : voices_tbl[voices - 14];
	if (!gus->gf1.enh_mode) {
		snd_gf1_i_write8(gus, SNDRV_GF1_GB_ACTIVE_VOICES, 0xc0 | (voices - 1));
		udelay(100);
	}
}

#ifdef CONFIG_SND_DEBUG

void snd_gf1_print_voice_registers(snd_gus_card_t * gus)
{
	unsigned char mode;
	int voice, ctrl;

	voice = gus->gf1.active_voice;
	printk(" -%i- GF1  voice ctrl, ramp ctrl  = 0x%x, 0x%x\n", voice, ctrl = snd_gf1_i_read8(gus, 0), snd_gf1_i_read8(gus, 0x0d));
	printk(" -%i- GF1  frequency              = 0x%x\n", voice, snd_gf1_i_read16(gus, 1));
	printk(" -%i- GF1  loop start, end        = 0x%x (0x%x), 0x%x (0x%x)\n", voice, snd_gf1_i_read_addr(gus, 2, ctrl & 4), snd_gf1_i_read_addr(gus, 2, (ctrl & 4) ^ 4), snd_gf1_i_read_addr(gus, 4, ctrl & 4), snd_gf1_i_read_addr(gus, 4, (ctrl & 4) ^ 4));
	printk(" -%i- GF1  ramp start, end, rate  = 0x%x, 0x%x, 0x%x\n", voice, snd_gf1_i_read8(gus, 7), snd_gf1_i_read8(gus, 8), snd_gf1_i_read8(gus, 6));
	printk(" -%i- GF1  volume                 = 0x%x\n", voice, snd_gf1_i_read16(gus, 9));
	printk(" -%i- GF1  position               = 0x%x (0x%x)\n", voice, snd_gf1_i_read_addr(gus, 0x0a, ctrl & 4), snd_gf1_i_read_addr(gus, 0x0a, (ctrl & 4) ^ 4));
	if (gus->interwave && snd_gf1_i_read8(gus, 0x19) & 0x01) {	/* enhanced mode */
		mode = snd_gf1_i_read8(gus, 0x15);
		printk(" -%i- GFA1 mode                   = 0x%x\n", voice, mode);
		if (mode & 0x01) {	/* Effect processor */
			printk(" -%i- GFA1 effect address         = 0x%x\n", voice, snd_gf1_i_read_addr(gus, 0x11, ctrl & 4));
			printk(" -%i- GFA1 effect volume          = 0x%x\n", voice, snd_gf1_i_read16(gus, 0x16));
			printk(" -%i- GFA1 effect volume final    = 0x%x\n", voice, snd_gf1_i_read16(gus, 0x1d));
			printk(" -%i- GFA1 effect acumulator      = 0x%x\n", voice, snd_gf1_i_read8(gus, 0x14));
		}
		if (mode & 0x20) {
			printk(" -%i- GFA1 left offset            = 0x%x (%i)\n", voice, snd_gf1_i_read16(gus, 0x13), snd_gf1_i_read16(gus, 0x13) >> 4);
			printk(" -%i- GFA1 left offset final      = 0x%x (%i)\n", voice, snd_gf1_i_read16(gus, 0x1c), snd_gf1_i_read16(gus, 0x1c) >> 4);
			printk(" -%i- GFA1 right offset           = 0x%x (%i)\n", voice, snd_gf1_i_read16(gus, 0x0c), snd_gf1_i_read16(gus, 0x0c) >> 4);
			printk(" -%i- GFA1 right offset final     = 0x%x (%i)\n", voice, snd_gf1_i_read16(gus, 0x1b), snd_gf1_i_read16(gus, 0x1b) >> 4);
		} else
			printk(" -%i- GF1  pan                    = 0x%x\n", voice, snd_gf1_i_read8(gus, 0x0c));
	} else
		printk(" -%i- GF1  pan                    = 0x%x\n", voice, snd_gf1_i_read8(gus, 0x0c));
}

#if 0

void snd_gf1_print_global_registers(snd_gus_card_t * gus)
{
	unsigned char global_mode = 0x00;

	printk(" -G- GF1 active voices            = 0x%x\n", snd_gf1_i_look8(gus, SNDRV_GF1_GB_ACTIVE_VOICES));
	if (gus->interwave) {
		global_mode = snd_gf1_i_read8(gus, SNDRV_GF1_GB_GLOBAL_MODE);
		printk(" -G- GF1 global mode              = 0x%x\n", global_mode);
	}
	if (global_mode & 0x02)	/* LFO enabled? */
		printk(" -G- GF1 LFO base                 = 0x%x\n", snd_gf1_i_look16(gus, SNDRV_GF1_GW_LFO_BASE));
	printk(" -G- GF1 voices IRQ read          = 0x%x\n", snd_gf1_i_look8(gus, SNDRV_GF1_GB_VOICES_IRQ_READ));
	printk(" -G- GF1 DRAM DMA control         = 0x%x\n", snd_gf1_i_look8(gus, SNDRV_GF1_GB_DRAM_DMA_CONTROL));
	printk(" -G- GF1 DRAM DMA high/low        = 0x%x/0x%x\n", snd_gf1_i_look8(gus, SNDRV_GF1_GB_DRAM_DMA_HIGH), snd_gf1_i_read16(gus, SNDRV_GF1_GW_DRAM_DMA_LOW));
	printk(" -G- GF1 DRAM IO high/low         = 0x%x/0x%x\n", snd_gf1_i_look8(gus, SNDRV_GF1_GB_DRAM_IO_HIGH), snd_gf1_i_read16(gus, SNDRV_GF1_GW_DRAM_IO_LOW));
	if (!gus->interwave)
		printk(" -G- GF1 record DMA control       = 0x%x\n", snd_gf1_i_look8(gus, SNDRV_GF1_GB_REC_DMA_CONTROL));
	printk(" -G- GF1 DRAM IO 16               = 0x%x\n", snd_gf1_i_look16(gus, SNDRV_GF1_GW_DRAM_IO16));
	if (gus->gf1.enh_mode) {
		printk(" -G- GFA1 memory config           = 0x%x\n", snd_gf1_i_look16(gus, SNDRV_GF1_GW_MEMORY_CONFIG));
		printk(" -G- GFA1 memory control          = 0x%x\n", snd_gf1_i_look8(gus, SNDRV_GF1_GB_MEMORY_CONTROL));
		printk(" -G- GFA1 FIFO record base        = 0x%x\n", snd_gf1_i_look16(gus, SNDRV_GF1_GW_FIFO_RECORD_BASE_ADDR));
		printk(" -G- GFA1 FIFO playback base      = 0x%x\n", snd_gf1_i_look16(gus, SNDRV_GF1_GW_FIFO_PLAY_BASE_ADDR));
		printk(" -G- GFA1 interleave control      = 0x%x\n", snd_gf1_i_look16(gus, SNDRV_GF1_GW_INTERLEAVE));
	}
}

void snd_gf1_print_setup_registers(snd_gus_card_t * gus)
{
	printk(" -S- mix control                  = 0x%x\n", inb(GUSP(gus, MIXCNTRLREG)));
	printk(" -S- IRQ status                   = 0x%x\n", inb(GUSP(gus, IRQSTAT)));
	printk(" -S- timer control                = 0x%x\n", inb(GUSP(gus, TIMERCNTRL)));
	printk(" -S- timer data                   = 0x%x\n", inb(GUSP(gus, TIMERDATA)));
	printk(" -S- status read                  = 0x%x\n", inb(GUSP(gus, REGCNTRLS)));
	printk(" -S- Sound Blaster control        = 0x%x\n", snd_gf1_i_look8(gus, SNDRV_GF1_GB_SOUND_BLASTER_CONTROL));
	printk(" -S- AdLib timer 1/2              = 0x%x/0x%x\n", snd_gf1_i_look8(gus, SNDRV_GF1_GB_ADLIB_TIMER_1), snd_gf1_i_look8(gus, SNDRV_GF1_GB_ADLIB_TIMER_2));
	printk(" -S- reset                        = 0x%x\n", snd_gf1_i_look8(gus, SNDRV_GF1_GB_RESET));
	if (gus->interwave) {
		printk(" -S- compatibility                = 0x%x\n", snd_gf1_i_look8(gus, SNDRV_GF1_GB_COMPATIBILITY));
		printk(" -S- decode control               = 0x%x\n", snd_gf1_i_look8(gus, SNDRV_GF1_GB_DECODE_CONTROL));
		printk(" -S- version number               = 0x%x\n", snd_gf1_i_look8(gus, SNDRV_GF1_GB_VERSION_NUMBER));
		printk(" -S- MPU-401 emul. control A/B    = 0x%x/0x%x\n", snd_gf1_i_look8(gus, SNDRV_GF1_GB_MPU401_CONTROL_A), snd_gf1_i_look8(gus, SNDRV_GF1_GB_MPU401_CONTROL_B));
		printk(" -S- emulation IRQ                = 0x%x\n", snd_gf1_i_look8(gus, SNDRV_GF1_GB_EMULATION_IRQ));
	}
}

void snd_gf1_peek_print_block(snd_gus_card_t * gus, unsigned int addr, int count, int w_16bit)
{
	if (!w_16bit) {
		while (count-- > 0)
			printk(count > 0 ? "%02x:" : "%02x", snd_gf1_peek(gus, addr++));
	} else {
		while (count-- > 0) {
			printk(count > 0 ? "%04x:" : "%04x", snd_gf1_peek(gus, addr) | (snd_gf1_peek(gus, addr + 1) << 8));
			addr += 2;
		}
	}
}

#endif  /*  0  */

#endif
