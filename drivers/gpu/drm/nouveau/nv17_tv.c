/*
 * Copyright (C) 2009 Francisco Jerez.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE COPYRIGHT OWNER(S) AND/OR ITS SUPPLIERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include "drmP.h"
#include "drm_crtc_helper.h"
#include "nouveau_drv.h"
#include "nouveau_encoder.h"
#include "nouveau_connector.h"
#include "nouveau_crtc.h"
#include "nouveau_hw.h"
#include "nv17_tv.h"

enum drm_connector_status nv17_tv_detect(struct drm_encoder *encoder,
					 struct drm_connector *connector,
					 uint32_t pin_mask)
{
	struct nv17_tv_encoder *tv_enc = to_tv_enc(encoder);

	tv_enc->pin_mask = pin_mask >> 28 & 0xe;

	switch (tv_enc->pin_mask) {
	case 0x2:
	case 0x4:
		tv_enc->subconnector = DRM_MODE_SUBCONNECTOR_Composite;
		break;
	case 0xc:
		tv_enc->subconnector = DRM_MODE_SUBCONNECTOR_SVIDEO;
		break;
	case 0xe:
		if (nouveau_encoder(encoder)->dcb->tvconf.has_component_output)
			tv_enc->subconnector = DRM_MODE_SUBCONNECTOR_Component;
		else
			tv_enc->subconnector = DRM_MODE_SUBCONNECTOR_SCART;
		break;
	default:
		tv_enc->subconnector = DRM_MODE_SUBCONNECTOR_Unknown;
		break;
	}

	drm_connector_property_set_value(connector,
			encoder->dev->mode_config.tv_subconnector_property,
							tv_enc->subconnector);

	return tv_enc->subconnector ? connector_status_connected :
					connector_status_disconnected;
}

static const struct {
	int hdisplay;
	int vdisplay;
} modes[] = {
	{ 640, 400 },
	{ 640, 480 },
	{ 720, 480 },
	{ 720, 576 },
	{ 800, 600 },
	{ 1024, 768 },
	{ 1280, 720 },
	{ 1280, 1024 },
	{ 1920, 1080 }
};

static int nv17_tv_get_modes(struct drm_encoder *encoder,
			     struct drm_connector *connector)
{
	struct nv17_tv_norm_params *tv_norm = get_tv_norm(encoder);
	struct drm_display_mode *mode;
	struct drm_display_mode *output_mode;
	int n = 0;
	int i;

	if (tv_norm->kind != CTV_ENC_MODE) {
		struct drm_display_mode *tv_mode;

		for (tv_mode = nv17_tv_modes; tv_mode->hdisplay; tv_mode++) {
			mode = drm_mode_duplicate(encoder->dev, tv_mode);

			mode->clock = tv_norm->tv_enc_mode.vrefresh *
						mode->htotal / 1000 *
						mode->vtotal / 1000;

			if (mode->flags & DRM_MODE_FLAG_DBLSCAN)
				mode->clock *= 2;

			if (mode->hdisplay == tv_norm->tv_enc_mode.hdisplay &&
			    mode->vdisplay == tv_norm->tv_enc_mode.vdisplay)
				mode->type |= DRM_MODE_TYPE_PREFERRED;

			drm_mode_probed_add(connector, mode);
			n++;
		}
		return n;
	}

	/* tv_norm->kind == CTV_ENC_MODE */
	output_mode = &tv_norm->ctv_enc_mode.mode;
	for (i = 0; i < ARRAY_SIZE(modes); i++) {
		if (modes[i].hdisplay > output_mode->hdisplay ||
		    modes[i].vdisplay > output_mode->vdisplay)
			continue;

		if (modes[i].hdisplay == output_mode->hdisplay &&
		    modes[i].vdisplay == output_mode->vdisplay) {
			mode = drm_mode_duplicate(encoder->dev, output_mode);
			mode->type |= DRM_MODE_TYPE_PREFERRED;
		} else {
			mode = drm_cvt_mode(encoder->dev, modes[i].hdisplay,
				modes[i].vdisplay, 60, false,
				output_mode->flags & DRM_MODE_FLAG_INTERLACE,
				false);
		}

		/* CVT modes are sometimes unsuitable... */
		if (output_mode->hdisplay <= 720
		    || output_mode->hdisplay >= 1920) {
			mode->htotal = output_mode->htotal;
			mode->hsync_start = (mode->hdisplay + (mode->htotal
					     - mode->hdisplay) * 9 / 10) & ~7;
			mode->hsync_end = mode->hsync_start + 8;
		}
		if (output_mode->vdisplay >= 1024) {
			mode->vtotal = output_mode->vtotal;
			mode->vsync_start = output_mode->vsync_start;
			mode->vsync_end = output_mode->vsync_end;
		}

		mode->type |= DRM_MODE_TYPE_DRIVER;
		drm_mode_probed_add(connector, mode);
		n++;
	}
	return n;
}

