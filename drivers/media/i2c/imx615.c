// SPDX-License-Identifier: GPL-2.0
/*
 * Sony IMX615 Camera Sensor Driver
 *
 * Copyright (C) 2024 Linaro Ltd.
 * Copyright (C) 2026 OnePlus / OPPO open source port
 */

#include <linux/unaligned.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>

#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

/* Streaming Mode */
#define IMX615_REG_MODE_SELECT		0x0100
#define IMX615_MODE_STANDBY		0x00
#define IMX615_MODE_STREAMING		0x01

/* Lines per frame (FLL) */
#define IMX615_REG_LPFR			0x0340

/* Chip ID */
#define IMX615_REG_ID			0x0016
#define IMX615_ID			0x0615

/* Factory identifier in OTP page 127. */
#define IMX615_REG_OTP_MODE		0x0a00
#define IMX615_REG_OTP_STATUS		0x0a01
#define IMX615_REG_OTP_PAGE		0x0a02
#define IMX615_REG_OTP_FAB_ID		0x0a1f
#define IMX615_OTP_FAB_ID_PAGE		0x7f
#define IMX615_OTP_READ_ENABLE		0x01
#define IMX615_OTP_READ_READY		0x01
#define IMX615_FAB_ID_2			0xb4

/* Exposure control */
#define IMX615_REG_EXPOSURE_CIT		0x0202
#define IMX615_EXPOSURE_MIN		16
#define IMX615_EXPOSURE_OFFSET		48
#define IMX615_EXPOSURE_STEP		1
#define IMX615_EXPOSURE_DEFAULT		0x0648

/* Analog gain control */
#define IMX615_REG_AGAIN		0x0204
#define IMX615_AGAIN_MIN		0
#define IMX615_AGAIN_MAX		978
#define IMX615_AGAIN_STEP		1
#define IMX615_AGAIN_DEFAULT		0

/* Group hold register */
#define IMX615_REG_HOLD			0x0104

#define IMX615_REG_TEST_PATTERN		0x0601

/* Input clock rate */
#define IMX615_INCLK_RATE		24000000

/* CSI2 HW configuration */
#define IMX615_LINK_FREQ		455000000LL
#define IMX615_NUM_DATA_LANES		4

#define IMX615_PIXEL_ARRAY_WIDTH		6560
#define IMX615_PIXEL_ARRAY_HEIGHT		4928

struct imx615_reg {
	u16 address;
	u8 val;
};

struct imx615_reg_list {
	u32 num_of_regs;
	const struct imx615_reg *regs;
};

struct imx615_mode {
	u32 width;
	u32 height;
	u32 code;
	u32 hblank;
	u32 vblank;
	u32 vblank_min;
	u32 vblank_max;
	u64 pclk;
	u32 link_freq_idx;
	struct imx615_reg_list reg_list;
};

static const char * const imx615_supply_names[] = {
	"dovdd",	/* Digital I/O power (1.8V) */
	"avdd",		/* Analog power (2.8V) */
	"dvdd",		/* Digital core power (1.1V) */
};

struct imx615 {
	struct device *dev;
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct gpio_desc *reset_gpio;
	struct clk *inclk;
	struct regulator_bulk_data supplies[ARRAY_SIZE(imx615_supply_names)];
	struct v4l2_ctrl_handler ctrl_handler;
	struct {
		struct v4l2_ctrl *link_freq_ctrl;
		struct v4l2_ctrl *pclk_ctrl;
		struct v4l2_ctrl *hblank_ctrl;
		struct v4l2_ctrl *vblank_ctrl;
	};
	struct {
		struct v4l2_ctrl *exp_ctrl;
		struct v4l2_ctrl *again_ctrl;
	};
	bool streaming;
	const struct imx615_mode *cur_mode;
	struct mutex mutex;
	u8 fab_id;
};

static const s64 link_freq[] = {
	IMX615_LINK_FREQ,
};

static const char * const imx615_test_pattern_menu[] = {
	"Disabled",
	"Color Bars",
};

