/*
    Conexant 22702 DVB OFDM demodulator driver

    based on:
	Alps TDMB7 DVB OFDM demodulator driver

    Copyright (C) 2001-2002 Convergence Integrated Media GmbH
	  Holger Waechtler <holger@convergence.de>

    Copyright (C) 2004 Steven Toth <stoth@hauppauge.com>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

*/

#ifndef CX22702_H
#define CX22702_H

#include <linux/dvb/frontend.h>

struct cx22702_config
{
	/* the demodulator's i2c address */
	u8 demod_address;

	/* serial/parallel output */
#define CX22702_PARALLEL_OUTPUT 0
#define CX22702_SERIAL_OUTPUT   1
	u8 output_mode;

	/* PLL maintenance */
	u8 pll_address;
	struct dvb_pll_desc *pll_desc;

	int (*pll_init)(struct dvb_frontend* fe);
	int (*pll_set)(struct dvb_frontend* fe, struct dvb_frontend_parameters* params);
};

extern struct dvb_frontend* cx22702_attach(const struct cx22702_config* config,
					   struct i2c_adapter* i2c);

#endif // CX22702_H