static int nv17_tv_mode_valid(struct drm_encoder *encoder,
			      struct drm_display_mode *mode)
{
	struct nv17_tv_norm_params *tv_norm = get_tv_norm(encoder);

	if (tv_norm->kind == CTV_ENC_MODE) {
		struct drm_display_mode *output_mode =
						&tv_norm->ctv_enc_mode.mode;

		if (mode->clock > 400000)
			return MODE_CLOCK_HIGH;

		if (mode->hdisplay > output_mode->hdisplay ||
		    mode->vdisplay > output_mode->vdisplay)
			return MODE_BAD;

		if ((mode->flags & DRM_MODE_FLAG_INTERLACE) !=
		    (output_mode->flags & DRM_MODE_FLAG_INTERLACE))
			return MODE_NO_INTERLACE;

		if (mode->flags & DRM_MODE_FLAG_DBLSCAN)
			return MODE_NO_DBLESCAN;

	} else {
		const int vsync_tolerance = 600;

		if (mode->clock > 70000)
			return MODE_CLOCK_HIGH;

		if (abs(drm_mode_vrefresh(mode) * 1000 -
			tv_norm->tv_enc_mode.vrefresh) > vsync_tolerance)
			return MODE_VSYNC;

		/* The encoder takes care of the actual interlacing */
		if (mode->flags & DRM_MODE_FLAG_INTERLACE)
			return MODE_NO_INTERLACE;
	}

	return MODE_OK;
}

static bool nv17_tv_mode_fixup(struct drm_encoder *encoder,
			       struct drm_display_mode *mode,
			       struct drm_display_mode *adjusted_mode)
{
	struct nv17_tv_norm_params *tv_norm = get_tv_norm(encoder);

	if (tv_norm->kind == CTV_ENC_MODE)
		adjusted_mode->clock = tv_norm->ctv_enc_mode.mode.clock;
	else
		adjusted_mode->clock = 90000;

	return true;
}

static void  nv17_tv_dpms(struct drm_encoder *encoder, int mode)
{
	struct drm_device *dev = encoder->dev;
	struct nv17_tv_state *regs = &to_tv_enc(encoder)->state;
	struct nv17_tv_norm_params *tv_norm = get_tv_norm(encoder);

	if (nouveau_encoder(encoder)->last_dpms == mode)
		return;
	nouveau_encoder(encoder)->last_dpms = mode;

	NV_INFO(dev, "Setting dpms mode %d on TV encoder (output %d)\n",
		 mode, nouveau_encoder(encoder)->dcb->index);

	regs->ptv_200 &= ~1;

	if (tv_norm->kind == CTV_ENC_MODE) {
		nv04_dfp_update_fp_control(encoder, mode);

	} else {
		nv04_dfp_update_fp_control(encoder, DRM_MODE_DPMS_OFF);

		if (mode == DRM_MODE_DPMS_ON)
			regs->ptv_200 |= 1;
	}

	nv_load_ptv(dev, regs, 200);

	nv17_gpio_set(dev, DCB_GPIO_TVDAC1, mode == DRM_MODE_DPMS_ON);
	nv17_gpio_set(dev, DCB_GPIO_TVDAC0, mode == DRM_MODE_DPMS_ON);

	nv04_dac_update_dacclk(encoder, mode == DRM_MODE_DPMS_ON);
}