/* 238 tuning registers from OnePlus 12 downstream kernel */
static const struct imx615_reg imx615_init_regs[] = {
	{ 0x0136, 0x18 },
	{ 0x0137, 0x00 },
	{ 0x3C7E, 0x01 },
	{ 0x3C7F, 0x01 },
	{ 0x0111, 0x02 },
	{ 0x380C, 0x00 },
	{ 0x3C00, 0x01 },
	{ 0x3C01, 0x00 },
	{ 0x3C02, 0x00 },
	{ 0x3C03, 0x03 },
	{ 0x3C04, 0xFF },
	{ 0x3C05, 0x01 },
	{ 0x3C06, 0x00 },
	{ 0x3C07, 0x00 },
	{ 0x3C08, 0x03 },
	{ 0x3C09, 0xFF },
	{ 0x3C0A, 0x00 },
	{ 0x3C0B, 0x00 },
	{ 0x3C0C, 0x10 },
	{ 0x3C0D, 0x10 },
	{ 0x3C0E, 0x10 },
	{ 0x3C0F, 0x10 },
	{ 0x3C10, 0x10 },
	{ 0x3C11, 0x20 },
	{ 0x3C15, 0x00 },
	{ 0x3C16, 0x00 },
	{ 0x3C17, 0x01 },
	{ 0x3C18, 0x00 },
	{ 0x3C19, 0x01 },
	{ 0x3C1A, 0x00 },
	{ 0x3C1B, 0x01 },
	{ 0x3C1C, 0x00 },
	{ 0x3C1D, 0x01 },
	{ 0x3C1E, 0x00 },
	{ 0x3C1F, 0x00 },
	{ 0x3F89, 0x01 },
	{ 0x3F8F, 0x01 },
	{ 0x53B9, 0x01 },
	{ 0x62C4, 0x04 },
	{ 0x658F, 0x07 },
	{ 0x6590, 0x05 },
	{ 0x6591, 0x07 },
	{ 0x6592, 0x05 },
	{ 0x6593, 0x07 },
	{ 0x6594, 0x05 },
	{ 0x6595, 0x07 },
	{ 0x6596, 0x05 },
	{ 0x6597, 0x05 },
	{ 0x6598, 0x05 },
	{ 0x6599, 0x05 },
	{ 0x659A, 0x05 },
	{ 0x659B, 0x05 },
	{ 0x659C, 0x05 },
	{ 0x659D, 0x05 },
	{ 0x659E, 0x07 },
	{ 0x659F, 0x05 },
	{ 0x65A0, 0x07 },
	{ 0x65A1, 0x05 },
	{ 0x65A2, 0x07 },
	{ 0x65A3, 0x05 },
	{ 0x65A4, 0x07 },
	{ 0x65A5, 0x05 },
	{ 0x65A6, 0x05 },
	{ 0x65A7, 0x05 },
	{ 0x65A8, 0x05 },
	{ 0x65A9, 0x05 },
	{ 0x65AA, 0x05 },
	{ 0x65AB, 0x05 },
	{ 0x65AC, 0x05 },
	{ 0x65AD, 0x07 },
	{ 0x65AE, 0x07 },
	{ 0x65AF, 0x07 },
	{ 0x65B0, 0x05 },
	{ 0x65B1, 0x05 },
	{ 0x65B2, 0x05 },
	{ 0x65B3, 0x05 },
	{ 0x65B4, 0x07 },
	{ 0x65B5, 0x07 },
	{ 0x65B6, 0x07 },
	{ 0x65B7, 0x07 },
	{ 0x65B8, 0x05 },
	{ 0x65B9, 0x05 },
	{ 0x65BA, 0x05 },
	{ 0x65BB, 0x05 },
	{ 0x65BC, 0x05 },
	{ 0x65BD, 0x05 },
	{ 0x65BE, 0x05 },
	{ 0x65BF, 0x05 },
	{ 0x65C0, 0x05 },
	{ 0x65C1, 0x05 },
	{ 0x65C2, 0x05 },
	{ 0x65C3, 0x05 },
	{ 0x65C4, 0x05 },
	{ 0x65C5, 0x05 },
	{ 0x6E1C, 0x00 },
	{ 0x6E1D, 0x00 },
	{ 0x6E25, 0x00 },
	{ 0x6E38, 0x03 },
	{ 0x895C, 0x01 },
	{ 0x895D, 0x00 },
	{ 0x8966, 0x00 },
	{ 0x8967, 0x4E },
	{ 0x896A, 0x00 },
	{ 0x896B, 0x24 },
	{ 0x896F, 0x34 },
	{ 0x8976, 0x00 },
	{ 0x8977, 0x00 },
	{ 0x9004, 0x1F },
	{ 0x9200, 0xB7 },
	{ 0x9201, 0x34 },
	{ 0x9202, 0xB7 },
	{ 0x9203, 0x36 },
	{ 0x9204, 0xB7 },
	{ 0x9205, 0x37 },
	{ 0x9206, 0xB7 },
	{ 0x9207, 0x38 },
	{ 0x9208, 0xB7 },
	{ 0x9209, 0x39 },
	{ 0x920A, 0xB7 },
	{ 0x920B, 0x3A },
	{ 0x920C, 0xB7 },
	{ 0x920D, 0x3C },
	{ 0x920E, 0xB7 },
	{ 0x920F, 0x3D },
	{ 0x9210, 0xB7 },
	{ 0x9211, 0x3E },
	{ 0x9212, 0xB7 },
	{ 0x9213, 0x3F },
	{ 0x9214, 0xF6 },
	{ 0x9215, 0x13 },
	{ 0x9216, 0xF6 },
	{ 0x9217, 0x34 },
	{ 0x9218, 0xF4 },
	{ 0x9219, 0xA7 },
	{ 0x921A, 0xF4 },
	{ 0x921B, 0xAA },
	{ 0x921C, 0xF4 },
	{ 0x921D, 0xAD },
	{ 0x921E, 0xF4 },
	{ 0x921F, 0xB0 },
	{ 0x9220, 0xF4 },
	{ 0x9221, 0xB3 },
	{ 0x9222, 0x85 },
	{ 0x9223, 0x77 },
	{ 0x9224, 0xC4 },
	{ 0x9225, 0x4B },
	{ 0x9226, 0xC4 },
	{ 0x9227, 0x4C },
	{ 0x9228, 0xC4 },
	{ 0x9229, 0x4D },
	{ 0x922A, 0xF5 },
	{ 0x922B, 0x5E },
	{ 0x922C, 0xF5 },
	{ 0x922D, 0x5F },
	{ 0x922E, 0xF5 },
	{ 0x922F, 0x64 },
	{ 0x9230, 0xF5 },
	{ 0x9231, 0x65 },
	{ 0x9232, 0xF5 },
	{ 0x9233, 0x6A },
	{ 0x9234, 0xF5 },
	{ 0x9235, 0x6B },
	{ 0x9236, 0xF5 },
	{ 0x9237, 0x70 },
	{ 0x9238, 0xF5 },
	{ 0x9239, 0x71 },
	{ 0x923A, 0xF5 },
	{ 0x923B, 0x76 },
	{ 0x923C, 0xF5 },
	{ 0x923D, 0x77 },
	{ 0x9810, 0x14 },
	{ 0x9814, 0x14 },
	{ 0xC020, 0x00 },
	{ 0xC026, 0x00 },
	{ 0xC027, 0x00 },
	{ 0xC448, 0x01 },
	{ 0xC44F, 0x01 },
	{ 0xC450, 0x00 },
	{ 0xC451, 0x00 },
	{ 0xC452, 0x01 },
	{ 0xC455, 0x00 },
	{ 0xE186, 0x36 },
	{ 0xE206, 0x35 },
	{ 0xE226, 0x33 },
	{ 0xE266, 0x34 },
	{ 0xE2A6, 0x31 },
	{ 0xE2C6, 0x37 },
	{ 0xE2E6, 0x32 },
	{ 0x88D6, 0x60 },
	{ 0x9852, 0x00 },
	{ 0xA569, 0x06 },
	{ 0xA56A, 0x13 },
	{ 0xA56B, 0x13 },
	{ 0xA56C, 0x01 },
	{ 0xA678, 0x00 },
	{ 0xA679, 0x20 },
	{ 0xA812, 0x00 },
	{ 0xA813, 0x3F },
	{ 0xA814, 0x3F },
	{ 0xA830, 0x68 },
	{ 0xA831, 0x56 },
	{ 0xA832, 0x2B },
	{ 0xA833, 0x55 },
	{ 0xA834, 0x55 },
	{ 0xA835, 0x16 },
	{ 0xA837, 0x51 },
	{ 0xA838, 0x34 },
	{ 0xA854, 0x4F },
	{ 0xA855, 0x48 },
	{ 0xA856, 0x45 },
	{ 0xA857, 0x02 },
	{ 0xA85A, 0x23 },
	{ 0xA85B, 0x16 },
	{ 0xA85C, 0x12 },
	{ 0xA85D, 0x02 },
	{ 0xAA55, 0x00 },
	{ 0xAA56, 0x01 },
	{ 0xAA57, 0x30 },
	{ 0xAA58, 0x01 },
	{ 0xAA59, 0x30 },
	{ 0xAC72, 0x01 },
	{ 0xAC73, 0x26 },
	{ 0xAC74, 0x01 },
	{ 0xAC75, 0x26 },
	{ 0xAC76, 0x00 },
	{ 0xAC77, 0xC4 },
	{ 0xAE09, 0xFF },
	{ 0xAE0A, 0xFF },
	{ 0xAE12, 0x58 },
	{ 0xAE13, 0x58 },
	{ 0xAE15, 0x10 },
	{ 0xAE16, 0x10 },
	{ 0xAF05, 0x48 },
	{ 0xB069, 0x02 },
	{ 0xEA4B, 0x00 },
	{ 0xEA4C, 0x00 },
	{ 0xEA4D, 0x00 },
	{ 0xEA4E, 0x00 },
};

