/* DVB USB compliant linux driver for MSI Mega Sky 580 DVB-T USB2.0 receiver
 *
 * Copyright (C) 2006 Aapo Tahkola (aet@rasterburn.org)
 *
 *	This program is free software; you can redistribute it and/or modify it
 *	under the terms of the GNU General Public License as published by the Free
 *	Software Foundation, version 2.
 *
 * see Documentation/dvb/README.dvb-usb for more information
 */

#include "m920x.h"

#include "mt352.h"
#include "mt352_priv.h"
#include "qt1010.h"
#include "tda1004x.h"
#include "tda827x.h"

/* debug */
static int dvb_usb_m920x_debug;
module_param_named(debug,dvb_usb_m920x_debug, int, 0644);
MODULE_PARM_DESC(debug, "set debugging level (1=rc (or-able))." DVB_USB_DEBUG_STATUS);

static struct dvb_usb_rc_key megasky_rc_keys [] = {
	{ 0x0, 0x12, KEY_POWER },
	{ 0x0, 0x1e, KEY_CYCLEWINDOWS }, /* min/max */
	{ 0x0, 0x02, KEY_CHANNELUP },
	{ 0x0, 0x05, KEY_CHANNELDOWN },
	{ 0x0, 0x03, KEY_VOLUMEUP },
	{ 0x0, 0x06, KEY_VOLUMEDOWN },
	{ 0x0, 0x04, KEY_MUTE },
	{ 0x0, 0x07, KEY_OK }, /* TS */
	{ 0x0, 0x08, KEY_STOP },
	{ 0x0, 0x09, KEY_MENU }, /* swap */
	{ 0x0, 0x0a, KEY_REWIND },
	{ 0x0, 0x1b, KEY_PAUSE },
	{ 0x0, 0x1f, KEY_FASTFORWARD },
	{ 0x0, 0x0c, KEY_RECORD },
	{ 0x0, 0x0d, KEY_CAMERA }, /* screenshot */
	{ 0x0, 0x0e, KEY_COFFEE }, /* "MTS" */
};

static struct dvb_usb_rc_key tvwalkertwin_rc_keys [] = {
	{ 0x0, 0x01, KEY_ZOOM }, /* Full Screen */
	{ 0x0, 0x02, KEY_CAMERA }, /* snapshot */
	{ 0x0, 0x03, KEY_MUTE },
	{ 0x0, 0x04, KEY_REWIND },
	{ 0x0, 0x05, KEY_PLAYPAUSE }, /* Play/Pause */
	{ 0x0, 0x06, KEY_FASTFORWARD },
	{ 0x0, 0x07, KEY_RECORD },
	{ 0x0, 0x08, KEY_STOP },
	{ 0x0, 0x09, KEY_TIME }, /* Timeshift */
	{ 0x0, 0x0c, KEY_COFFEE }, /* Recall */
	{ 0x0, 0x0e, KEY_CHANNELUP },
	{ 0x0, 0x12, KEY_POWER },
	{ 0x0, 0x15, KEY_MENU }, /* source */
	{ 0x0, 0x18, KEY_CYCLEWINDOWS }, /* TWIN PIP */
	{ 0x0, 0x1a, KEY_CHANNELDOWN },
	{ 0x0, 0x1b, KEY_VOLUMEDOWN },
	{ 0x0, 0x1e, KEY_VOLUMEUP },
};

static inline int m9206_read(struct usb_device *udev, u8 request, u16 value,\
			     u16 index, void *data, int size)
{
	int ret;

	ret = usb_control_msg(udev, usb_rcvctrlpipe(udev, 0),
			      request, USB_TYPE_VENDOR | USB_DIR_IN,
			      value, index, data, size, 2000);
	if (ret < 0) {
		printk(KERN_INFO "m920x_read = error: %d\n", ret);
		return ret;
	}

	if (ret != size) {
		deb_rc("m920x_read = no data\n");
		return -EIO;
	}

	return 0;
}

static inline int m9206_write(struct usb_device *udev, u8 request,
			      u16 value, u16 index)
{
	int ret;

	ret = usb_control_msg(udev, usb_sndctrlpipe(udev, 0),
			      request, USB_TYPE_VENDOR | USB_DIR_OUT,
			      value, index, NULL, 0, 2000);

	return ret;
}

