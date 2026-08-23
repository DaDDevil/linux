// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 FIXME
// Generated with linux-mdss-dsi-panel-driver-generator from vendor device tree:
//   Copyright (c) 2013, The Linux Foundation. All rights reserved. (FIXME)

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/display/drm_dsc.h>
#include <drm/display/drm_dsc_helper.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

struct panel_aa545_p_3_a0005_dsc {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct drm_dsc_config dsc;
	struct regulator_bulk_data supplies[2];
	struct gpio_desc *reset_gpio;
};

static inline
struct panel_aa545_p_3_a0005_dsc *to_panel_aa545_p_3_a0005_dsc(struct drm_panel *panel)
{
	return container_of_const(panel, struct panel_aa545_p_3_a0005_dsc, panel);
}

static void panel_aa545_p_3_a0005_dsc_reset(struct panel_aa545_p_3_a0005_dsc *ctx)
{
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(5000, 6000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	msleep(30);
}

static int panel_aa545_p_3_a0005_dsc_on(struct panel_aa545_p_3_a0005_dsc *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

    ctx->dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0x08, 0x58, 0x02);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xf8, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0x08, 0x58, 0x20);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb3, 0x50);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb5, 0x03);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0x08, 0x58, 0x06);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xd2, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xa1, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0x08, 0x58, 0x21);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xa4, 0x18);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0x08, 0x58, 0x08);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc8, 0x62);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0x08, 0x58, 0x30);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xae, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x80, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0x08, 0x58, 0x07);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x8a, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0x08, 0x58, 0x4f);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x80, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x81, 0x02);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0x08, 0x58, 0x17);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xa0, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0x08, 0x58, 0x02);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x81, 0x03);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0x08, 0x58, 0x06);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xc6, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0x08, 0x58, 0x08);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xd2, 0x05);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0x08, 0x58, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x51, 0x0d, 0xbb, 0x0d, 0xbb);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0x08, 0x58, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x20);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0x08, 0x58, 0x00);
	mipi_dsi_dcs_set_tear_on_multi(&dsi_ctx, MIPI_DSI_DCS_TEAR_MODE_VBLANK);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0x08, 0x58, 0x49);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xab, 0x93);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0x08, 0x58, 0x31);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xd0, 0x81);
	mipi_dsi_usleep_range(&dsi_ctx, 1000, 2000);
	/* Gamma */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x80,
				     0xf0, 0xa0, 0x78, 0x50, 0x26, 0xda, 0xb0,
				     0x88, 0x60, 0xf0, 0x20, 0x0d, 0x0d, 0x06,
				     0xfa, 0xf3, 0xf3, 0xe0);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x81,
				     0xf0, 0x68, 0x4e, 0x34, 0x19, 0xe7, 0xcc,
				     0xb2, 0x98, 0xf0, 0x20, 0x0d, 0x0d, 0x02,
				     0xfe, 0xfc, 0xfc, 0xf5);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x82,
				     0xf0, 0x5a, 0x44, 0x2d, 0x16, 0xea, 0xd3,
				     0xbc, 0xa6, 0xf0, 0x20, 0x0d, 0x0d, 0x04,
				     0xfc, 0xf8, 0xf8, 0xec);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x83,
				     0xf0, 0x50, 0x3c, 0x28, 0x13, 0xed, 0xd8,
				     0xc4, 0xb0, 0xf0, 0x20, 0x0d, 0x03, 0x02,
				     0xfe, 0xfd, 0xf3, 0xe0);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x84,
				     0xf0, 0x50, 0x3c, 0x28, 0x13, 0xed, 0xd8,
				     0xc4, 0xb0, 0xf0, 0x20, 0x0d, 0x03, 0x02,
				     0xfe, 0xfd, 0xf3, 0xe0);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x85,
				     0xf0, 0x50, 0x3c, 0x28, 0x13, 0xed, 0xd8,
				     0xc4, 0xb0, 0xf0, 0x20, 0x0d, 0x03, 0x02,
				     0xfe, 0xfd, 0xf3, 0xe0);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x86,
				     0xf0, 0xa0, 0x78, 0x50, 0x26, 0xda, 0xb0,
				     0x88, 0x60, 0xf0, 0x20, 0x0d, 0x0d, 0x06,
				     0xfa, 0xf3, 0xf3, 0xe0);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x87,
				     0xf0, 0x68, 0x4e, 0x34, 0x19, 0xe7, 0xcc,
				     0xb2, 0x98, 0xf0, 0x20, 0x0d, 0x0d, 0x02,
				     0xfe, 0xfc, 0xfc, 0xf5);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x88,
				     0xf0, 0x5a, 0x44, 0x2d, 0x16, 0xea, 0xd3,
				     0xbc, 0xa6, 0xf0, 0x20, 0x0d, 0x0d, 0x04,
				     0xfc, 0xf8, 0xf8, 0xec);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x89,
				     0xf0, 0x50, 0x3c, 0x28, 0x13, 0xed, 0xd8,
				     0xc4, 0xb0, 0xf0, 0x20, 0x0d, 0x03, 0x02,
				     0xfe, 0xfd, 0xf3, 0xe0);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x8a,
				     0xf0, 0x50, 0x3c, 0x28, 0x13, 0xed, 0xd8,
				     0xc4, 0xb0, 0xf0, 0x20, 0x0d, 0x03, 0x02,
				     0xfe, 0xfd, 0xf3, 0xe0);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x8b,
				     0xf0, 0x50, 0x3c, 0x28, 0x13, 0xed, 0xd8,
				     0xc4, 0xb0, 0xf0, 0x20, 0x0d, 0x03, 0x02,
				     0xfe, 0xfd, 0xf3, 0xe0);
	mipi_dsi_usleep_range(&dsi_ctx, 1000, 2000);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xd0, 0x80);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xa0, 0xb3);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0x08, 0x58, 0x10);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x85, 0x04);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0x08, 0x58, 0x23);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xcc, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0x08, 0x58, 0x69);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb6,
				     0x44, 0x55, 0x44, 0x43, 0x43, 0x33);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb7,
				     0x33, 0x44, 0x55, 0x54, 0x44, 0x44);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb8,
				     0x23, 0x44, 0x55, 0x45, 0x44, 0x44);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb9,
				     0x33, 0x44, 0x55, 0x43, 0x33, 0x33);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xba,
				     0x43, 0x55, 0x22, 0x32, 0x23, 0x43);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xbb,
				     0x55, 0x55, 0x55, 0x53, 0x22, 0x22);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0x08, 0x58, 0x6a);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x86,
				     0x44, 0x55, 0x44, 0x43, 0x43, 0x33);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x87,
				     0x33, 0x44, 0x55, 0x54, 0x44, 0x44);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x88,
				     0x23, 0x44, 0x55, 0x45, 0x44, 0x44);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x89,
				     0x33, 0x44, 0x55, 0x43, 0x33, 0x33);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x8a,
				     0x43, 0x55, 0x22, 0x32, 0x23, 0x43);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x8b,
				     0x55, 0x55, 0x55, 0x53, 0x22, 0x22);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0x08, 0x58, 0x6b);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe6,
				     0x44, 0x55, 0x44, 0x43, 0x43, 0x33);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe7,
				     0x33, 0x34, 0x55, 0x55, 0x55, 0x55);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe8,
				     0x23, 0x44, 0x55, 0x45, 0x44, 0x44);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xe9,
				     0x33, 0x44, 0x55, 0x44, 0x44, 0x44);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xea,
				     0x43, 0x55, 0x22, 0x32, 0x23, 0x43);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xeb,
				     0x55, 0x44, 0x66, 0x65, 0x43, 0x33);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0x08, 0x58, 0x6c);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb6,
				     0x44, 0x55, 0x44, 0x43, 0x43, 0x33);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb7,
				     0x33, 0x34, 0x55, 0x55, 0x55, 0x55);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb8,
				     0x23, 0x44, 0x55, 0x45, 0x44, 0x44);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb9,
				     0x33, 0x44, 0x55, 0x44, 0x44, 0x44);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xba,
				     0x43, 0x55, 0x22, 0x32, 0x23, 0x43);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xbb,
				     0x55, 0x44, 0x66, 0x65, 0x43, 0x33);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0x08, 0x58, 0x23);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xcc, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0x08, 0x58, 0x00);
	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0x08, 0x58, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x5a, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x5b, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_SET_CABC_MIN_BRIGHTNESS,
				     0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x60, 0x02);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0x08, 0x58, 0x20);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xbd, 0x45);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xbe, 0x65);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0x08, 0x58, 0x02);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xa7, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0,
				     0x01, 0x00, 0x80, 0x00, 0x00, 0x10);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb6, 0x20);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb8, 0x03);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0x08, 0x58, 0x2d);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x80, 0x40);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x85, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x93, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x94, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x95, 0x01);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x96, 0x0d);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xa0, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xa1, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xeb, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0x08, 0x58, 0x00);	
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0x08, 0x58, 0x53);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x83, 0x77);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x88,
				     0x10, 0x40, 0x80, 0xf0, 0x20, 0xf0, 0xff,
				     0xff, 0x04, 0x08, 0x18, 0x40, 0xff);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x93,
				     0x00, 0x10, 0x30, 0x30, 0x30, 0x50, 0x70,
				     0x70, 0x00, 0x00, 0x10, 0x30, 0x30, 0x30,
				     0x50, 0x70, 0x70, 0x00, 0x00, 0x10, 0x30,
				     0x30, 0x30, 0x50, 0x70, 0x70, 0x00);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0x90,
				     0x00, 0x00, 0x00, 0x31, 0x31, 0x00, 0x31,
				     0x00, 0x20, 0x20, 0x30, 0x40, 0x00, 0x40,
				     0x00, 0x30, 0x58, 0x58, 0x86, 0x00, 0xb6,
				     0x00, 0x20, 0x58, 0x40, 0x77, 0x00, 0x97,
				     0x00, 0x20, 0x50, 0x4a, 0x72, 0x00, 0xb2,
				     0x00, 0x20, 0x60, 0x58, 0x77, 0x00, 0xb2,
				     0x00, 0x20, 0x70, 0xf0, 0xf0, 0x01, 0x2d,
				     0x00, 0x20, 0x80, 0x98, 0x90, 0x01, 0x00,
				     0x3f, 0x00, 0x20, 0xff, 0xff, 0x03, 0xff);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0x08, 0x58, 0x00);
	mipi_dsi_dcs_set_display_brightness_multi(&dsi_ctx, 0x0000);
    mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);

	return dsi_ctx.accum_err;
}