/* Fab2-specific settings, applied after the common initialization table. */
static const struct imx615_reg imx615_fab2_init_regs[] = {
	{ 0x574b, 0x01 },
	{ 0x5765, 0x33 },
};

/*
 * OnePlusOSS android_kernel_oneplus_mt6893, oneplus/MT6893_R_11.0,
 * imx615_mipi_raw/imx615mipiraw_Sensor.c: normal_video_setting.
 * 24 MHz XCLK, 3264x1856 RAW10, four D-PHY lanes, approximately 30 fps.
 * VT pixel rate: 24 MHz / 4 * 292 / 4 / 5 * 4 = 350.4 MHz.
 * CSI link frequency: 24 MHz / 6 * 455 / 2 / 2 = 455 MHz.
 * Retain the Waffle global tuning above; only its XCLK setting is changed.
 */
/*
 * Full-array variant of the mode below: 6560x4928 -> 3280x2464 binning,
 * followed by an 8-pixel crop on every edge. PLL and timing are unchanged.
 */
static const struct imx615_reg mode_3264x2448_regs[] = {
	{ 0x0112, 0x0a },
	{ 0x0113, 0x0a },
	{ 0x0114, 0x03 },
	{ 0x0342, 0x0e },
	{ 0x0343, 0xb8 },
	{ 0x0340, 0x0c },
	{ 0x0341, 0x1a },
	{ 0x0344, 0x00 },
	{ 0x0345, 0x00 },
	{ 0x0346, 0x00 },
	{ 0x0347, 0x00 },
	{ 0x0348, 0x19 },
	{ 0x0349, 0x9f },
	{ 0x034a, 0x13 },
	{ 0x034b, 0x3f },
	{ 0x0900, 0x01 },
	{ 0x0901, 0x22 },
	{ 0x0902, 0x08 },
	{ 0x3246, 0x81 },
	{ 0x3247, 0x81 },
	{ 0x0401, 0x00 },
	{ 0x0404, 0x00 },
	{ 0x0405, 0x10 },
	{ 0x0408, 0x00 },
	{ 0x0409, 0x08 },
	{ 0x040a, 0x00 },
	{ 0x040b, 0x08 },
	{ 0x040c, 0x0c },
	{ 0x040d, 0xc0 },
	{ 0x040e, 0x09 },
	{ 0x040f, 0x90 },
	{ 0x034c, 0x0c },
	{ 0x034d, 0xc0 },
	{ 0x034e, 0x09 },
	{ 0x034f, 0x90 },
	{ 0x0301, 0x05 },
	{ 0x0303, 0x04 },
	{ 0x0305, 0x04 },
	{ 0x0306, 0x01 },
	{ 0x0307, 0x24 },
	{ 0x030b, 0x02 },
	{ 0x030d, 0x06 },
	{ 0x030e, 0x01 },
	{ 0x030f, 0xc7 },
	{ 0x0310, 0x01 },
	{ 0x3620, 0x00 },
	{ 0x3621, 0x00 },
	{ 0x3c12, 0x56 },
	{ 0x3c13, 0x52 },
	{ 0x3c14, 0x3e },
	{ 0x3f0c, 0x00 },
	{ 0x3f14, 0x01 },
	{ 0x3f80, 0x00 },
	{ 0x3f81, 0xa0 },
	{ 0x3f8c, 0x00 },
	{ 0x3f8d, 0x00 },
	{ 0x3ffc, 0x00 },
	{ 0x3ffd, 0x1e },
	{ 0x3ffe, 0x00 },
	{ 0x3fff, 0xdc },
	{ 0x0202, 0x0b },
	{ 0x0203, 0xea },
	{ 0x0204, 0x00 },
	{ 0x0205, 0x70 },
	{ 0x020e, 0x01 },
	{ 0x020f, 0x00 },
	{ 0x0210, 0x01 },
	{ 0x0211, 0x00 },
	{ 0x0212, 0x01 },
	{ 0x0213, 0x00 },
	{ 0x0214, 0x01 },
	{ 0x0215, 0x00 },
};

