// SPDX-License-Identifier: GPL-2.0-only

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/display/drm_dsc.h>
#include <drm/display/drm_dsc_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

struct boe_aa545 {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct drm_dsc_config dsc;
	struct regulator_bulk_data supplies[2];
	struct gpio_desc *reset_gpio;
	struct drm_connector *connector;
};

enum panel_aa545_refresh_rate {
	PANEL_AA545_REFRESH_RATE_60HZ,
	PANEL_AA545_REFRESH_RATE_90HZ,
	PANEL_AA545_REFRESH_RATE_120HZ,
};

static inline
struct boe_aa545 *to_boe_aa545(struct drm_panel *panel)
{
	return container_of_const(panel, struct boe_aa545, panel);
}

static void boe_aa545_reset(struct boe_aa545 *ctx)
{
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(5000, 6000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	msleep(30);
}

static void boe_aa545_timing_switch_120hz(struct mipi_dsi_multi_context *ctx)
{
	mipi_dsi_dcs_write_seq_multi(ctx, 0xff, 0x08, 0x58, 0x00);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x5a, 0x00);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x5b, 0x00);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x5e, 0x01);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x60, 0x02);
	mipi_dsi_dcs_write_seq_multi(ctx, 0xff, 0x08, 0x58, 0x20);
	mipi_dsi_dcs_write_seq_multi(ctx, 0xbd, 0x45);
	mipi_dsi_dcs_write_seq_multi(ctx, 0xbe, 0x65);
	mipi_dsi_dcs_write_seq_multi(ctx, 0xff, 0x08, 0x58, 0x02);
	mipi_dsi_dcs_write_seq_multi(ctx, 0xa7, 0x00);
	mipi_dsi_dcs_write_seq_multi(ctx, 0xb0, 0x00, 0x00, 0x80, 0x00, 0x00, 0x10);
	mipi_dsi_dcs_write_seq_multi(ctx, 0xb6, 0x00);
	mipi_dsi_dcs_write_seq_multi(ctx, 0xb8, 0x03);
	mipi_dsi_dcs_write_seq_multi(ctx, 0xff, 0x08, 0x58, 0x2d);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x80, 0x40);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x85, 0x00);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x93, 0x00);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x94, 0x00);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x95, 0x01);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x96, 0x0d);
	mipi_dsi_dcs_write_seq_multi(ctx, 0xa0, 0x00);
	mipi_dsi_dcs_write_seq_multi(ctx, 0xa1, 0x00);
	mipi_dsi_dcs_write_seq_multi(ctx, 0xeb, 0x00);
	mipi_dsi_dcs_write_seq_multi(ctx, 0xff, 0x08, 0x58, 0x00);
}

static void boe_aa545_timing_switch_90hz(struct mipi_dsi_multi_context *ctx)
{
	mipi_dsi_dcs_write_seq_multi(ctx, 0xff, 0x08, 0x58, 0x00);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x5a, 0x00);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x5b, 0x00);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x5e, 0x01);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x60, 0x08);
	mipi_dsi_dcs_write_seq_multi(ctx, 0xff, 0x08, 0x58, 0x20);
	mipi_dsi_dcs_write_seq_multi(ctx, 0xbd, 0x45);
	mipi_dsi_dcs_write_seq_multi(ctx, 0xbe, 0x65);
	mipi_dsi_dcs_write_seq_multi(ctx, 0xff, 0x08, 0x58, 0x02);
	mipi_dsi_dcs_write_seq_multi(ctx, 0xa7, 0x00);
	mipi_dsi_dcs_write_seq_multi(ctx, 0xb0, 0x00, 0x00, 0x80, 0x00, 0x00, 0x10);
	mipi_dsi_dcs_write_seq_multi(ctx, 0xb6, 0x00);
	mipi_dsi_dcs_write_seq_multi(ctx, 0xb8, 0x00);
	mipi_dsi_dcs_write_seq_multi(ctx, 0xff, 0x08, 0x58, 0x2d);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x80, 0x40);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x8b, 0x00);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x93, 0x00);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x94, 0x00);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x95, 0x01);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x96, 0x0d);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x98, 0x00);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x99, 0xff);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x9a, 0xff);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x9b, 0xff);
	mipi_dsi_dcs_write_seq_multi(ctx, 0xa0, 0x00);
	mipi_dsi_dcs_write_seq_multi(ctx, 0xa1, 0x00);
	mipi_dsi_dcs_write_seq_multi(ctx, 0xeb, 0x00);
	mipi_dsi_dcs_write_seq_multi(ctx, 0xff, 0x08, 0x58, 0x00);
}