static int m9206_init(struct dvb_usb_device *d, struct m9206_inits *rc_seq)
{
	int ret = 0;

	/* Remote controller init. */
	if (d->props.rc_query) {
		deb_rc("Initialising remote control\n");
		while (rc_seq->address) {
			if ((ret = m9206_write(d->udev, M9206_CORE, rc_seq->data, rc_seq->address)) != 0) {
				deb_rc("Initialising remote control failed\n");
				return ret;
			}

			rc_seq++;
		}

		deb_rc("Initialising remote control success\n");
	}

	return ret;
}

static int m9206_rc_query(struct dvb_usb_device *d, u32 *event, int *state)
{
	struct m9206_state *m = d->priv;
	int i, ret = 0;
	u8 rc_state[2];

	if ((ret = m9206_read(d->udev, M9206_CORE, 0x0, M9206_RC_STATE, rc_state, 1)) != 0)
		goto unlock;

	if ((ret = m9206_read(d->udev, M9206_CORE, 0x0, M9206_RC_KEY, rc_state + 1, 1)) != 0)
		goto unlock;

	for (i = 0; i < d->props.rc_key_map_size; i++)
		if (d->props.rc_key_map[i].data == rc_state[1]) {
			*event = d->props.rc_key_map[i].event;

			switch(rc_state[0]) {
			case 0x80:
				*state = REMOTE_NO_KEY_PRESSED;
				goto unlock;

			case 0x88: /* framing error or "invalid code" */
			case 0x99:
			case 0xc0:
			case 0xd8:
				*state = REMOTE_NO_KEY_PRESSED;
				m->rep_count = 0;
				goto unlock;

			case 0x93:
			case 0x92:
				m->rep_count = 0;
				*state = REMOTE_KEY_PRESSED;
				goto unlock;

			case 0x91:
				/* prevent immediate auto-repeat */
				if (++m->rep_count > 2)
					*state = REMOTE_KEY_REPEAT;
				else
					*state = REMOTE_NO_KEY_PRESSED;
				goto unlock;

			default:
				deb_rc("Unexpected rc state %02x\n", rc_state[0]);
				*state = REMOTE_NO_KEY_PRESSED;
				goto unlock;
			}
		}

	if (rc_state[1] != 0)
		deb_rc("Unknown rc key %02x\n", rc_state[1]);

	*state = REMOTE_NO_KEY_PRESSED;

	unlock:

	return ret;
}

/* I2C */
static int m9206_i2c_xfer(struct i2c_adapter *adap, struct i2c_msg msg[],
			  int num)
{
	struct dvb_usb_device *d = i2c_get_adapdata(adap);
	int i, j;
	int ret = 0;

	if (!num)
		return -EINVAL;

	if (mutex_lock_interruptible(&d->i2c_mutex) < 0)
		return -EAGAIN;

	for (i = 0; i < num; i++) {
		if (msg[i].flags & (I2C_M_NO_RD_ACK|I2C_M_IGNORE_NAK|I2C_M_TEN) ||
		    msg[i].len == 0) {
			/* For a 0 byte message, I think sending the address to index 0x80|0x40
			 * would be the correct thing to do.  However, zero byte messages are
			 * only used for probing, and since we don't know how to get the slave's
			 * ack, we can't probe. */
			ret = -ENOTSUPP;
			goto unlock;
		}
		/* Send START & address/RW bit */
		if (!(msg[i].flags & I2C_M_NOSTART)) {
			if ((ret = m9206_write(d->udev, M9206_I2C, (msg[i].addr<<1)|(msg[i].flags&I2C_M_RD?0x01:0), 0x80)) != 0)
				goto unlock;
			/* Should check for ack here, if we knew how. */
		}
		if (msg[i].flags & I2C_M_RD) {
			for (j = 0; j < msg[i].len; j++) {
				/* Last byte of transaction? Send STOP, otherwise send ACK. */
				int stop = (i+1 == num && j+1 == msg[i].len)?0x40:0x01;
				if ((ret = m9206_read(d->udev, M9206_I2C, 0x0, 0x20|stop, &msg[i].buf[j], 1)) != 0)
					goto unlock;
			}
		} else {
			for (j = 0; j < msg[i].len; j++) {
				/* Last byte of transaction? Then send STOP. */
				int stop = (i+1 == num && j+1 == msg[i].len)?0x40:0x00;
				if ((ret = m9206_write(d->udev, M9206_I2C, msg[i].buf[j], stop)) != 0)
					goto unlock;
				/* Should check for ack here too. */
			}
		}
	}
	ret = num;

unlock:
	mutex_unlock(&d->i2c_mutex);

	return ret;
}

