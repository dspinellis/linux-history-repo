/*
 *  Atari mouse driver for Linux/m68k
 *
 *  Copyright (c) 2005 Michael Schmitz
 *
 *  Based on:
 *  Amiga mouse driver for Linux/m68k
 *
 *  Copyright (c) 2000-2002 Vojtech Pavlik
 *
 */
/*
 * The low level init and interrupt stuff is handled in arch/mm68k/atari/atakeyb.c
 * (the keyboard ACIA also handles the mouse and joystick data, and the keyboard
 * interrupt is shared with the MIDI ACIA so MIDI data also get handled there).
 * This driver only deals with handing key events off to the input layer.
 *
 * Largely based on the old:
 *
 * Atari Mouse Driver for Linux
 * by Robert de Vries (robert@and.nl) 19Jul93
 *
 * 16 Nov 1994 Andreas Schwab
 * Compatibility with busmouse
 * Support for three button mouse (shamelessly stolen from MiNT)
 * third button wired to one of the joystick directions on joystick 1
 *
 * 1996/02/11 Andreas Schwab
 * Module support
 * Allow multiple open's
 *
 * Converted to use new generic busmouse code.  5 Apr 1998
 *   Russell King <rmk@arm.uk.linux.org>
 */


/*
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/interrupt.h>

#include <asm/irq.h>
#include <asm/setup.h>
#include <asm/system.h>
#include <asm/uaccess.h>
#include <asm/atarihw.h>
#include <asm/atarikb.h>
#include <asm/atariints.h>

MODULE_AUTHOR("Michael Schmitz <schmitz@biophys.uni-duesseldorf.de>");
MODULE_DESCRIPTION("Atari mouse driver");
MODULE_LICENSE("GPL");

static int mouse_threshold[2] = {2,2};

#ifdef __MODULE__
MODULE_PARM(mouse_threshold, "2i");
#endif
#ifdef FIXED_ATARI_JOYSTICK
extern int atari_mouse_buttons;
#endif
static int atamouse_used = 0;

static struct input_dev *atamouse_dev;

static void atamouse_interrupt(char *buf)
{
	int buttons, dx, dy;

/*	ikbd_mouse_disable(); */

	buttons = (buf[0] & 1) | ((buf[0] & 2) << 1);
#ifdef FIXED_ATARI_JOYSTICK
	buttons |= atari_mouse_buttons & 2;
	atari_mouse_buttons = buttons;
#endif
/*	ikbd_mouse_rel_pos(); */

	/* only relative events get here */
	dx =  buf[1];
	dy = -buf[2];

	input_report_rel(atamouse_dev, REL_X, dx);
	input_report_rel(atamouse_dev, REL_Y, dy);

	input_report_key(atamouse_dev, BTN_LEFT,   buttons & 0x1);
	input_report_key(atamouse_dev, BTN_MIDDLE, buttons & 0x2);
	input_report_key(atamouse_dev, BTN_RIGHT,  buttons & 0x4);

	input_sync(atamouse_dev);

	return;
}

static int atamouse_open(struct input_dev *dev)
{
	if (atamouse_used++)
		return 0;

#ifdef FIXED_ATARI_JOYSTICK
	atari_mouse_buttons = 0;
#endif
	ikbd_mouse_y0_top();
	ikbd_mouse_thresh(mouse_threshold[0], mouse_threshold[1]);
	ikbd_mouse_rel_pos();
	atari_input_mouse_interrupt_hook = atamouse_interrupt;
	return 0;
}

static void atamouse_close(struct input_dev *dev)
{
	if (!--atamouse_used) {
		ikbd_mouse_disable();
		atari_mouse_interrupt_hook = NULL;
	}
}

static int __init atamouse_init(void)
{
	if (!MACH_IS_ATARI || !ATARIHW_PRESENT(ST_MFP))
		return -ENODEV;

	if (!(atamouse_dev = input_allocate_device()))
		return -ENOMEM;

	if (!(atari_keyb_init()))
		return -ENODEV;

	atamouse_dev->name = "Atari mouse";
	atamouse_dev->phys = "atamouse/input0";
	atamouse_dev->id.bustype = BUS_ATARI;
	atamouse_dev->id.vendor = 0x0001;
	atamouse_dev->id.product = 0x0002;
	atamouse_dev->id.version = 0x0100;

	atamouse_dev->evbit[0] = BIT(EV_KEY) | BIT(EV_REL);
	atamouse_dev->relbit[0] = BIT(REL_X) | BIT(REL_Y);
	atamouse_dev->keybit[LONG(BTN_LEFT)] = BIT(BTN_LEFT) | BIT(BTN_MIDDLE) | BIT(BTN_RIGHT);
	atamouse_dev->open = atamouse_open;
	atamouse_dev->close = atamouse_close;

	input_register_device(atamouse_dev);

	printk(KERN_INFO "input: %s at keyboard ACIA\n", atamouse_dev->name);
	return 0;
}

static void __exit atamouse_exit(void)
{
	input_unregister_device(atamouse_dev);
}

module_init(atamouse_init);
module_exit(atamouse_exit);
