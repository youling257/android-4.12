/*
 * Copyright (C) 2012-2015 Red Hat Inc.
 *
 * Based in parts on the udl code. Based in parts on the gm12u320 fb driver:
 * Copyright (C) 2013 Viacheslav Nurmekhamitov <slavrn@yandex.ru>
 * Copyright (C) 2009 Roberto De Ioris <roberto@unbit.it>
 * Copyright (C) 2009 Jaya Kumar <jayakumar.lkml@gmail.com>
 * Copyright (C) 2009 Bernie Thompson <bernie@plugable.com>
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License v2. See the file COPYING in the main directory of this archive for
 * more details.
 */

#include <drm/drmP.h>
#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>
#include "gm12u320_drv.h"

/* dummy encoder */
static void gm12u320_enc_destroy(struct drm_encoder *encoder)
{
	drm_encoder_cleanup(encoder);
	kfree(encoder);
}

static void gm12u320_encoder_disable(struct drm_encoder *encoder)
{
}

static bool gm12u320_mode_fixup(struct drm_encoder *encoder,
			   const struct drm_display_mode *mode,
			   struct drm_display_mode *adjusted_mode)
{
	return true;
}

static void gm12u320_encoder_prepare(struct drm_encoder *encoder)
{
}

static void gm12u320_encoder_commit(struct drm_encoder *encoder)
{
}

static void gm12u320_encoder_mode_set(struct drm_encoder *encoder,
				 struct drm_display_mode *mode,
				 struct drm_display_mode *adjusted_mode)
{
}

static void
gm12u320_encoder_dpms(struct drm_encoder *encoder, int mode)
{
}

static const struct drm_encoder_helper_funcs gm12u320_helper_funcs = {
	.dpms = gm12u320_encoder_dpms,
	.mode_fixup = gm12u320_mode_fixup,
	.prepare = gm12u320_encoder_prepare,
	.mode_set = gm12u320_encoder_mode_set,
	.commit = gm12u320_encoder_commit,
	.disable = gm12u320_encoder_disable,
};

static const struct drm_encoder_funcs gm12u320_enc_funcs = {
	.destroy = gm12u320_enc_destroy,
};

struct drm_encoder *gm12u320_encoder_init(struct drm_device *dev)
{
	struct drm_encoder *encoder;

	encoder = kzalloc(sizeof(struct drm_encoder), GFP_KERNEL);
	if (!encoder)
		return NULL;

	drm_encoder_init(dev, encoder, &gm12u320_enc_funcs,
			 DRM_MODE_ENCODER_TMDS);
	drm_encoder_helper_add(encoder, &gm12u320_helper_funcs);
	encoder->possible_crtcs = 1;
	return encoder;
}