static const struct imx615_reg mode_3264x1856_regs[] = {
	{ 0x0112, 0x0a },
	{ 0x0113, 0x0a },
	{ 0x0114, 0x03 },
	{ 0x0342, 0x0e },
	{ 0x0343, 0xb8 },
	{ 0x0340, 0x0c },
	{ 0x0341, 0x1a },
	{ 0x0344, 0x00 },
	{ 0x0345, 0x00 },
	{ 0x0346, 0x02 },
	{ 0x0347, 0x60 },
	{ 0x0348, 0x19 },
	{ 0x0349, 0x9f },
	{ 0x034a, 0x10 },
	{ 0x034b, 0xdf },
	{ 0x0900, 0x01 },
	{ 0x0901, 0x22 },
	{ 0x0902, 0x08 },
	{ 0x3246, 0x81 },
	{ 0x3247, 0x81 },
	{ 0x0401, 0x00 },
	{ 0x0404, 0x00 },
	{ 0x0405, 0x10 },
	{ 0x0408, 0x00 },
	{ 0x0409, 0x08 },
	{ 0x040a, 0x00 },
	{ 0x040b, 0x00 },
	{ 0x040c, 0x0c },
	{ 0x040d, 0xc0 },
	{ 0x040e, 0x07 },
	{ 0x040f, 0x40 },
	{ 0x034c, 0x0c },
	{ 0x034d, 0xc0 },
	{ 0x034e, 0x07 },
	{ 0x034f, 0x40 },
	{ 0x0301, 0x05 },
	{ 0x0303, 0x04 },
	{ 0x0305, 0x04 },
	{ 0x0306, 0x01 },
	{ 0x0307, 0x24 },
	{ 0x030b, 0x02 },
	{ 0x030d, 0x06 },
	{ 0x030e, 0x01 },
	{ 0x030f, 0xc7 },
	{ 0x0310, 0x01 },
	{ 0x3620, 0x00 },
	{ 0x3621, 0x00 },
	{ 0x3c12, 0x56 },
	{ 0x3c13, 0x52 },
	{ 0x3c14, 0x3e },
	{ 0x3f0c, 0x00 },
	{ 0x3f14, 0x01 },
	{ 0x3f80, 0x00 },
	{ 0x3f81, 0xa0 },
	{ 0x3f8c, 0x00 },
	{ 0x3f8d, 0x00 },
	{ 0x3ffc, 0x00 },
	{ 0x3ffd, 0x1e },
	{ 0x3ffe, 0x00 },
	{ 0x3fff, 0xdc },
	{ 0x0202, 0x0b },
	{ 0x0203, 0xea },
	{ 0x0204, 0x00 },
	{ 0x0205, 0x70 },
	{ 0x020e, 0x01 },
	{ 0x020f, 0x00 },
	{ 0x0210, 0x01 },
	{ 0x0211, 0x00 },
	{ 0x0212, 0x01 },
	{ 0x0213, 0x00 },
	{ 0x0214, 0x01 },
	{ 0x0215, 0x00 },
};

static const struct imx615_mode supported_modes[] = {
	{
		.width = 3264,
		.height = 2448,
		.code = MEDIA_BUS_FMT_SRGGB10_1X10,
		.hblank = 3768 - 3264,
		.vblank = 3098 - 2448,
		.vblank_min = 3098 - 2448,
		.vblank_max = 0xffff - 5 - 2448,
		.pclk = 350400000,
		.link_freq_idx = 0,
		.reg_list = {
			.num_of_regs = ARRAY_SIZE(mode_3264x2448_regs),
			.regs = mode_3264x2448_regs,
		},
	},
	{
		.width = 3264,
		.height = 1856,
		.code = MEDIA_BUS_FMT_SRGGB10_1X10,
		.hblank = 3768 - 3264,
		.vblank = 3098 - 1856,
		.vblank_min = 3098 - 1856,
		.vblank_max = 0xffff - 5 - 1856,
		.pclk = 350400000,
		.link_freq_idx = 0,
		.reg_list = {
			.num_of_regs = ARRAY_SIZE(mode_3264x1856_regs),
			.regs = mode_3264x1856_regs,
		},
	},
};

static inline struct imx615 *to_imx615(struct v4l2_subdev *subdev)
{
	return container_of(subdev, struct imx615, sd);
}

static int imx615_read_reg(struct imx615 *imx615, u16 reg, u32 len, u32 *val)
{
	struct i2c_client *client = v4l2_get_subdevdata(&imx615->sd);
	struct i2c_msg msgs[2] = {0};
	u8 addr_buf[2] = {0};
	u8 data_buf[4] = {0};
	int ret;

	if (WARN_ON(!len || len > 4))
		return -EINVAL;

	put_unaligned_be16(reg, addr_buf);

	msgs[0].addr = client->addr;
	msgs[0].flags = 0;
	msgs[0].len = ARRAY_SIZE(addr_buf);
	msgs[0].buf = addr_buf;

	msgs[1].addr = client->addr;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len = len;
	msgs[1].buf = &data_buf[4 - len];

	ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
	if (ret != ARRAY_SIZE(msgs))
		return ret < 0 ? ret : -EIO;

	*val = get_unaligned_be32(data_buf);

	return 0;
}

static int imx615_write_reg(struct imx615 *imx615, u16 reg, u32 len, u32 val)
{
	struct i2c_client *client = v4l2_get_subdevdata(&imx615->sd);
	u8 buf[6] = {0};
	int ret;

	if (WARN_ON(!len || len > 4))
		return -EINVAL;

	put_unaligned_be16(reg, buf);
	put_unaligned_be32(val << (8 * (4 - len)), buf + 2);

	ret = i2c_master_send(client, buf, len + 2);
	if (ret != len + 2)
		return ret < 0 ? ret : -EIO;

	return 0;
}

static int imx615_write_regs(struct imx615 *imx615,
			     const struct imx615_reg *regs, u32 len)
{
	unsigned int i;
	int ret;

	for (i = 0; i < len; i++) {
		ret = imx615_write_reg(imx615, regs[i].address, 1, regs[i].val);
		if (ret)
			return ret;
	}

	return 0;
}

static int imx615_update_controls(struct imx615 *imx615,
				  const struct imx615_mode *mode)
{
	int ret;

	ret = __v4l2_ctrl_s_ctrl(imx615->link_freq_ctrl, mode->link_freq_idx);
	if (ret)
		return ret;

	ret = __v4l2_ctrl_s_ctrl_int64(imx615->pclk_ctrl, mode->pclk);
	if (ret)
		return ret;

	ret = __v4l2_ctrl_modify_range(imx615->hblank_ctrl, mode->hblank,
				       mode->hblank, 1, mode->hblank);
	if (ret)
		return ret;

	ret = __v4l2_ctrl_modify_range(imx615->vblank_ctrl, mode->vblank_min,
				       mode->vblank_max, 1, mode->vblank);
	if (ret)
		return ret;

	ret = __v4l2_ctrl_s_ctrl(imx615->vblank_ctrl, mode->vblank);
	if (ret)
		return ret;

	/* The height may change even when the VBLANK value stays the same. */
	return __v4l2_ctrl_modify_range(imx615->exp_ctrl, IMX615_EXPOSURE_MIN,
			mode->height + mode->vblank - IMX615_EXPOSURE_OFFSET,
			1, IMX615_EXPOSURE_DEFAULT);
}