static u32 m9206_i2c_func(struct i2c_adapter *adapter)
{
	return I2C_FUNC_I2C;
}

static struct i2c_algorithm m9206_i2c_algo = {
	.master_xfer   = m9206_i2c_xfer,
	.functionality = m9206_i2c_func,
};


static int m9206_set_filter(struct dvb_usb_adapter *adap, int type, int idx,
			    int pid)
{
	int ret = 0;

	if (pid >= 0x8000)
		return -EINVAL;

	pid |= 0x8000;

	if ((ret = m9206_write(adap->dev->udev, M9206_FILTER, pid, (type << 8) | (idx * 4) )) != 0)
		return ret;

	if ((ret = m9206_write(adap->dev->udev, M9206_FILTER, 0, (type << 8) | (idx * 4) )) != 0)
		return ret;

	return ret;
}

static int m9206_update_filters(struct dvb_usb_adapter *adap)
{
	struct m9206_state *m = adap->dev->priv;
	int enabled = m->filtering_enabled;
	int i, ret = 0, filter = 0;

	for (i = 0; i < M9206_MAX_FILTERS; i++)
		if (m->filters[i] == 8192)
			enabled = 0;

	/* Disable all filters */
	if ((ret = m9206_set_filter(adap, 0x81, 1, enabled)) != 0)
		return ret;

	for (i = 0; i < M9206_MAX_FILTERS; i++)
		if ((ret = m9206_set_filter(adap, 0x81, i + 2, 0)) != 0)
			return ret;

	if ((ret = m9206_set_filter(adap, 0x82, 0, 0x0)) != 0)
		return ret;

	/* Set */
	if (enabled) {
		for (i = 0; i < M9206_MAX_FILTERS; i++) {
			if (m->filters[i] == 0)
				continue;

			if ((ret = m9206_set_filter(adap, 0x81, filter + 2, m->filters[i])) != 0)
				return ret;

			filter++;
		}
	}

	if ((ret = m9206_set_filter(adap, 0x82, 0, 0x02f5)) != 0)
		return ret;

	return ret;
}

static int m9206_pid_filter_ctrl(struct dvb_usb_adapter *adap, int onoff)
{
	struct m9206_state *m = adap->dev->priv;

	m->filtering_enabled = onoff ? 1 : 0;

	return m9206_update_filters(adap);
}

static int m9206_pid_filter(struct dvb_usb_adapter *adap, int index, u16 pid,
			    int onoff)
{
	struct m9206_state *m = adap->dev->priv;

	m->filters[index] = onoff ? pid : 0;

	return m9206_update_filters(adap);
}

static int m9206_firmware_download(struct usb_device *udev,
				   const struct firmware *fw)
{
	u16 value, index, size;
	u8 read[4], *buff;
	int i, pass, ret = 0;

	buff = kmalloc(65536, GFP_KERNEL);

	if ((ret = m9206_read(udev, M9206_FILTER, 0x0, 0x8000, read, 4)) != 0)
		goto done;
	deb_rc("%x %x %x %x\n", read[0], read[1], read[2], read[3]);

	if ((ret = m9206_read(udev, M9206_FW, 0x0, 0x0, read, 1)) != 0)
		goto done;
	deb_rc("%x\n", read[0]);

	for (pass = 0; pass < 2; pass++) {
		for (i = 0; i + (sizeof(u16) * 3) < fw->size;) {
			value = le16_to_cpu(*(u16 *)(fw->data + i));
			i += sizeof(u16);

			index = le16_to_cpu(*(u16 *)(fw->data + i));
			i += sizeof(u16);

			size = le16_to_cpu(*(u16 *)(fw->data + i));
			i += sizeof(u16);

			if (pass == 1) {
				/* Will stall if using fw->data ... */
				memcpy(buff, fw->data + i, size);

				ret = usb_control_msg(udev, usb_sndctrlpipe(udev,0),
					    M9206_FW,
					    USB_TYPE_VENDOR | USB_DIR_OUT,
					    value, index, buff, size, 20);
				if (ret != size) {
					deb_rc("error while uploading fw!\n");
					ret = -EIO;
					goto done;
				}
				msleep(3);
			}
			i += size;
		}
		if (i != fw->size) {
			deb_rc("bad firmware file!\n");
			ret = -EINVAL;
			goto done;
		}
	}

	msleep(36);

	/* m9206 will disconnect itself from the bus after this. */
	(void) m9206_write(udev, M9206_CORE, 0x01, M9206_FW_GO);
	deb_rc("firmware uploaded!\n");

	done:
	kfree(buff);

	return ret;
}