static void boe_aa545_timing_switch_60hz(struct mipi_dsi_multi_context *ctx)
{
	mipi_dsi_dcs_write_seq_multi(ctx, 0xff, 0x08, 0x58, 0x00);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x5a, 0x00);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x5b, 0x00);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x5e, 0x01);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x60, 0x01);
	mipi_dsi_dcs_write_seq_multi(ctx, 0xff, 0x08, 0x58, 0x20);
	mipi_dsi_dcs_write_seq_multi(ctx, 0xbd, 0x45);
	mipi_dsi_dcs_write_seq_multi(ctx, 0xbe, 0x65);
	mipi_dsi_dcs_write_seq_multi(ctx, 0xff, 0x08, 0x58, 0x02);
	mipi_dsi_dcs_write_seq_multi(ctx, 0xa7, 0x00);
	mipi_dsi_dcs_write_seq_multi(ctx, 0xb0, 0x01, 0x00, 0x80, 0x00, 0x00, 0x10);
	mipi_dsi_dcs_write_seq_multi(ctx, 0xb6, 0x00);
	mipi_dsi_dcs_write_seq_multi(ctx, 0xb8, 0x00);
	mipi_dsi_dcs_write_seq_multi(ctx, 0xff, 0x08, 0x58, 0x2d);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x80, 0x40);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x84, 0x01);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x93, 0x00);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x94, 0x00);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x95, 0x01);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x96, 0x0d);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x98, 0x00);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x99, 0xff);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x9a, 0xff);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x9b, 0xff);
	mipi_dsi_dcs_write_seq_multi(ctx, 0xa0, 0x00);
	mipi_dsi_dcs_write_seq_multi(ctx, 0xa1, 0x00);
	mipi_dsi_dcs_write_seq_multi(ctx, 0xeb, 0x00);
	mipi_dsi_dcs_write_seq_multi(ctx, 0xff, 0x08, 0x58, 0x00);
}

static int boe_aa545_on(struct boe_aa545 *ctx,
					 enum panel_aa545_refresh_rate rate)
{
	struct drm_dsc_picture_parameter_set pps;
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	/* Initialization commands use LP mode, including the DSC PPS. */
	ctx->dsi->mode_flags |= MIPI_DSI_MODE_LPM;

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

	/* The panel's timing-switch sequence requires HS command mode. */
	ctx->dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;
	switch (rate) {
	case PANEL_AA545_REFRESH_RATE_90HZ:
		boe_aa545_timing_switch_90hz(&dsi_ctx);
		break;
	case PANEL_AA545_REFRESH_RATE_60HZ:
		boe_aa545_timing_switch_60hz(&dsi_ctx);
		break;
	case PANEL_AA545_REFRESH_RATE_120HZ:
	default:
		boe_aa545_timing_switch_120hz(&dsi_ctx);
		break;
	}
	ctx->dsi->mode_flags |= MIPI_DSI_MODE_LPM;
	/* Keep panel refresh synchronized with the DRM mode and DPU TE timing. */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0x08, 0x58, 0x07);
	drm_dsc_pps_payload_pack(&pps, &ctx->dsc);
	mipi_dsi_picture_parameter_set_multi(&dsi_ctx, &pps);
	mipi_dsi_compression_mode_ext_multi(&dsi_ctx, true,
					    MIPI_DSI_COMPRESSION_DSC, 0);
	mipi_dsi_msleep(&dsi_ctx, 28);
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