static int imx615_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct imx615 *imx615 =
		container_of(ctrl->handler, struct imx615, ctrl_handler);
	u32 lpfr = imx615->cur_mode->height + imx615->vblank_ctrl->val;
	int ret, release_ret;

	if (ctrl->id == V4L2_CID_VBLANK) {
		ret = __v4l2_ctrl_modify_range(imx615->exp_ctrl,
				IMX615_EXPOSURE_MIN, lpfr - IMX615_EXPOSURE_OFFSET,
				1, min_t(u32, IMX615_EXPOSURE_DEFAULT,
					 lpfr - IMX615_EXPOSURE_OFFSET));
		if (ret)
			return ret;
	}

	ret = pm_runtime_get_if_in_use(imx615->dev);
	if (ret <= 0)
		return ret;

	if (ctrl->id == V4L2_CID_TEST_PATTERN) {
		ret = imx615_write_reg(imx615, IMX615_REG_TEST_PATTERN, 1,
				       ctrl->val ? 2 : 0);
		goto out_pm;
	}

	/* Frame length, exposure and gain must take effect in the same frame. */
	ret = imx615_write_reg(imx615, IMX615_REG_HOLD, 1, 1);
	if (ret)
		goto out_pm;

	switch (ctrl->id) {
	case V4L2_CID_VBLANK:
		ret = imx615_write_reg(imx615, IMX615_REG_LPFR, 2, lpfr);
		if (ret)
			break;
		fallthrough;
	case V4L2_CID_EXPOSURE:
		/* Exposure is the master of the exposure/analogue gain cluster. */
		ret = imx615_write_reg(imx615, IMX615_REG_EXPOSURE_CIT, 2,
				       imx615->exp_ctrl->val);
		if (!ret)
			ret = imx615_write_reg(imx615, IMX615_REG_AGAIN, 2,
					       imx615->again_ctrl->val);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	release_ret = imx615_write_reg(imx615, IMX615_REG_HOLD, 1, 0);
	if (!ret)
		ret = release_ret;
out_pm:
	pm_runtime_put(imx615->dev);
	return ret;
}

static const struct v4l2_ctrl_ops imx615_ctrl_ops = {
	.s_ctrl = imx615_set_ctrl,
};

static int imx615_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index > 0)
		return -EINVAL;

	code->code = supported_modes[0].code;

	return 0;
}

static int imx615_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	if (fse->index >= ARRAY_SIZE(supported_modes))
		return -EINVAL;

	if (fse->code != supported_modes[fse->index].code)
		return -EINVAL;

	fse->min_width = supported_modes[fse->index].width;
	fse->max_width = fse->min_width;
	fse->min_height = supported_modes[fse->index].height;
	fse->max_height = fse->min_height;

	return 0;
}

static void imx615_fill_pad_format(struct imx615 *imx615,
				   const struct imx615_mode *mode,
				   struct v4l2_subdev_format *fmt)
{
	fmt->format.width = mode->width;
	fmt->format.height = mode->height;
	fmt->format.code = mode->code;
	fmt->format.field = V4L2_FIELD_NONE;
	fmt->format.colorspace = V4L2_COLORSPACE_RAW;
	fmt->format.ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	fmt->format.quantization = V4L2_QUANTIZATION_DEFAULT;
	fmt->format.xfer_func = V4L2_XFER_FUNC_NONE;
}

static int imx615_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *sd_state)
{
	struct imx615 *imx615 = to_imx615(sd);
	struct v4l2_subdev_format fmt = { 0 };

	fmt.which = sd_state ? V4L2_SUBDEV_FORMAT_TRY : V4L2_SUBDEV_FORMAT_ACTIVE;
	imx615_fill_pad_format(imx615, &supported_modes[0], &fmt);
	*v4l2_subdev_state_get_format(sd_state, 0) = fmt.format;

	return 0;
}

static int imx615_get_pad_format(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_format *fmt)
{
	struct imx615 *imx615 = to_imx615(sd);

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
		struct v4l2_mbus_framefmt *try_fmt =
			v4l2_subdev_state_get_format(sd_state, fmt->pad);

		fmt->format = *try_fmt;
	} else {
		imx615_fill_pad_format(imx615, imx615->cur_mode, fmt);
	}

	return 0;
}

static int imx615_set_pad_format(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_format *fmt)
{
	struct imx615 *imx615 = to_imx615(sd);
	const struct imx615_mode *mode, *old_mode;
	int ret = 0;

	mutex_lock(&imx615->mutex);

	mode = v4l2_find_nearest_size(supported_modes,
				      ARRAY_SIZE(supported_modes),
				      width, height,
				      fmt->format.width, fmt->format.height);

	imx615_fill_pad_format(imx615, mode, fmt);

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
		struct v4l2_mbus_framefmt *try_fmt =
			v4l2_subdev_state_get_format(sd_state, fmt->pad);

		*try_fmt = fmt->format;
	} else {
		if (imx615->streaming) {
			ret = -EBUSY;
			goto unlock;
		}
		old_mode = imx615->cur_mode;
		imx615->cur_mode = mode;
		ret = imx615_update_controls(imx615, mode);
		if (ret) {
			imx615->cur_mode = old_mode;
			imx615_update_controls(imx615, old_mode);
		}
	}

unlock:
	mutex_unlock(&imx615->mutex);

	return ret;
}

static int imx615_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *sd_state,
				struct v4l2_subdev_selection *sel)
{
	struct imx615 *imx615 = to_imx615(sd);
	const struct imx615_mode *mode;
	struct v4l2_mbus_framefmt *fmt;

	if (sel->pad)
		return -EINVAL;

