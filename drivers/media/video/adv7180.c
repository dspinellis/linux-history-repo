/*
 * adv7180.c Analog Devices ADV7180 video decoder driver
 * Copyright (c) 2009 Intel Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/i2c.h>
#include <linux/i2c-id.h>
#include <media/v4l2-ioctl.h>
#include <linux/videodev2.h>
#include <media/v4l2-device.h>
#include <media/v4l2-chip-ident.h>

#define DRIVER_NAME "adv7180"

#define ADV7180_INPUT_CONTROL_REG			0x00
#define ADV7180_INPUT_CONTROL_AD_PAL_BG_NTSC_J_SECAM	0x00
#define ADV7180_INPUT_CONTROL_AD_PAL_BG_NTSC_J_SECAM_PED 0x10
#define ADV7180_INPUT_CONTROL_AD_PAL_N_NTSC_J_SECAM	0x20
#define ADV7180_INPUT_CONTROL_AD_PAL_N_NTSC_M_SECAM	0x30
#define ADV7180_INPUT_CONTROL_NTSC_J			0x40
#define ADV7180_INPUT_CONTROL_NTSC_M			0x50
#define ADV7180_INPUT_CONTROL_PAL60			0x60
#define ADV7180_INPUT_CONTROL_NTSC_443			0x70
#define ADV7180_INPUT_CONTROL_PAL_BG			0x80
#define ADV7180_INPUT_CONTROL_PAL_N			0x90
#define ADV7180_INPUT_CONTROL_PAL_M			0xa0
#define ADV7180_INPUT_CONTROL_PAL_M_PED			0xb0
#define ADV7180_INPUT_CONTROL_PAL_COMB_N		0xc0
#define ADV7180_INPUT_CONTROL_PAL_COMB_N_PED		0xd0
#define ADV7180_INPUT_CONTROL_PAL_SECAM			0xe0
#define ADV7180_INPUT_CONTROL_PAL_SECAM_PED		0xf0

#define ADV7180_AUTODETECT_ENABLE_REG	0x07
#define ADV7180_AUTODETECT_DEFAULT	0x7f


#define ADV7180_STATUS1_REG				0x10
#define ADV7180_STATUS1_IN_LOCK		0x01
#define ADV7180_STATUS1_AUTOD_MASK	0x70
#define ADV7180_STATUS1_AUTOD_NTSM_M_J	0x00
#define ADV7180_STATUS1_AUTOD_NTSC_4_43 0x10
#define ADV7180_STATUS1_AUTOD_PAL_M	0x20
#define ADV7180_STATUS1_AUTOD_PAL_60	0x30
#define ADV7180_STATUS1_AUTOD_PAL_B_G	0x40
#define ADV7180_STATUS1_AUTOD_SECAM	0x50
#define ADV7180_STATUS1_AUTOD_PAL_COMB	0x60
#define ADV7180_STATUS1_AUTOD_SECAM_525	0x70

#define ADV7180_IDENT_REG 0x11
#define ADV7180_ID_7180 0x18


struct adv7180_state {
	struct v4l2_subdev	sd;
	v4l2_std_id		curr_norm;
	bool			autodetect;
};

static v4l2_std_id adv7180_std_to_v4l2(u8 status1)
{
	switch (status1 & ADV7180_STATUS1_AUTOD_MASK) {
	case ADV7180_STATUS1_AUTOD_NTSM_M_J:
		return V4L2_STD_NTSC;
	case ADV7180_STATUS1_AUTOD_NTSC_4_43:
		return V4L2_STD_NTSC_443;
	case ADV7180_STATUS1_AUTOD_PAL_M:
		return V4L2_STD_PAL_M;
	case ADV7180_STATUS1_AUTOD_PAL_60:
		return V4L2_STD_PAL_60;
	case ADV7180_STATUS1_AUTOD_PAL_B_G:
		return V4L2_STD_PAL;
	case ADV7180_STATUS1_AUTOD_SECAM:
		return V4L2_STD_SECAM;
	case ADV7180_STATUS1_AUTOD_PAL_COMB:
		return V4L2_STD_PAL_Nc | V4L2_STD_PAL_N;
	case ADV7180_STATUS1_AUTOD_SECAM_525:
		return V4L2_STD_SECAM;
	default:
		return V4L2_STD_UNKNOWN;
	}
}

static int v4l2_std_to_adv7180(v4l2_std_id std)
{
	if (std == V4L2_STD_PAL_60)
		return ADV7180_INPUT_CONTROL_PAL60;
	if (std == V4L2_STD_NTSC_443)
		return ADV7180_INPUT_CONTROL_NTSC_443;
	if (std == V4L2_STD_PAL_N)
		return ADV7180_INPUT_CONTROL_PAL_N;
	if (std == V4L2_STD_PAL_M)
		return ADV7180_INPUT_CONTROL_PAL_M;
	if (std == V4L2_STD_PAL_Nc)
		return ADV7180_INPUT_CONTROL_PAL_COMB_N;

	if (std & V4L2_STD_PAL)
		return ADV7180_INPUT_CONTROL_PAL_BG;
	if (std & V4L2_STD_NTSC)
		return ADV7180_INPUT_CONTROL_NTSC_M;
	if (std & V4L2_STD_SECAM)
		return ADV7180_INPUT_CONTROL_PAL_SECAM;

	return -EINVAL;
}

static u32 adv7180_status_to_v4l2(u8 status1)
{
	if (!(status1 & ADV7180_STATUS1_IN_LOCK))
		return V4L2_IN_ST_NO_SIGNAL;

	return 0;
}

static int __adv7180_status(struct i2c_client *client, u32 *status,
	v4l2_std_id *std)
{
	int status1 = i2c_smbus_read_byte_data(client, ADV7180_STATUS1_REG);

	if (status1 < 0)
		return status1;

	if (status)
		*status = adv7180_status_to_v4l2(status1);
	if (std)
		*std = adv7180_std_to_v4l2(status1);

	return 0;
}

static inline struct adv7180_state *to_state(struct v4l2_subdev *sd)
{
	return container_of(sd, struct adv7180_state, sd);
}

static int adv7180_querystd(struct v4l2_subdev *sd, v4l2_std_id *std)
{
	struct adv7180_state *state = to_state(sd);
	int err = 0;

	if (!state->autodetect)
		*std = state->curr_norm;
	else
		err = __adv7180_status(v4l2_get_subdevdata(sd), NULL, std);

	return err;
}

static int adv7180_g_input_status(struct v4l2_subdev *sd, u32 *status)
{
	return __adv7180_status(v4l2_get_subdevdata(sd), status, NULL);
}

static int adv7180_g_chip_ident(struct v4l2_subdev *sd,
	struct v4l2_dbg_chip_ident *chip)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);

	return v4l2_chip_ident_i2c_client(client, chip, V4L2_IDENT_ADV7180, 0);
}

static int adv7180_s_std(struct v4l2_subdev *sd, v4l2_std_id std)
{
	struct adv7180_state *state = to_state(sd);
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	int ret;

	/* all standards -> autodetect */
	if (std == V4L2_STD_ALL) {
		ret = i2c_smbus_write_byte_data(client,
			ADV7180_INPUT_CONTROL_REG,
			ADV7180_INPUT_CONTROL_AD_PAL_BG_NTSC_J_SECAM);
		if (ret < 0)
			goto out;

		state->autodetect = true;
	} else {
		ret = v4l2_std_to_adv7180(std);
		if (ret < 0)
			goto out;

		ret = i2c_smbus_write_byte_data(client,
			ADV7180_INPUT_CONTROL_REG, ret);
		if (ret < 0)
			goto out;

		state->curr_norm = std;
		state->autodetect = false;
	}
	ret = 0;
