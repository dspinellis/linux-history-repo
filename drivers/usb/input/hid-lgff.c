/*
 * Force feedback support for hid-compliant for some of the devices from
 * Logitech, namely:
 * - WingMan Cordless RumblePad
 * - WingMan Force 3D
 *
 *  Copyright (c) 2002-2004 Johann Deneux
 *  Copyright (c) 2006 Anssi Hannula <anssi.hannula@gmail.com>
 */

/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 * Should you need to contact me, the author, you can do so by
 * e-mail - mail your message to <johann.deneux@it.uu.se>
 */

#include <linux/input.h>
#include <linux/usb.h>
#include "hid.h"

struct device_type {
	u16 idVendor;
	u16 idProduct;
	const signed short *ff;
};

static const signed short ff_rumble[] = {
	FF_RUMBLE,
	-1
};

static const signed short ff_joystick[] = {
	FF_CONSTANT,
	-1
};

static const struct device_type devices[] = {
	{ 0x046d, 0xc211, ff_rumble },
	{ 0x046d, 0xc219, ff_rumble },
	{ 0x046d, 0xc283, ff_joystick },
	{ 0x0000, 0x0000, ff_joystick }
};

static int hid_lgff_play(struct input_dev *dev, void *data, struct ff_effect *effect)
{
	struct hid_device *hid = dev->private;
	struct list_head *report_list = &hid->report_enum[HID_OUTPUT_REPORT].report_list;
	struct hid_report *report = list_entry(report_list->next, struct hid_report, list);
	int x, y;
	unsigned int left, right;

#define CLAMP(x) if (x < 0) x = 0; if (x > 0xff) x = 0xff

	switch (effect->type) {
	case FF_CONSTANT:
		x = effect->u.ramp.start_level + 0x7f;	/* 0x7f is center */
		y = effect->u.ramp.end_level + 0x7f;
		CLAMP(x);
		CLAMP(y);
		report->field[0]->value[0] = 0x51;
		report->field[0]->value[1] = 0x08;
		report->field[0]->value[2] = x;
		report->field[0]->value[3] = y;
		dbg("(x, y)=(%04x, %04x)", x, y);
		hid_submit_report(hid, report, USB_DIR_OUT);
		break;

	case FF_RUMBLE:
		right = effect->u.rumble.strong_magnitude;
		left = effect->u.rumble.weak_magnitude;
		right = right * 0xff / 0xffff;
		left = left * 0xff / 0xffff;
		CLAMP(left);
		CLAMP(right);
		report->field[0]->value[0] = 0x42;
		report->field[0]->value[1] = 0x00;
		report->field[0]->value[2] = left;
		report->field[0]->value[3] = right;
		dbg("(left, right)=(%04x, %04x)", left, right);
		hid_submit_report(hid, report, USB_DIR_OUT);
		break;
	}
	return 0;
}

int hid_lgff_init(struct hid_device* hid)
{
	struct hid_input *hidinput = list_entry(hid->inputs.next, struct hid_input, list);
	struct list_head *report_list = &hid->report_enum[HID_OUTPUT_REPORT].report_list;
	struct input_dev *dev = hidinput->input;
	struct hid_report *report;
	struct hid_field *field;
	int error;
	int i, j;

	/* Find the report to use */
	if (list_empty(report_list)) {
		err("No output report found");
		return -1;
	}

	/* Check that the report looks ok */
	report = list_entry(report_list->next, struct hid_report, list);
	if (!report) {
		err("NULL output report");
		return -1;
	}

	field = report->field[0];
	if (!field) {
		err("NULL field");
		return -1;
	}

	for (i = 0; i < ARRAY_SIZE(devices); i++) {
		if (dev->id.vendor == devices[i].idVendor &&
		    dev->id.product == devices[i].idProduct) {
			for (j = 0; devices[i].ff[j] >= 0; j++)
				set_bit(devices[i].ff[j], dev->ffbit);
			break;
		}
	}

	error = input_ff_create_memless(dev, NULL, hid_lgff_play);
	if (error)
		return error;

	printk(KERN_INFO "Force feedback for Logitech force feedback devices by Johann Deneux <johann.deneux@it.uu.se>\n");

	return 0;
}