static void nv17_tv_prepare(struct drm_encoder *encoder)
{
	struct drm_device *dev = encoder->dev;
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	struct drm_encoder_helper_funcs *helper = encoder->helper_private;
	struct nv17_tv_norm_params *tv_norm = get_tv_norm(encoder);
	int head = nouveau_crtc(encoder->crtc)->index;
	uint8_t *cr_lcd = &dev_priv->mode_reg.crtc_reg[head].CRTC[
							NV_CIO_CRE_LCD__INDEX];
	uint32_t dacclk_off = NV_PRAMDAC_DACCLK +
					nv04_dac_output_offset(encoder);
	uint32_t dacclk;

	helper->dpms(encoder, DRM_MODE_DPMS_OFF);

	nv04_dfp_disable(dev, head);

	/* Unbind any FP encoders from this head if we need the FP
	 * stuff enabled. */
	if (tv_norm->kind == CTV_ENC_MODE) {
		struct drm_encoder *enc;

		list_for_each_entry(enc, &dev->mode_config.encoder_list, head) {
			struct dcb_entry *dcb = nouveau_encoder(enc)->dcb;

			if ((dcb->type == OUTPUT_TMDS ||
			     dcb->type == OUTPUT_LVDS) &&
			     !enc->crtc &&
			     nv04_dfp_get_bound_head(dev, dcb) == head) {
				nv04_dfp_bind_head(dev, dcb, head ^ 1,
						dev_priv->VBIOS.fp.dual_link);
			}
		}

	}

	/* Some NV4x have unknown values (0x3f, 0x50, 0x54, 0x6b, 0x79, 0x7f)
	 * at LCD__INDEX which we don't alter
	 */
	if (!(*cr_lcd & 0x44)) {
		if (tv_norm->kind == CTV_ENC_MODE)
			*cr_lcd = 0x1 | (head ? 0x0 : 0x8);
		else
			*cr_lcd = 0;
	}

	/* Set the DACCLK register */
	dacclk = (NVReadRAMDAC(dev, 0, dacclk_off) & ~0x30) | 0x1;

	if (dev_priv->card_type == NV_40)
		dacclk |= 0x1a << 16;

	if (tv_norm->kind == CTV_ENC_MODE) {
		dacclk |=  0x20;

		if (head)
			dacclk |= 0x100;
		else
			dacclk &= ~0x100;

	} else {
		dacclk |= 0x10;

	}

	NVWriteRAMDAC(dev, 0, dacclk_off, dacclk);
}

static void nv17_tv_mode_set(struct drm_encoder *encoder,
			     struct drm_display_mode *drm_mode,
			     struct drm_display_mode *adjusted_mode)
{
	struct drm_device *dev = encoder->dev;
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	int head = nouveau_crtc(encoder->crtc)->index;
	struct nv04_crtc_reg *regs = &dev_priv->mode_reg.crtc_reg[head];
	struct nv17_tv_state *tv_regs = &to_tv_enc(encoder)->state;
	struct nv17_tv_norm_params *tv_norm = get_tv_norm(encoder);
	int i;

	regs->CRTC[NV_CIO_CRE_53] = 0x40; /* FP_HTIMING */
	regs->CRTC[NV_CIO_CRE_54] = 0; /* FP_VTIMING */
	regs->ramdac_630 = 0x2; /* turn off green mode (tv test pattern?) */
	regs->tv_setup = 1;
	regs->ramdac_8c0 = 0x0;