out:
	return ret;
}

static const struct v4l2_subdev_video_ops adv7180_video_ops = {
	.querystd = adv7180_querystd,
	.g_input_status = adv7180_g_input_status,
};

static const struct v4l2_subdev_core_ops adv7180_core_ops = {
	.g_chip_ident = adv7180_g_chip_ident,
	.s_std = adv7180_s_std,
};

static const struct v4l2_subdev_ops adv7180_ops = {
	.core = &adv7180_core_ops,
	.video = &adv7180_video_ops,
};

/*
 * Generic i2c probe
 * concerning the addresses: i2c wants 7 bit (without the r/w bit), so '>>1'
 */

static __devinit int adv7180_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct adv7180_state *state;
	struct v4l2_subdev *sd;
	int ret;

	/* Check if the adapter supports the needed features */
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_SMBUS_BYTE_DATA))
		return -EIO;

	v4l_info(client, "chip found @ 0x%02x (%s)\n",
			client->addr << 1, client->adapter->name);

	state = kzalloc(sizeof(struct adv7180_state), GFP_KERNEL);
	if (state == NULL)
		return -ENOMEM;
	state->autodetect = true;
	sd = &state->sd;
	v4l2_i2c_subdev_init(sd, client, &adv7180_ops);

	/* Initialize adv7180 */
	/* enable autodetection */
	ret = i2c_smbus_write_byte_data(client, ADV7180_INPUT_CONTROL_REG,
		ADV7180_INPUT_CONTROL_AD_PAL_BG_NTSC_J_SECAM);
	if (ret > 0)
		ret = i2c_smbus_write_byte_data(client,
			ADV7180_AUTODETECT_ENABLE_REG,
			ADV7180_AUTODETECT_DEFAULT);
	if (ret < 0) {
		printk(KERN_ERR DRIVER_NAME
			": Failed to communicate to chip: %d\n", ret);
		return ret;
	}

	return 0;
}

static __devexit int adv7180_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);

	v4l2_device_unregister_subdev(sd);
	kfree(to_state(sd));
	return 0;
}

static const struct i2c_device_id adv7180_id[] = {
	{DRIVER_NAME, 0},
	{},
};

MODULE_DEVICE_TABLE(i2c, adv7180_id);

static struct i2c_driver adv7180_driver = {
	.driver = {
		.owner	= THIS_MODULE,
		.name	= DRIVER_NAME,
	},
	.probe		= adv7180_probe,
	.remove		= __devexit_p(adv7180_remove),
	.id_table	= adv7180_id,
};

static __init int adv7180_init(void)
{
	return i2c_add_driver(&adv7180_driver);
}

static __exit void adv7180_exit(void)
{
	i2c_del_driver(&adv7180_driver);
}

module_init(adv7180_init);
module_exit(adv7180_exit);

MODULE_DESCRIPTION("Analog Devices ADV7180 video decoder driver");
MODULE_AUTHOR("Mocean Laboratories");
MODULE_LICENSE("GPL v2");