static int panel_aa545_p_3_a0005_dsc_off(struct panel_aa545_p_3_a0005_dsc *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	ctx->dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0x08, 0x58, 0x00);
	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 20);
	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);

	return dsi_ctx.accum_err;
}

static int panel_aa545_p_3_a0005_dsc_prepare(struct drm_panel *panel)
{
	struct panel_aa545_p_3_a0005_dsc *ctx = to_panel_aa545_p_3_a0005_dsc(panel);
	struct device *dev = &ctx->dsi->dev;
	struct drm_dsc_picture_parameter_set pps;
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
	if (ret < 0) {
		dev_err(dev, "Failed to enable regulators: %d\n", ret);
		return ret;
	}

	panel_aa545_p_3_a0005_dsc_reset(ctx);

	ret = panel_aa545_p_3_a0005_dsc_on(ctx);
	if (ret < 0) {
		dev_err(dev, "Failed to initialize panel: %d\n", ret);
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		regulator_bulk_disable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
		return ret;
	}

	drm_dsc_pps_payload_pack(&pps, &ctx->dsc);

	ret = mipi_dsi_picture_parameter_set(ctx->dsi, &pps);
	if (ret < 0) {
		dev_err(panel->dev, "failed to transmit PPS: %d\n", ret);
		return ret;
	}

	ret = mipi_dsi_compression_mode(ctx->dsi, true);
	if (ret < 0) {
		dev_err(dev, "failed to enable compression mode: %d\n", ret);
		return ret;
	}


	msleep(28); /* TODO: Is this panel-dependent? */

	return 0;
}