	if (tv_norm->kind == TV_ENC_MODE) {
		tv_regs->ptv_200 = 0x13111100;
		if (head)
			tv_regs->ptv_200 |= 0x10;

		tv_regs->ptv_20c = 0x808010;
		tv_regs->ptv_304 = 0x2d00000;
		tv_regs->ptv_600 = 0x0;
		tv_regs->ptv_60c = 0x0;
		tv_regs->ptv_610 = 0x1e00000;

		if (tv_norm->tv_enc_mode.vdisplay == 576) {
			tv_regs->ptv_508 = 0x1200000;
			tv_regs->ptv_614 = 0x33;

		} else if (tv_norm->tv_enc_mode.vdisplay == 480) {
			tv_regs->ptv_508 = 0xf00000;
			tv_regs->ptv_614 = 0x13;
		}

		if (dev_priv->card_type >= NV_30) {
			tv_regs->ptv_500 = 0xe8e0;
			tv_regs->ptv_504 = 0x1710;
			tv_regs->ptv_604 = 0x0;
			tv_regs->ptv_608 = 0x0;
		} else {
			if (tv_norm->tv_enc_mode.vdisplay == 576) {
				tv_regs->ptv_604 = 0x20;
				tv_regs->ptv_608 = 0x10;
				tv_regs->ptv_500 = 0x19710;
				tv_regs->ptv_504 = 0x68f0;

			} else if (tv_norm->tv_enc_mode.vdisplay == 480) {
				tv_regs->ptv_604 = 0x10;
				tv_regs->ptv_608 = 0x20;
				tv_regs->ptv_500 = 0x4b90;
				tv_regs->ptv_504 = 0x1b480;
			}
		}

		for (i = 0; i < 0x40; i++)
			tv_regs->tv_enc[i] = tv_norm->tv_enc_mode.tv_enc[i];

	} else {
		struct drm_display_mode *output_mode =
						&tv_norm->ctv_enc_mode.mode;

		/* The registers in PRAMDAC+0xc00 control some timings and CSC
		 * parameters for the CTV encoder (It's only used for "HD" TV
		 * modes, I don't think I have enough working to guess what
		 * they exactly mean...), it's probably connected at the
		 * output of the FP encoder, but it also needs the analog
		 * encoder in its OR enabled and routed to the head it's
		 * using. It's enabled with the DACCLK register, bits [5:4].
		 */
		for (i = 0; i < 38; i++)
			regs->ctv_regs[i] = tv_norm->ctv_enc_mode.ctv_regs[i];

		regs->fp_horiz_regs[FP_DISPLAY_END] = output_mode->hdisplay - 1;
		regs->fp_horiz_regs[FP_TOTAL] = output_mode->htotal - 1;
		regs->fp_horiz_regs[FP_SYNC_START] =
						output_mode->hsync_start - 1;
		regs->fp_horiz_regs[FP_SYNC_END] = output_mode->hsync_end - 1;
		regs->fp_horiz_regs[FP_CRTC] = output_mode->hdisplay +
			max((output_mode->hdisplay-600)/40 - 1, 1);

		regs->fp_vert_regs[FP_DISPLAY_END] = output_mode->vdisplay - 1;
		regs->fp_vert_regs[FP_TOTAL] = output_mode->vtotal - 1;
		regs->fp_vert_regs[FP_SYNC_START] =
						output_mode->vsync_start - 1;
		regs->fp_vert_regs[FP_SYNC_END] = output_mode->vsync_end - 1;
		regs->fp_vert_regs[FP_CRTC] = output_mode->vdisplay - 1;

		regs->fp_control = NV_PRAMDAC_FP_TG_CONTROL_DISPEN_POS |
			NV_PRAMDAC_FP_TG_CONTROL_READ_PROG |
			NV_PRAMDAC_FP_TG_CONTROL_WIDTH_12;

		if (output_mode->flags & DRM_MODE_FLAG_PVSYNC)
			regs->fp_control |= NV_PRAMDAC_FP_TG_CONTROL_VSYNC_POS;
		if (output_mode->flags & DRM_MODE_FLAG_PHSYNC)
			regs->fp_control |= NV_PRAMDAC_FP_TG_CONTROL_HSYNC_POS;

		regs->fp_debug_0 = NV_PRAMDAC_FP_DEBUG_0_YWEIGHT_ROUND |
			NV_PRAMDAC_FP_DEBUG_0_XWEIGHT_ROUND |
			NV_PRAMDAC_FP_DEBUG_0_YINTERP_BILINEAR |
			NV_PRAMDAC_FP_DEBUG_0_XINTERP_BILINEAR |
			NV_RAMDAC_FP_DEBUG_0_TMDS_ENABLED |
			NV_PRAMDAC_FP_DEBUG_0_YSCALE_ENABLE |
			NV_PRAMDAC_FP_DEBUG_0_XSCALE_ENABLE;

		regs->fp_debug_2 = 0;

		regs->fp_margin_color = 0x801080;

	}
}