/* Callbacks for DVB USB */
static int m920x_identify_state(struct usb_device *udev,
				struct dvb_usb_device_properties *props,
				struct dvb_usb_device_description **desc,
				int *cold)
{
	struct usb_host_interface *alt;

	alt = usb_altnum_to_altsetting(usb_ifnum_to_if(udev, 0), 1);
	*cold = (alt == NULL) ? 1 : 0;

	return 0;
}

static int megasky_mt352_demod_init(struct dvb_frontend *fe)
{
	u8 config[] = { CONFIG, 0x3d };
	u8 clock[] = { CLOCK_CTL, 0x30 };
	u8 reset[] = { RESET, 0x80 };
	u8 adc_ctl[] = { ADC_CTL_1, 0x40 };
	u8 agc[] = { AGC_TARGET, 0x1c, 0x20 };
	u8 sec_agc[] = { 0x69, 0x00, 0xff, 0xff, 0x40, 0xff, 0x00, 0x40, 0x40 };
	u8 unk1[] = { 0x93, 0x1a };
	u8 unk2[] = { 0xb5, 0x7a };

	mt352_write(fe, config, ARRAY_SIZE(config));
	mt352_write(fe, clock, ARRAY_SIZE(clock));
	mt352_write(fe, reset, ARRAY_SIZE(reset));
	mt352_write(fe, adc_ctl, ARRAY_SIZE(adc_ctl));
	mt352_write(fe, agc, ARRAY_SIZE(agc));
	mt352_write(fe, sec_agc, ARRAY_SIZE(sec_agc));
	mt352_write(fe, unk1, ARRAY_SIZE(unk1));
	mt352_write(fe, unk2, ARRAY_SIZE(unk2));

	deb_rc("Demod init!\n");

	return 0;
}

static struct mt352_config megasky_mt352_config = {
	.demod_address = 0x0f,
	.no_tuner = 1,
	.demod_init = megasky_mt352_demod_init,
};

static int megasky_mt352_frontend_attach(struct dvb_usb_adapter *adap)
{
	deb_rc("megasky_frontend_attach!\n");

	if ((adap->fe = dvb_attach(mt352_attach, &megasky_mt352_config, &adap->dev->i2c_adap)) == NULL)
		return -EIO;

	return 0;
}

static struct qt1010_config megasky_qt1010_config = {
	.i2c_address = 0x62
};

static int megasky_qt1010_tuner_attach(struct dvb_usb_adapter *adap)
{
	if (dvb_attach(qt1010_attach, adap->fe, &adap->dev->i2c_adap,
		       &megasky_qt1010_config) == NULL)
		return -ENODEV;

	return 0;
}

static struct m9206_inits megasky_rc_init [] = {
	{ M9206_RC_INIT2, 0xa8 },
	{ M9206_RC_INIT1, 0x51 },
	{ } /* terminating entry */
};

static struct tda1004x_config digivox_tda10046_config = {
	.demod_address = 0x08,
	.invert = 0,
	.invert_oclk = 0,
	.ts_mode = TDA10046_TS_SERIAL,
	.xtal_freq = TDA10046_XTAL_16M,
	.if_freq = TDA10046_FREQ_045,
	.agc_config = TDA10046_AGC_TDA827X,
	.gpio_config = TDA10046_GPTRI,
	.request_firmware = NULL,
};

static int digivox_tda10046_frontend_attach(struct dvb_usb_adapter *adap)
{
	deb_rc("digivox_tda10046_frontend_attach!\n");

	if ((adap->fe = dvb_attach(tda10046_attach, &digivox_tda10046_config,
				   &adap->dev->i2c_adap)) == NULL)
		return -EIO;

	return 0;
}

static int digivox_tda8275_tuner_attach(struct dvb_usb_adapter *adap)
{
	if (dvb_attach(tda827x_attach, adap->fe, 0x60, &adap->dev->i2c_adap,
		       NULL) == NULL)
		return -ENODEV;
	return 0;
}

