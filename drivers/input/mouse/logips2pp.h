/*
 * Logitech PS/2++ mouse driver header
 *
 * Copyright (c) 2003 Vojtech Pavlik <vojtech@suse.cz>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 */

#ifndef _LOGIPS2PP_H
#define _LOGIPS2PP_H

int ps2pp_init(struct psmouse *psmouse, int set_properties);

#endif