static void nv17_tv_commit(struct drm_encoder *encoder)
{
	struct drm_device *dev = encoder->dev;
	struct drm_nouveau_private *dev_priv = dev->dev_private;
	struct nouveau_crtc *nv_crtc = nouveau_crtc(encoder->crtc);
	struct nouveau_encoder *nv_encoder = nouveau_encoder(encoder);
	struct drm_encoder_helper_funcs *helper = encoder->helper_private;

	if (get_tv_norm(encoder)->kind == TV_ENC_MODE) {
		nv17_tv_update_rescaler(encoder);
		nv17_tv_update_properties(encoder);
	} else {
		nv17_ctv_update_rescaler(encoder);
	}

	nv17_tv_state_load(dev, &to_tv_enc(encoder)->state);

	/* This could use refinement for flatpanels, but it should work */
	if (dev_priv->chipset < 0x44)
		NVWriteRAMDAC(dev, 0, NV_PRAMDAC_TEST_CONTROL +
					nv04_dac_output_offset(encoder),
					0xf0000000);
	else
		NVWriteRAMDAC(dev, 0, NV_PRAMDAC_TEST_CONTROL +
					nv04_dac_output_offset(encoder),
					0x00100000);

	helper->dpms(encoder, DRM_MODE_DPMS_ON);

	NV_INFO(dev, "Output %s is running on CRTC %d using output %c\n",
		drm_get_connector_name(
			&nouveau_encoder_connector_get(nv_encoder)->base),
		nv_crtc->index, '@' + ffs(nv_encoder->dcb->or));
}

static void nv17_tv_save(struct drm_encoder *encoder)
{
	struct drm_device *dev = encoder->dev;
	struct nv17_tv_encoder *tv_enc = to_tv_enc(encoder);

	nouveau_encoder(encoder)->restore.output =
					NVReadRAMDAC(dev, 0,
					NV_PRAMDAC_DACCLK +
					nv04_dac_output_offset(encoder));

	nv17_tv_state_save(dev, &tv_enc->saved_state);

	tv_enc->state.ptv_200 = tv_enc->saved_state.ptv_200;
}

static void nv17_tv_restore(struct drm_encoder *encoder)
{
	struct drm_device *dev = encoder->dev;

	NVWriteRAMDAC(dev, 0, NV_PRAMDAC_DACCLK +
				nv04_dac_output_offset(encoder),
				nouveau_encoder(encoder)->restore.output);

	nv17_tv_state_load(dev, &to_tv_enc(encoder)->saved_state);
}

static int nv17_tv_create_resources(struct drm_encoder *encoder,
				    struct drm_connector *connector)
{
	struct drm_device *dev = encoder->dev;
	struct drm_mode_config *conf = &dev->mode_config;
	struct nv17_tv_encoder *tv_enc = to_tv_enc(encoder);
	struct dcb_entry *dcb = nouveau_encoder(encoder)->dcb;
	int num_tv_norms = dcb->tvconf.has_component_output ? NUM_TV_NORMS :
							NUM_LD_TV_NORMS;
	int i;

	if (nouveau_tv_norm) {
		for (i = 0; i < num_tv_norms; i++) {
			if (!strcmp(nv17_tv_norm_names[i], nouveau_tv_norm)) {
				tv_enc->tv_norm = i;
				break;
			}
		}

		if (i == num_tv_norms)
			NV_WARN(dev, "Invalid TV norm setting \"%s\"\n",
				nouveau_tv_norm);
	}

	drm_mode_create_tv_properties(dev, num_tv_norms, nv17_tv_norm_names);

	drm_connector_attach_property(connector,
					conf->tv_select_subconnector_property,
					tv_enc->select_subconnector);
	drm_connector_attach_property(connector,
					conf->tv_subconnector_property,
					tv_enc->subconnector);
	drm_connector_attach_property(connector,
					conf->tv_mode_property,
					tv_enc->tv_norm);
	drm_connector_attach_property(connector,
					conf->tv_flicker_reduction_property,
					tv_enc->flicker);
	drm_connector_attach_property(connector,
					conf->tv_saturation_property,
					tv_enc->saturation);
	drm_connector_attach_property(connector,
					conf->tv_hue_property,
					tv_enc->hue);
	drm_connector_attach_property(connector,
					conf->tv_overscan_property,
					tv_enc->overscan);

	return 0;
}