static int panel_aa545_p_3_a0005_dsc_unprepare(struct drm_panel *panel)
{
	struct panel_aa545_p_3_a0005_dsc *ctx = to_panel_aa545_p_3_a0005_dsc(panel);
	struct device *dev = &ctx->dsi->dev;
	int ret;

	ret = panel_aa545_p_3_a0005_dsc_off(ctx);
	if (ret < 0)
		dev_err(dev, "Failed to un-initialize panel: %d\n", ret);

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	regulator_bulk_disable(ARRAY_SIZE(ctx->supplies), ctx->supplies);

	return 0;
}

static const struct drm_display_mode panel_aa545_p_3_a0005_dsc_mode = {
	.clock = (1440 + 64 + 8 + 50) * (3168 + 16 + 4 + 12) * 120 / 1000,
	.hdisplay = 1440,
	.hsync_start = 1440 + 64,
	.hsync_end = 1440 + 64 + 8,
	.htotal = 1440 + 64 + 8 + 50,
	.vdisplay = 3168,
	.vsync_start = 3168 + 16,
	.vsync_end = 3168 + 16 + 4,
	.vtotal = 3168 + 16 + 4 + 12,
	.width_mm = 71,
	.height_mm = 158,
	.type = DRM_MODE_TYPE_DRIVER,
};

static int panel_aa545_p_3_a0005_dsc_get_modes(struct drm_panel *panel,
					       struct drm_connector *connector)
{
	return drm_connector_helper_get_modes_fixed(connector, &panel_aa545_p_3_a0005_dsc_mode);
}