	switch (sel->target) {
	case V4L2_SEL_TGT_NATIVE_SIZE:
	case V4L2_SEL_TGT_CROP_BOUNDS:
	case V4L2_SEL_TGT_CROP_DEFAULT:
		sel->r = (struct v4l2_rect) {
			.width = IMX615_PIXEL_ARRAY_WIDTH,
			.height = IMX615_PIXEL_ARRAY_HEIGHT,
		};
		return 0;
	case V4L2_SEL_TGT_CROP:
		mutex_lock(&imx615->mutex);
		if (sel->which == V4L2_SUBDEV_FORMAT_TRY) {
			fmt = v4l2_subdev_state_get_format(sd_state, sel->pad);
			mode = v4l2_find_nearest_size(supported_modes,
					ARRAY_SIZE(supported_modes), width, height,
					fmt->width, fmt->height);
		} else {
			mode = imx615->cur_mode;
		}
		/* Both modes use a centred crop and 2x2 binning. */
		sel->r.width = mode->width * 2;
		sel->r.height = mode->height * 2;
		sel->r.left = (IMX615_PIXEL_ARRAY_WIDTH - sel->r.width) / 2;
		sel->r.top = (IMX615_PIXEL_ARRAY_HEIGHT - sel->r.height) / 2;
		mutex_unlock(&imx615->mutex);
		return 0;
	default:
		return -EINVAL;
	}
}

static int imx615_start_streaming(struct imx615 *imx615)
{
	const struct imx615_reg_list *reg_list;
	int ret;

	/* Write generic 238 tuning registers from OnePlus downstream */
	ret = imx615_write_regs(imx615, imx615_init_regs,
				ARRAY_SIZE(imx615_init_regs));
	if (ret) {
		dev_err(imx615->dev, "failed to set init registers: %d\n", ret);
		return ret;
	}

	if (imx615->fab_id == IMX615_FAB_ID_2) {
		ret = imx615_write_regs(imx615, imx615_fab2_init_regs,
					ARRAY_SIZE(imx615_fab2_init_regs));
		if (ret)
			return ret;
	}

	/* Apply mode registers */
	reg_list = &imx615->cur_mode->reg_list;
	ret = imx615_write_regs(imx615, reg_list->regs, reg_list->num_of_regs);
	if (ret) {
		dev_err(imx615->dev, "failed to set mode registers: %d\n", ret);
		return ret;
	}

	/* Set V4L2 controls */
	ret = __v4l2_ctrl_handler_setup(&imx615->ctrl_handler);
	if (ret)
		return ret;

	/* Start streaming */
	ret = imx615_write_reg(imx615, IMX615_REG_MODE_SELECT, 1,
			       IMX615_MODE_STREAMING);
	if (ret) {
		dev_err(imx615->dev, "failed to start streaming: %d\n", ret);
		return ret;
	}

	/* Sensor stabilization delay */
	usleep_range(10000, 12000);

	return 0;
}

static int imx615_stop_streaming(struct imx615 *imx615)
{
	return imx615_write_reg(imx615, IMX615_REG_MODE_SELECT,
				1, IMX615_MODE_STANDBY);
}

static int imx615_set_stream(struct v4l2_subdev *sd, int enable)
{
	struct imx615 *imx615 = to_imx615(sd);
	int ret = 0;

	mutex_lock(&imx615->mutex);
	if (imx615->streaming == !!enable)
		goto unlock;

	if (enable) {
		ret = pm_runtime_resume_and_get(imx615->dev);
		if (ret)
			goto unlock;
		ret = imx615_start_streaming(imx615);
		if (ret) {
			imx615_stop_streaming(imx615);
			pm_runtime_put(imx615->dev);
			goto unlock;
		}
	} else {
		ret = imx615_stop_streaming(imx615);
		/* Release the streaming reference even if the I2C write fails. */
		pm_runtime_put(imx615->dev);
	}
	imx615->streaming = !!enable;
unlock:
	mutex_unlock(&imx615->mutex);
	return ret;
}

static int imx615_read_fab_id(struct imx615 *imx615)
{
	u32 old_page, old_mode, status, value;
	int ret, read_ret, restore_ret;

	ret = imx615_read_reg(imx615, IMX615_REG_OTP_PAGE, 1, &old_page);
	if (ret)
		return ret;
	ret = imx615_read_reg(imx615, IMX615_REG_OTP_MODE, 1, &old_mode);
	if (ret)
		return ret;

	ret = imx615_write_reg(imx615, IMX615_REG_OTP_PAGE, 1,
			       IMX615_OTP_FAB_ID_PAGE);
	if (ret)
		goto restore_page;
	ret = imx615_write_reg(imx615, IMX615_REG_OTP_MODE, 1,
			       IMX615_OTP_READ_ENABLE);
	if (ret)
		goto restore_mode;

	ret = read_poll_timeout(imx615_read_reg, read_ret,
				read_ret || status == IMX615_OTP_READ_READY,
				1000, 20000, false, imx615,
				IMX615_REG_OTP_STATUS, 1, &status);
	if (!ret)
		ret = read_ret;
	if (!ret) {
		ret = imx615_read_reg(imx615, IMX615_REG_OTP_FAB_ID, 1, &value);
		if (!ret)
			imx615->fab_id = value;
	}

restore_mode:
	restore_ret = imx615_write_reg(imx615, IMX615_REG_OTP_MODE, 1, old_mode);
	if (!ret)
		ret = restore_ret;
restore_page:
	restore_ret = imx615_write_reg(imx615, IMX615_REG_OTP_PAGE, 1, old_page);
	if (!ret)
		ret = restore_ret;

	return ret;
}

static int imx615_detect(struct imx615 *imx615)
{
	int ret;
	u32 val = 0;

	ret = imx615_read_reg(imx615, IMX615_REG_ID, 2, &val);

	if (ret) {
		dev_err(imx615->dev, "failed to read sensor ID from 0x%04x: %d\n",
			IMX615_REG_ID, ret);
		return ret;
	}

	if (val != IMX615_ID) {
		dev_err(imx615->dev, "chip id mismatch: 0x%x!=0x%x\n",
			val, IMX615_ID);
		return -ENODEV;
	}

	ret = imx615_read_fab_id(imx615);
	if (ret)
		return dev_err_probe(imx615->dev, ret, "failed to read factory ID\n");

	dev_info(imx615->dev,
		 "Sony IMX615 sensor identified successfully (id 0x%04x, fab 0x%02x)\n",
		 val, imx615->fab_id);
	return 0;
}