static int nv17_tv_set_property(struct drm_encoder *encoder,
				struct drm_connector *connector,
				struct drm_property *property,
				uint64_t val)
{
	struct drm_mode_config *conf = &encoder->dev->mode_config;
	struct drm_crtc *crtc = encoder->crtc;
	struct nv17_tv_encoder *tv_enc = to_tv_enc(encoder);
	struct nv17_tv_norm_params *tv_norm = get_tv_norm(encoder);
	bool modes_changed = false;

	if (property == conf->tv_overscan_property) {
		tv_enc->overscan = val;
		if (encoder->crtc) {
			if (tv_norm->kind == CTV_ENC_MODE)
				nv17_ctv_update_rescaler(encoder);
			else
				nv17_tv_update_rescaler(encoder);
		}

	} else if (property == conf->tv_saturation_property) {
		if (tv_norm->kind != TV_ENC_MODE)
			return -EINVAL;

		tv_enc->saturation = val;
		nv17_tv_update_properties(encoder);

	} else if (property == conf->tv_hue_property) {
		if (tv_norm->kind != TV_ENC_MODE)
			return -EINVAL;

		tv_enc->hue = val;
		nv17_tv_update_properties(encoder);

	} else if (property == conf->tv_flicker_reduction_property) {
		if (tv_norm->kind != TV_ENC_MODE)
			return -EINVAL;

		tv_enc->flicker = val;
		if (encoder->crtc)
			nv17_tv_update_rescaler(encoder);

	} else if (property == conf->tv_mode_property) {
		if (connector->dpms != DRM_MODE_DPMS_OFF)
			return -EINVAL;

		tv_enc->tv_norm = val;

		modes_changed = true;

	} else if (property == conf->tv_select_subconnector_property) {
		if (tv_norm->kind != TV_ENC_MODE)
			return -EINVAL;

		tv_enc->select_subconnector = val;
		nv17_tv_update_properties(encoder);

	} else {
		return -EINVAL;
	}

	if (modes_changed) {
		drm_helper_probe_single_connector_modes(connector, 0, 0);

		/* Disable the crtc to ensure a full modeset is
		 * performed whenever it's turned on again. */
		if (crtc) {
			struct drm_mode_set modeset = {
				.crtc = crtc,
			};

			crtc->funcs->set_config(&modeset);
		}
	}

	return 0;
}

static void nv17_tv_destroy(struct drm_encoder *encoder)
{
	struct nv17_tv_encoder *tv_enc = to_tv_enc(encoder);

	NV_DEBUG_KMS(encoder->dev, "\n");

	drm_encoder_cleanup(encoder);
	kfree(tv_enc);
}

static struct drm_encoder_helper_funcs nv17_tv_helper_funcs = {
	.dpms = nv17_tv_dpms,
	.save = nv17_tv_save,
	.restore = nv17_tv_restore,
	.mode_fixup = nv17_tv_mode_fixup,
	.prepare = nv17_tv_prepare,
	.commit = nv17_tv_commit,
	.mode_set = nv17_tv_mode_set,
	.detect = nv17_dac_detect,
};

static struct drm_encoder_slave_funcs nv17_tv_slave_funcs = {
	.get_modes = nv17_tv_get_modes,
	.mode_valid = nv17_tv_mode_valid,
	.create_resources = nv17_tv_create_resources,
	.set_property = nv17_tv_set_property,
};

static struct drm_encoder_funcs nv17_tv_funcs = {
	.destroy = nv17_tv_destroy,
};

int nv17_tv_create(struct drm_device *dev, struct dcb_entry *entry)
{
	struct drm_encoder *encoder;
	struct nv17_tv_encoder *tv_enc = NULL;

	tv_enc = kzalloc(sizeof(*tv_enc), GFP_KERNEL);
	if (!tv_enc)
		return -ENOMEM;

	tv_enc->overscan = 50;
	tv_enc->flicker = 50;
	tv_enc->saturation = 50;
	tv_enc->hue = 0;
	tv_enc->tv_norm = TV_NORM_PAL;
	tv_enc->subconnector = DRM_MODE_SUBCONNECTOR_Unknown;
	tv_enc->select_subconnector = DRM_MODE_SUBCONNECTOR_Automatic;
	tv_enc->pin_mask = 0;

	encoder = to_drm_encoder(&tv_enc->base);

	tv_enc->base.dcb = entry;
	tv_enc->base.or = ffs(entry->or) - 1;

	drm_encoder_init(dev, encoder, &nv17_tv_funcs, DRM_MODE_ENCODER_TVDAC);
	drm_encoder_helper_add(encoder, &nv17_tv_helper_funcs);
	to_encoder_slave(encoder)->slave_funcs = &nv17_tv_slave_funcs;

	encoder->possible_crtcs = entry->heads;
	encoder->possible_clones = 0;

	return 0;
}