static int boe_aa545_off(struct boe_aa545 *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	ctx->dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xff, 0x08, 0x58, 0x00);
	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 20);
	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);

	return dsi_ctx.accum_err;
}

static const struct drm_display_mode boe_aa545_modes[] = {
	/* 1440x3168 @ 120Hz */
	{
		.clock = 599808,
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
		.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
	},
		/* 1440x3168 @ 90Hz */
	{
		.clock = 449856,
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
	},
		/* 1440x3168 @ 60Hz */
	{
		.clock = 299904,
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
	},
};

static enum panel_aa545_refresh_rate
boe_aa545_get_current_mode(struct boe_aa545 *ctx)
{
	struct drm_connector *connector = ctx->connector;
	struct drm_crtc_state *crtc_state;
	int vrefresh;

	/* Use the preferred 120 Hz mode before a CRTC state is available. */
	if (!connector || !connector->state || !connector->state->crtc)
		return PANEL_AA545_REFRESH_RATE_120HZ;

	crtc_state = connector->state->crtc->state;
	if (!crtc_state)
		return PANEL_AA545_REFRESH_RATE_120HZ;

	vrefresh = drm_mode_vrefresh(&crtc_state->mode);
	switch (vrefresh) {
	case 60:
		return PANEL_AA545_REFRESH_RATE_60HZ;
	case 90:
		return PANEL_AA545_REFRESH_RATE_90HZ;
	case 120:
		return PANEL_AA545_REFRESH_RATE_120HZ;
	default:
		return PANEL_AA545_REFRESH_RATE_120HZ;
	}
}

static int boe_aa545_prepare(struct drm_panel *panel)
{
	struct boe_aa545 *ctx = to_boe_aa545(panel);
	struct device *dev = &ctx->dsi->dev;
	enum panel_aa545_refresh_rate rate;
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
	if (ret < 0) {
		dev_err(dev, "Failed to enable regulators: %d\n", ret);
		return ret;
	}

	boe_aa545_reset(ctx);

	rate = boe_aa545_get_current_mode(ctx);

	ret = boe_aa545_on(ctx, rate);
	if (ret < 0) {
		dev_err(dev, "Failed to initialize panel: %d\n", ret);
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		regulator_bulk_disable(ARRAY_SIZE(ctx->supplies), ctx->supplies);
		return ret;
	}

	return 0;
}

static int boe_aa545_unprepare(struct drm_panel *panel)
{
	struct boe_aa545 *ctx = to_boe_aa545(panel);
	struct device *dev = &ctx->dsi->dev;
	int ret;

	ret = boe_aa545_off(ctx);
	if (ret < 0)
		dev_err(dev, "Failed to un-initialize panel: %d\n", ret);

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	regulator_bulk_disable(ARRAY_SIZE(ctx->supplies), ctx->supplies);

	return 0;
}

static int boe_aa545_get_modes(struct drm_panel *panel,
					       struct drm_connector *connector)
{
	struct boe_aa545 *ctx = to_boe_aa545(panel);
	int i;

	for (i = 0; i < ARRAY_SIZE(boe_aa545_modes); i++) {
		const struct drm_display_mode *m = &boe_aa545_modes[i];
		struct drm_display_mode *mode;

		mode = drm_mode_duplicate(connector->dev, m);
		if (!mode)
			return -ENOMEM;

		drm_mode_set_name(mode);
		drm_mode_probed_add(connector, mode);
	}

	connector->display_info.width_mm = 71;
	connector->display_info.height_mm = 158;
	connector->display_info.bpc = 10;
	ctx->connector = connector;