static int imx615_parse_hw_config(struct imx615 *imx615)
{
	struct fwnode_handle *fwnode = dev_fwnode(imx615->dev);
	struct v4l2_fwnode_endpoint bus_cfg = {
		.bus_type = V4L2_MBUS_CSI2_DPHY
	};
	struct fwnode_handle *ep;
	unsigned long rate;
	unsigned int i;
	int ret;

	if (!fwnode)
		return -ENXIO;

	imx615->reset_gpio = devm_gpiod_get_optional(imx615->dev, "reset",
						     GPIOD_OUT_HIGH);
	if (IS_ERR(imx615->reset_gpio)) {
		dev_err(imx615->dev, "failed to get reset gpio %pe\n",
			imx615->reset_gpio);
		return PTR_ERR(imx615->reset_gpio);
	}

	imx615->inclk = devm_v4l2_sensor_clk_get(imx615->dev, NULL);
	if (IS_ERR(imx615->inclk))
		return dev_err_probe(imx615->dev, PTR_ERR(imx615->inclk),
				     "could not get inclk\n");

	rate = clk_get_rate(imx615->inclk);
	if (rate != IMX615_INCLK_RATE) {
		dev_err(imx615->dev, "inclk frequency mismatch: expected %d, got %lu\n",
			IMX615_INCLK_RATE, rate);
		return -EINVAL;
	}

	for (i = 0; i < ARRAY_SIZE(imx615_supply_names); i++)
		imx615->supplies[i].supply = imx615_supply_names[i];

	ret = devm_regulator_bulk_get(imx615->dev,
				      ARRAY_SIZE(imx615_supply_names),
				      imx615->supplies);
	if (ret)
		return ret;

	ep = fwnode_graph_get_next_endpoint(fwnode, NULL);
	if (!ep)
		return -ENXIO;

	ret = v4l2_fwnode_endpoint_alloc_parse(ep, &bus_cfg);
	fwnode_handle_put(ep);
	if (ret)
		return ret;

	if (bus_cfg.bus.mipi_csi2.num_data_lanes != IMX615_NUM_DATA_LANES) {
		dev_err(imx615->dev,
			"number of CSI2 data lanes %d is not supported\n",
			bus_cfg.bus.mipi_csi2.num_data_lanes);
		ret = -EINVAL;
		goto done_endpoint_free;
	}

	if (!bus_cfg.nr_of_link_frequencies) {
		dev_err(imx615->dev, "no link frequencies defined\n");
		ret = -EINVAL;
		goto done_endpoint_free;
	}

	for (i = 0; i < bus_cfg.nr_of_link_frequencies; i++) {
		if (bus_cfg.link_frequencies[i] == IMX615_LINK_FREQ)
			goto done_endpoint_free;
	}

	ret = -EINVAL;

done_endpoint_free:
	v4l2_fwnode_endpoint_free(&bus_cfg);

	return ret;
}

static const struct v4l2_subdev_video_ops imx615_video_ops = {
	.s_stream = imx615_set_stream,
};

static const struct v4l2_subdev_pad_ops imx615_pad_ops = {
	.enum_mbus_code = imx615_enum_mbus_code,
	.enum_frame_size = imx615_enum_frame_size,
	.get_fmt = imx615_get_pad_format,
	.set_fmt = imx615_set_pad_format,
	.get_selection = imx615_get_selection,
};

static const struct v4l2_subdev_ops imx615_subdev_ops = {
	.video = &imx615_video_ops,
	.pad = &imx615_pad_ops,
};

static const struct v4l2_subdev_internal_ops imx615_internal_ops = {
	.init_state = imx615_init_state,
};

static int imx615_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx615 *imx615 = to_imx615(sd);
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(imx615_supply_names),
				    imx615->supplies);
	if (ret < 0) {
		dev_err(dev, "failed to enable regulators: %d\n", ret);
		return ret;
	}

	/* Keep reset asserted while turning on clock */
	gpiod_set_value_cansleep(imx615->reset_gpio, 1);

	ret = clk_prepare_enable(imx615->inclk);
	if (ret) {
		dev_err(imx615->dev, "fail to enable inclk: %d\n", ret);
		goto error_regulators;
	}

	/* Allow MCLK to stabilize (t0 >= 0.5us) */
	usleep_range(2000, 3000);

	/* Release reset (XCLR high) */
	gpiod_set_value_cansleep(imx615->reset_gpio, 0);

	/* Wait for sensor boot and internal initialization (t4 >= 10ms) */
	usleep_range(12000, 15000);

	return 0;

error_regulators:
	regulator_bulk_disable(ARRAY_SIZE(imx615_supply_names),
			       imx615->supplies);

	return ret;
}

static int imx615_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx615 *imx615 = to_imx615(sd);

	/* Assert reset (XCLR low) */
	gpiod_set_value_cansleep(imx615->reset_gpio, 1);
	usleep_range(1000, 2000);

	clk_disable_unprepare(imx615->inclk);

	regulator_bulk_disable(ARRAY_SIZE(imx615_supply_names),
			       imx615->supplies);

	return 0;
}