/* LifeView TV Walker Twin has 1 x M9206, 2 x TDA10046, 2 x TDA8275A
 * TDA10046 #0 is located at i2c address 0x08
 * TDA10046 #1 is located at i2c address 0x0b
 * TDA8275A #0 is located at i2c address 0x60
 * TDA8275A #1 is located at i2c address 0x61
 */

static struct tda1004x_config tvwalkertwin_0_tda10046_config = {
	.demod_address = 0x08,
	.invert = 0,
	.invert_oclk = 0,
	.ts_mode = TDA10046_TS_SERIAL,
	.xtal_freq = TDA10046_XTAL_16M,
	.if_freq = TDA10046_FREQ_045,
	.agc_config = TDA10046_AGC_TDA827X,
	.gpio_config = TDA10046_GPTRI,
	.request_firmware = NULL, /* uses firmware EEPROM */
};

static struct tda1004x_config tvwalkertwin_1_tda10046_config = {
	.demod_address = 0x0b,
	.invert = 0,
	.invert_oclk = 0,
	.ts_mode = TDA10046_TS_SERIAL,
	.xtal_freq = TDA10046_XTAL_16M,
	.if_freq = TDA10046_FREQ_045,
	.agc_config = TDA10046_AGC_TDA827X,
	.gpio_config = TDA10046_GPTRI,
	.request_firmware = NULL, /* uses firmware EEPROM */
};

static int tvwalkertwin_0_tda10046_frontend_attach(struct dvb_usb_adapter *adap)
{
	deb_rc("tvwalkertwin_0_tda10046_frontend_attach!\n");

	if ((adap->fe = dvb_attach(tda10046_attach, &tvwalkertwin_0_tda10046_config, &adap->dev->i2c_adap)) == NULL)
		return -EIO;

	deb_rc("Attached demod 0 at address %02x\n", tvwalkertwin_0_tda10046_config.demod_address);

	return 0;
}

static int tvwalkertwin_1_tda10046_frontend_attach(struct dvb_usb_adapter *adap)
{
	deb_rc("tvwalkertwin_1_tda10046_frontend_attach!\n");

	if ((adap->fe = dvb_attach(tda10046_attach, &tvwalkertwin_1_tda10046_config, &adap->dev->i2c_adap)) == NULL)
		return -EIO;

	deb_rc("Attached demod 1 at address %02x\n", tvwalkertwin_1_tda10046_config.demod_address);

	return 0;
}

static int tvwalkertwin_0_tda8275_tuner_attach(struct dvb_usb_adapter *adap)
{
	int address = 0x60;

	deb_rc("tvwalkertwin_0_tda8275_tuner_attach!\n");

	if (dvb_attach(tda827x_attach, adap->fe, address, &adap->dev->i2c_adap,
		       NULL) == NULL)
		return -ENODEV;

	deb_rc("Attached tuner 0 at address %02x\n", address);

	return 0;
}

static int tvwalkertwin_1_tda8275_tuner_attach(struct dvb_usb_adapter *adap)
{
	int address = 0x61;

	deb_rc("tvwalkertwin_1_tda8275_tuner_attach!\n");

	if (dvb_attach(tda827x_attach, adap->fe, address, &adap->dev->i2c_adap,
		       NULL) == NULL)
		return -ENODEV;

	deb_rc("Attached tuner 1 at address %02x\n", address);

	return 0;
}

static struct m9206_inits tvwalkertwin_rc_init [] = {
	{ M9206_RC_INIT2, 0x00 },
	{ M9206_RC_INIT1, 0xef },
	{ 0xff28,         0x00 },
	{ 0xff23,         0x00 },
	{ 0xff21,         0x30 },
	{ } /* terminating entry */
};

/* DVB USB Driver stuff */
static struct dvb_usb_device_properties megasky_properties;
static struct dvb_usb_device_properties digivox_mini_ii_properties;
static struct dvb_usb_device_properties tvwalkertwin_properties;
static struct m9206_inits megasky_rc_init [];
static struct m9206_inits tvwalkertwin_rc_init [];