	return ARRAY_SIZE(boe_aa545_modes);
}

static const struct drm_panel_funcs boe_aa545_panel_funcs = {
	.prepare = boe_aa545_prepare,
	.unprepare = boe_aa545_unprepare,
	.get_modes = boe_aa545_get_modes,
};

static int boe_aa545_bl_update_status(struct backlight_device *bl)
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

static int boe_aa545_bl_get_brightness(struct backlight_device *bl)
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

static const struct backlight_ops boe_aa545_bl_ops = {
	.update_status = boe_aa545_bl_update_status,
	.get_brightness = boe_aa545_bl_get_brightness,
};

static struct backlight_device *
boe_aa545_create_backlight(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	const struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.brightness = 1433,
		.max_brightness = 4094,
	};

	return devm_backlight_device_register(dev, dev_name(dev), dev, dsi,
					      &boe_aa545_bl_ops, &props);
}

static int boe_aa545_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct boe_aa545 *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct boe_aa545, panel,
				   &boe_aa545_panel_funcs,
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
	dsi->mode_flags = MIPI_DSI_MODE_NO_EOT_PACKET |
			  MIPI_DSI_CLOCK_NON_CONTINUOUS;

	ctx->panel.prepare_prev_first = true;

	ctx->panel.backlight = boe_aa545_create_backlight(dsi);
	if (IS_ERR(ctx->panel.backlight))
		return dev_err_probe(dev, PTR_ERR(ctx->panel.backlight),
				     "Failed to create backlight\n");

	drm_panel_add(&ctx->panel);

	/* This panel only supports DSC 1.1; unconditionally enable it */
	dsi->dsc = &ctx->dsc;

	ctx->dsc.dsc_version_major = 1;
	ctx->dsc.dsc_version_minor = 1;

	ctx->dsc.slice_height = 22;
	ctx->dsc.slice_width = 720;
	ctx->dsc.slice_count = 2;
	ctx->dsc.bits_per_component = 10;
	ctx->dsc.bits_per_pixel = 8 << 4; /* 4 fractional bits = 8 bpp */
	ctx->dsc.block_pred_enable = true;
	ctx->dsc.simple_422 = 0;
	ctx->dsc.convert_rgb = 1;
	ctx->dsc.vbr_enable = 0;

	/* Pre-populate DSC 1.1 parameters and RC model */
	ctx->dsc.pic_width = 1440;
	ctx->dsc.pic_height = 3168;
	ctx->dsc.slice_chunk_size = 720;
	ctx->dsc.line_buf_depth = ctx->dsc.bits_per_component + 1;

	drm_dsc_set_const_params(&ctx->dsc);
	drm_dsc_set_rc_buf_thresh(&ctx->dsc);
	drm_dsc_setup_rc_params(&ctx->dsc, DRM_DSC_1_1_PRE_SCR);
	ctx->dsc.initial_scale_value = drm_dsc_initial_scale_value(&ctx->dsc);
	drm_dsc_compute_rc_parameters(&ctx->dsc);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		drm_panel_remove(&ctx->panel);
		return dev_err_probe(dev, ret, "Failed to attach to DSI host\n");
	}

	return 0;
}

static void boe_aa545_remove(struct mipi_dsi_device *dsi)
{
	struct boe_aa545 *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "Failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id boe_aa545_of_match[] = {
	{ .compatible = "boe,aa545-p-3-a0005" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, boe_aa545_of_match);

static struct mipi_dsi_driver boe_aa545_driver = {
	.probe = boe_aa545_probe,
	.remove = boe_aa545_remove,
	.driver = {
		.name = "panel-boe-aa545",
		.of_match_table = boe_aa545_of_match,
	},
};
module_mipi_dsi_driver(boe_aa545_driver);

MODULE_DESCRIPTION("BOE AA545 P 3 A0005 AMOLED panel driver");
MODULE_LICENSE("GPL");