static const struct drm_panel_funcs panel_aa545_p_3_a0005_dsc_panel_funcs = {
	.prepare = panel_aa545_p_3_a0005_dsc_prepare,
	.unprepare = panel_aa545_p_3_a0005_dsc_unprepare,
	.get_modes = panel_aa545_p_3_a0005_dsc_get_modes,
};

static int panel_aa545_p_3_a0005_dsc_bl_update_status(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	u16 brightness = backlight_get_brightness(bl);
	int ret;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_set_display_brightness_large(dsi, brightness);
	if (ret < 0)
		return ret;

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	return 0;
}

// TODO: Check if /sys/class/backlight/.../actual_brightness actually returns
// correct values. If not, remove this function.
static int panel_aa545_p_3_a0005_dsc_bl_get_brightness(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	u16 brightness;
	int ret;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_get_display_brightness_large(dsi, &brightness);
	if (ret < 0)
		return ret;

	dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	return brightness;
}

static const struct backlight_ops panel_aa545_p_3_a0005_dsc_bl_ops = {
	.update_status = panel_aa545_p_3_a0005_dsc_bl_update_status,
	.get_brightness = panel_aa545_p_3_a0005_dsc_bl_get_brightness,
};

static struct backlight_device *
panel_aa545_p_3_a0005_dsc_create_backlight(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	const struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.brightness = 1433,
		.max_brightness = 4094,
	};

	return devm_backlight_device_register(dev, dev_name(dev), dev, dsi,
					      &panel_aa545_p_3_a0005_dsc_bl_ops, &props);
}

static int panel_aa545_p_3_a0005_dsc_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct panel_aa545_p_3_a0005_dsc *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct panel_aa545_p_3_a0005_dsc, panel,
				   &panel_aa545_p_3_a0005_dsc_panel_funcs,
				   DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ctx->supplies[0].supply = "vddio";
	ctx->supplies[1].supply = "vci";

	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(ctx->supplies), ctx->supplies);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to get regulators\n");

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "Failed to get reset-gpios\n");

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB101010;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO_BURST |
			  MIPI_DSI_MODE_NO_EOT_PACKET |
			  MIPI_DSI_CLOCK_NON_CONTINUOUS;

	ctx->panel.prepare_prev_first = true;

	ctx->panel.backlight = panel_aa545_p_3_a0005_dsc_create_backlight(dsi);
	if (IS_ERR(ctx->panel.backlight))
		return dev_err_probe(dev, PTR_ERR(ctx->panel.backlight),
				     "Failed to create backlight\n");

	drm_panel_add(&ctx->panel);

	/* This panel only supports DSC; unconditionally enable it */
	dsi->dsc = &ctx->dsc;

	ctx->dsc.dsc_version_major = 1;
	ctx->dsc.dsc_version_minor = 1;

	/* TODO: Pass slice_per_pkt = 2 */
	ctx->dsc.slice_height = 22;
	ctx->dsc.slice_width = 720;
	ctx->dsc.slice_count = 2;
	ctx->dsc.bits_per_component = 10;
	ctx->dsc.bits_per_pixel = 8 << 4; /* 4 fractional bits */
	ctx->dsc.block_pred_enable = true;

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		drm_panel_remove(&ctx->panel);
		return dev_err_probe(dev, ret, "Failed to attach to DSI host\n");
	}

	return 0;
}

static void panel_aa545_p_3_a0005_dsc_remove(struct mipi_dsi_device *dsi)
{
	struct panel_aa545_p_3_a0005_dsc *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "Failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id panel_aa545_p_3_a0005_dsc_of_match[] = {
	{ .compatible = "panel,aa545-p-3-a0005-dsc" }, // FIXME
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, panel_aa545_p_3_a0005_dsc_of_match);

static struct mipi_dsi_driver panel_aa545_p_3_a0005_dsc_driver = {
	.probe = panel_aa545_p_3_a0005_dsc_probe,
	.remove = panel_aa545_p_3_a0005_dsc_remove,
	.driver = {
		.name = "panel-panel-aa545-p-3-a0005-dsc",
		.of_match_table = panel_aa545_p_3_a0005_dsc_of_match,
	},
};
module_mipi_dsi_driver(panel_aa545_p_3_a0005_dsc_driver);

MODULE_AUTHOR("linux-mdss-dsi-panel-driver-generator <fix@me>"); // FIXME
MODULE_DESCRIPTION("DRM driver for AA545 P 3 A0005 dsc cmd mode panel");
MODULE_LICENSE("GPL");