static int m920x_probe(struct usb_interface *intf,
		       const struct usb_device_id *id)
{
	struct dvb_usb_device *d;
	struct usb_host_interface *alt;
	int ret;
	struct m9206_inits *rc_init_seq = NULL;
	int bInterfaceNumber = intf->cur_altsetting->desc.bInterfaceNumber;

	deb_rc("Probing for m920x device at interface %d\n", bInterfaceNumber);

	if (bInterfaceNumber == 0) {
		/* Single-tuner device, or first interface on
		 * multi-tuner device
		 */

		if ((ret = dvb_usb_device_init(intf, &megasky_properties,
			THIS_MODULE, &d)) == 0) {
			rc_init_seq = megasky_rc_init;
			goto found;
		}

		if ((ret = dvb_usb_device_init(intf,
			&digivox_mini_ii_properties, THIS_MODULE, &d)) == 0) {
			/* No remote control, so no rc_init_seq */
			goto found;
		}

		/* This configures both tuners on the TV Walker Twin */
		if ((ret = dvb_usb_device_init(intf, &tvwalkertwin_properties,
			THIS_MODULE, &d)) == 0) {
			rc_init_seq = tvwalkertwin_rc_init;
			goto found;
		}

		return ret;
	} else {
		/* Another interface on a multi-tuner device */

		/* The LifeView TV Walker Twin gets here, but struct
		 * tvwalkertwin_properties already configured both
		 * tuners, so there is nothing for us to do here
		 */

		return -ENODEV;
	}

found:
	alt = usb_altnum_to_altsetting(intf, 1);
	if (alt == NULL) {
		deb_rc("No alt found!\n");
		return -ENODEV;
	}

	ret = usb_set_interface(d->udev, alt->desc.bInterfaceNumber,
				alt->desc.bAlternateSetting);
	if (ret < 0)
		return ret;

	if ((ret = m9206_init(d, rc_init_seq)) != 0)
		return ret;

	return ret;
}

static struct usb_device_id m920x_table [] = {
		{ USB_DEVICE(USB_VID_MSI, USB_PID_MSI_MEGASKY580) },
		{ USB_DEVICE(USB_VID_ANUBIS_ELECTRONIC,
			     USB_PID_MSI_DIGI_VOX_MINI_II) },
		{ USB_DEVICE(USB_VID_ANUBIS_ELECTRONIC,
			     USB_PID_LIFEVIEW_TV_WALKER_TWIN_COLD) },
		{ USB_DEVICE(USB_VID_ANUBIS_ELECTRONIC,
			     USB_PID_LIFEVIEW_TV_WALKER_TWIN_WARM) },
		{ }		/* Terminating entry */
};
MODULE_DEVICE_TABLE (usb, m920x_table);

static struct dvb_usb_device_properties megasky_properties = {
	.caps = DVB_USB_IS_AN_I2C_ADAPTER,

	.usb_ctrl = DEVICE_SPECIFIC,
	.firmware = "dvb-usb-megasky-02.fw",
	.download_firmware = m9206_firmware_download,

	.rc_interval      = 100,
	.rc_key_map       = megasky_rc_keys,
	.rc_key_map_size  = ARRAY_SIZE(megasky_rc_keys),
	.rc_query         = m9206_rc_query,

	.size_of_priv     = sizeof(struct m9206_state),

	.identify_state   = m920x_identify_state,
	.num_adapters = 1,
	.adapter = {{
		.caps = DVB_USB_ADAP_HAS_PID_FILTER |
			DVB_USB_ADAP_PID_FILTER_CAN_BE_TURNED_OFF,

		.pid_filter_count = 8,
		.pid_filter       = m9206_pid_filter,
		.pid_filter_ctrl  = m9206_pid_filter_ctrl,

		.frontend_attach  = megasky_mt352_frontend_attach,
		.tuner_attach     = megasky_qt1010_tuner_attach,

		.stream = {
			.type = USB_BULK,
			.count = 8,
			.endpoint = 0x81,
			.u = {
				.bulk = {
					.buffersize = 512,
				}
			}
		},
	}},
	.i2c_algo         = &m9206_i2c_algo,

	.num_device_descs = 1,
	.devices = {
		{   "MSI Mega Sky 580 DVB-T USB2.0",
			{ &m920x_table[0], NULL },
			{ NULL },
		}
	}
};

static struct dvb_usb_device_properties digivox_mini_ii_properties = {
	.caps = DVB_USB_IS_AN_I2C_ADAPTER,

	.usb_ctrl = DEVICE_SPECIFIC,
	.firmware = "dvb-usb-digivox-02.fw",
	.download_firmware = m9206_firmware_download,

	.size_of_priv     = sizeof(struct m9206_state),