static int imx615_init_controls(struct imx615 *imx615)
{
	struct v4l2_ctrl_handler *ctrl_hdlr = &imx615->ctrl_handler;
	const struct imx615_mode *mode = imx615->cur_mode;
	struct v4l2_fwnode_device_properties props;
	u32 lpfr;
	int ret;

	ret = v4l2_ctrl_handler_init(ctrl_hdlr, 7);
	if (ret)
		return ret;

	ctrl_hdlr->lock = &imx615->mutex;

	lpfr = mode->vblank + mode->height;
	imx615->exp_ctrl = v4l2_ctrl_new_std(ctrl_hdlr,
					     &imx615_ctrl_ops,
					     V4L2_CID_EXPOSURE,
					     IMX615_EXPOSURE_MIN,
					     lpfr - IMX615_EXPOSURE_OFFSET,
					     IMX615_EXPOSURE_STEP,
					     IMX615_EXPOSURE_DEFAULT);

	imx615->again_ctrl = v4l2_ctrl_new_std(ctrl_hdlr,
					       &imx615_ctrl_ops,
					       V4L2_CID_ANALOGUE_GAIN,
					       IMX615_AGAIN_MIN,
					       IMX615_AGAIN_MAX,
					       IMX615_AGAIN_STEP,
					       IMX615_AGAIN_DEFAULT);

	v4l2_ctrl_cluster(2, &imx615->exp_ctrl);

	v4l2_ctrl_new_std_menu_items(ctrl_hdlr, &imx615_ctrl_ops,
				     V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(imx615_test_pattern_menu) - 1,
				     0, 0, imx615_test_pattern_menu);

	imx615->vblank_ctrl = v4l2_ctrl_new_std(ctrl_hdlr,
						&imx615_ctrl_ops,
						V4L2_CID_VBLANK,
						mode->vblank_min,
						mode->vblank_max,
						1, mode->vblank);

	imx615->pclk_ctrl = v4l2_ctrl_new_std(ctrl_hdlr,
					      &imx615_ctrl_ops,
					      V4L2_CID_PIXEL_RATE,
					      mode->pclk, mode->pclk,
					      1, mode->pclk);

	imx615->hblank_ctrl = v4l2_ctrl_new_std(ctrl_hdlr,
						&imx615_ctrl_ops,
						V4L2_CID_HBLANK,
						mode->hblank, mode->hblank,
						1, mode->hblank);

	if (imx615->hblank_ctrl)
		imx615->hblank_ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	imx615->link_freq_ctrl =
		v4l2_ctrl_new_int_menu(ctrl_hdlr,
				       &imx615_ctrl_ops,
				       V4L2_CID_LINK_FREQ,
				       ARRAY_SIZE(link_freq) - 1,
				       mode->link_freq_idx,
				       link_freq);

	if (imx615->link_freq_ctrl)
		imx615->link_freq_ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	ret = ctrl_hdlr->error;
	if (ret)
		goto error;

	ret = v4l2_fwnode_device_parse(imx615->dev, &props);
	if (ret)
		goto error;

	ret = v4l2_ctrl_new_fwnode_properties(ctrl_hdlr, &imx615_ctrl_ops,
					      &props);
	if (ret)
		goto error;

	return 0;

error:
	v4l2_ctrl_handler_free(ctrl_hdlr);
	return ret;
}

static int imx615_probe(struct i2c_client *client)
{
	struct imx615 *imx615;
	const char *name = "imx615";
	int ret;

	imx615 = devm_kzalloc(&client->dev, sizeof(*imx615), GFP_KERNEL);
	if (!imx615)
		return -ENOMEM;

	imx615->dev = &client->dev;

	v4l2_i2c_subdev_init(&imx615->sd, client, &imx615_subdev_ops);
	imx615->sd.internal_ops = &imx615_internal_ops;

	ret = imx615_parse_hw_config(imx615);
	if (ret) {
		dev_err(imx615->dev, "HW configuration is not supported: %d\n", ret);
		return ret;
	}

	mutex_init(&imx615->mutex);

	ret = imx615_power_on(imx615->dev);
	if (ret) {
		dev_err(imx615->dev, "failed to power-on the sensor\n");
		goto error_mutex_destroy;
	}

	/* Check sensor module identity (0x0615) */
	ret = imx615_detect(imx615);
	if (ret) {
		dev_err(imx615->dev, "failed to find sensor: %d\n", ret);
		goto error_power_off;
	}

	/* Set default mode */
	imx615->cur_mode = &supported_modes[0];

	ret = imx615_init_controls(imx615);
	if (ret) {
		dev_err(imx615->dev, "failed to init controls: %d\n", ret);
		goto error_power_off;
	}

	imx615->sd.ctrl_handler = &imx615->ctrl_handler;
	imx615->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	imx615->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	v4l2_i2c_subdev_set_name(&imx615->sd, client, name, NULL);

	imx615->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&imx615->sd.entity, 1, &imx615->pad);
	if (ret) {
		dev_err(imx615->dev, "failed to init entity pads: %d\n", ret);
		goto error_handler_free;
	}

	pm_runtime_set_active(imx615->dev);
	pm_runtime_get_noresume(imx615->dev);
	pm_runtime_enable(imx615->dev);

	ret = v4l2_async_register_subdev_sensor(&imx615->sd);
	if (ret < 0) {
		dev_err(imx615->dev,
			"failed to register async subdev: %d\n", ret);
		goto error_media_entity;
	}

	pm_runtime_put(imx615->dev);

	return 0;

error_media_entity:
	pm_runtime_disable(imx615->dev);
	pm_runtime_put_noidle(imx615->dev);
	pm_runtime_set_suspended(imx615->dev);
	media_entity_cleanup(&imx615->sd.entity);
error_handler_free:
	v4l2_ctrl_handler_free(imx615->sd.ctrl_handler);
error_power_off:
	imx615_power_off(imx615->dev);
error_mutex_destroy:
	mutex_destroy(&imx615->mutex);

	return ret;
}

static void imx615_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx615 *imx615 = to_imx615(sd);

	v4l2_async_unregister_subdev(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(sd->ctrl_handler);

	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		imx615_power_off(&client->dev);
	pm_runtime_set_suspended(&client->dev);

	mutex_destroy(&imx615->mutex);
}

static const struct dev_pm_ops imx615_pm_ops = {
	SET_RUNTIME_PM_OPS(imx615_power_off, imx615_power_on, NULL)
};

static const struct of_device_id imx615_of_match[] = {
	{ .compatible = "sony,imx615" },
	{ }
};
MODULE_DEVICE_TABLE(of, imx615_of_match);

static struct i2c_driver imx615_driver = {
	.probe = imx615_probe,
	.remove = imx615_remove,
	.driver = {
		.name = "imx615",
		.pm = &imx615_pm_ops,
		.of_match_table = imx615_of_match,
	},
};

module_i2c_driver(imx615_driver);

MODULE_DESCRIPTION("Sony IMX615 sensor driver");
MODULE_AUTHOR("OnePlus 12 Mainline Porting Team");
MODULE_LICENSE("GPL");