	.identify_state   = m920x_identify_state,
	.num_adapters = 1,
	.adapter = {{
		.caps = DVB_USB_ADAP_HAS_PID_FILTER |
		DVB_USB_ADAP_PID_FILTER_CAN_BE_TURNED_OFF,

		.pid_filter_count = 8,
		.pid_filter       = m9206_pid_filter,
		.pid_filter_ctrl  = m9206_pid_filter_ctrl,

		.frontend_attach  = digivox_tda10046_frontend_attach,
		.tuner_attach     = digivox_tda8275_tuner_attach,

		.stream = {
			.type = USB_BULK,
			.count = 8,
			.endpoint = 0x81,
			.u = {
				.bulk = {
					.buffersize = 0x4000,
				}
			}
		},
	}},
	.i2c_algo         = &m9206_i2c_algo,

	.num_device_descs = 1,
	.devices = {
		{   "MSI DIGI VOX mini II DVB-T USB2.0",
			{ &m920x_table[1], NULL },
			{ NULL },
		},
	}
};

/* LifeView TV Walker Twin support by Nick Andrew <nick@nick-andrew.net> */

static struct dvb_usb_device_properties tvwalkertwin_properties = {
	.caps = DVB_USB_IS_AN_I2C_ADAPTER,

	.usb_ctrl = DEVICE_SPECIFIC,
	.firmware = "dvb-usb-tvwalkert.fw",
	.download_firmware = m9206_firmware_download,

	.rc_interval      = 100,
	.rc_key_map       = tvwalkertwin_rc_keys,
	.rc_key_map_size  = ARRAY_SIZE(tvwalkertwin_rc_keys),
	.rc_query         = m9206_rc_query,

	.size_of_priv     = sizeof(struct m9206_state),

	.identify_state   = m920x_identify_state,
	.num_adapters = 2,
	.adapter = {
		{
			.caps = DVB_USB_ADAP_HAS_PID_FILTER |
				DVB_USB_ADAP_PID_FILTER_CAN_BE_TURNED_OFF,

			.pid_filter_count = 8,
			.pid_filter       = m9206_pid_filter,
			.pid_filter_ctrl  = m9206_pid_filter_ctrl,

			.frontend_attach  = tvwalkertwin_0_tda10046_frontend_attach,
			.tuner_attach     = tvwalkertwin_0_tda8275_tuner_attach,

			.stream = {
				.type = USB_BULK,
				.count = 8,
				.endpoint = 0x81,
				.u = {
					.bulk = {
						.buffersize = 512,
					}
				}
			},
		},
		{
			.caps = DVB_USB_ADAP_HAS_PID_FILTER |
				DVB_USB_ADAP_PID_FILTER_CAN_BE_TURNED_OFF,

			.pid_filter_count = 8,
			.pid_filter       = m9206_pid_filter,
			.pid_filter_ctrl  = m9206_pid_filter_ctrl,

			.frontend_attach  = tvwalkertwin_1_tda10046_frontend_attach,
			.tuner_attach     = tvwalkertwin_1_tda8275_tuner_attach,

			.stream = {
				.type = USB_BULK,
				.count = 8,
				.endpoint = 0x82,
				.u = {
					.bulk = {
						.buffersize = 512,
					}
				}
			},
		}
	},
	.i2c_algo         = &m9206_i2c_algo,

	.num_device_descs = 1,
	.devices = {
		{   .name = "LifeView TV Walker Twin DVB-T USB2.0",
		    .cold_ids = { &m920x_table[2], NULL },
		    .warm_ids = { &m920x_table[3], NULL },
		},
	}
};

static struct usb_driver m920x_driver = {
	.name		= "dvb_usb_m920x",
	.probe		= m920x_probe,
	.disconnect	= dvb_usb_device_exit,
	.id_table	= m920x_table,
};

/* module stuff */
static int __init m920x_module_init(void)
{
	int ret;

	if ((ret = usb_register(&m920x_driver))) {
		err("usb_register failed. Error number %d", ret);
		return ret;
	}

	return 0;
}

static void __exit m920x_module_exit(void)
{
	/* deregister this driver from the USB subsystem */
	usb_deregister(&m920x_driver);
}

module_init (m920x_module_init);
module_exit (m920x_module_exit);

MODULE_AUTHOR("Aapo Tahkola <aet@rasterburn.org>");
MODULE_DESCRIPTION("DVB Driver for ULI M920x");
MODULE_VERSION("0.1");
MODULE_LICENSE("GPL");
