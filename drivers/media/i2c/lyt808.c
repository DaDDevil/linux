// SPDX-License-Identifier: GPL-2.0
/*
 * Sony LYT808 Camera Sensor Driver
 *
 * Copyright (C) 2024 Linaro Ltd.
 * Copyright (C) 2026 OnePlus / OPPO open source port
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>

#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

/* Streaming Mode */
#define LYT808_REG_MODE_SELECT		0x0100
#define LYT808_MODE_STANDBY		0x00
#define LYT808_MODE_STREAMING		0x01

/* Lines per frame (FLL) */
#define LYT808_REG_LPFR			0x0340

/* Chip ID */
#define LYT808_REG_ID			0x0016
#define LYT808_ID			0x9186

/* Exposure control */
#define LYT808_REG_EXPOSURE_CIT		0x0202
#define LYT808_EXPOSURE_MIN		16
#define LYT808_EXPOSURE_OFFSET		48
#define LYT808_EXPOSURE_STEP		1
#define LYT808_EXPOSURE_DEFAULT		0x0648

/* Analog gain control */
#define LYT808_REG_AGAIN		0x0204
#define LYT808_AGAIN_MIN		0
#define LYT808_AGAIN_MAX		16128
#define LYT808_AGAIN_STEP		1
#define LYT808_AGAIN_DEFAULT		0

/* Group hold register */
#define LYT808_REG_HOLD			0x0104

#define LYT808_REG_TEST_PATTERN		0x0601

/* Input clock rate */
#define LYT808_INCLK_RATE		19200000

/* CSI2 HW configuration */
#define LYT808_LINK_FREQ		2412800000LL
#define LYT808_NUM_DATA_LANES		3

#define LYT808_PIXEL_ARRAY_WIDTH		8192
#define LYT808_PIXEL_ARRAY_HEIGHT		6144

struct lyt808_reg_list {
	u32 num_of_regs;
	const struct cci_reg_sequence *regs;
};

struct lyt808_mode {
	u32 width;
	u32 height;
	u32 code;
	u32 hblank;
	u32 vblank;
	u32 vblank_min;
	u32 vblank_max;
	u64 pclk;
	u32 link_freq_idx;
	struct lyt808_reg_list reg_list;
};

static const char * const lyt808_supply_names[] = {
	"dovdd",
	"auxvdd",
	"avdd",
	"dvdd",
	"afvdd",
};

struct lyt808 {
	struct device *dev;
	struct regmap *regmap;
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct gpio_desc *reset_gpio;
	struct clk *inclk;
	struct regulator_bulk_data supplies[ARRAY_SIZE(lyt808_supply_names)];
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
	const struct lyt808_mode *cur_mode;
	struct mutex mutex;
};

static const s64 link_freq[] = {
	LYT808_LINK_FREQ,
};

static const char * const lyt808_test_pattern_menu[] = {
	"Disabled",
	"Color Bars",
};

/* OEM tuning, with the PHY interface selected for four-lane D-PHY. */
static const struct cci_reg_sequence lyt808_init_regs[] = {
	{ CCI_REG8(0x0136), 0x13 },
	{ CCI_REG8(0x0137), 0x33 },
	{ CCI_REG8(0xf800), 0x19 },
	{ CCI_REG8(0xf801), 0x19 },
	{ CCI_REG8(0xf802), 0x92 },
	{ CCI_REG8(0xf803), 0xed },
	{ CCI_REG8(0xf804), 0x55 },
	{ CCI_REG8(0xf805), 0x6c },
	{ CCI_REG8(0xf806), 0x92 },
	{ CCI_REG8(0xf807), 0x00 },
	{ CCI_REG8(0xf808), 0x00 },
	{ CCI_REG8(0xf809), 0x00 },
	{ CCI_REG8(0xf80a), 0x10 },
	{ CCI_REG8(0xf80b), 0x08 },
	{ CCI_REG8(0xf80c), 0x55 },
	{ CCI_REG8(0xf80d), 0x70 },
	{ CCI_REG8(0xf80e), 0x92 },
	{ CCI_REG8(0xf80f), 0x00 },
	{ CCI_REG8(0xf810), 0xc6 },
	{ CCI_REG8(0xf811), 0x6e },
	{ CCI_REG8(0xf812), 0x17 },
	{ CCI_REG8(0xf813), 0xfa },
	{ CCI_REG8(0xf814), 0x55 },
	{ CCI_REG8(0xf815), 0x0c },
	{ CCI_REG8(0xf816), 0x95 },
	{ CCI_REG8(0xf817), 0x00 },
	{ CCI_REG8(0xf818), 0x00 },
	{ CCI_REG8(0xf819), 0x00 },
	{ CCI_REG8(0xf81a), 0xbe },
	{ CCI_REG8(0xf81b), 0x19 },
	{ CCI_REG8(0xf81c), 0x55 },
	{ CCI_REG8(0xf81d), 0x10 },
	{ CCI_REG8(0xf81e), 0x95 },
	{ CCI_REG8(0xf81f), 0x00 },
	{ CCI_REG8(0xf820), 0x17 },
	{ CCI_REG8(0xf821), 0xfa },
	{ CCI_REG8(0xf822), 0x00 },
	{ CCI_REG8(0xf823), 0x00 },
	{ CCI_REG8(0xf824), 0x55 },
	{ CCI_REG8(0xf825), 0x14 },
	{ CCI_REG8(0xf826), 0x95 },
	{ CCI_REG8(0xf827), 0x00 },
	{ CCI_REG8(0xf828), 0x17 },
	{ CCI_REG8(0xf829), 0xa8 },
	{ CCI_REG8(0xf82a), 0x72 },
	{ CCI_REG8(0xf82b), 0x6b },
	{ CCI_REG8(0xf82c), 0x55 },
	{ CCI_REG8(0xf82d), 0x7c },
	{ CCI_REG8(0xf82e), 0x92 },
	{ CCI_REG8(0xf82f), 0x00 },
	{ CCI_REG8(0xf830), 0x60 },
	{ CCI_REG8(0xf831), 0x6f },
	{ CCI_REG8(0xf832), 0x17 },
	{ CCI_REG8(0xf833), 0xfa },
	{ CCI_REG8(0xf834), 0x55 },
	{ CCI_REG8(0xf835), 0x10 },
	{ CCI_REG8(0xf836), 0x94 },
	{ CCI_REG8(0xf837), 0x00 },
	{ CCI_REG8(0xf838), 0x17 },
	{ CCI_REG8(0xf839), 0xfa },
	{ CCI_REG8(0xf83a), 0x17 },
	{ CCI_REG8(0xf83b), 0xac },
	{ CCI_REG8(0xf83c), 0x55 },
	{ CCI_REG8(0xf83d), 0x14 },
	{ CCI_REG8(0xf83e), 0x94 },
	{ CCI_REG8(0xf83f), 0x00 },
	{ CCI_REG8(0xf840), 0x17 },
	{ CCI_REG8(0xf841), 0xa8 },
	{ CCI_REG8(0xf842), 0xe0 },
	{ CCI_REG8(0xf843), 0x6d },
	{ CCI_REG8(0xf884), 0x4f },
	{ CCI_REG8(0xf885), 0x86 },
	{ CCI_REG8(0xf886), 0xac },
	{ CCI_REG8(0xf887), 0x17 },
	{ CCI_REG8(0xf888), 0xfa },
	{ CCI_REG8(0xf889), 0xe8 },
	{ CCI_REG8(0xf88a), 0xae },
	{ CCI_REG8(0xf88b), 0x40 },
	{ CCI_REG8(0xf88c), 0xd1 },
	{ CCI_REG8(0xf88d), 0x18 },
	{ CCI_REG8(0xf88e), 0x08 },
	{ CCI_REG8(0xf88f), 0x00 },
	{ CCI_REG8(0xf890), 0xf7 },
	{ CCI_REG8(0xf891), 0x10 },
	{ CCI_REG8(0xf892), 0x07 },
	{ CCI_REG8(0xf893), 0x0e },
	{ CCI_REG8(0xf894), 0xf0 },
	{ CCI_REG8(0xf895), 0x10 },
	{ CCI_REG8(0xf896), 0x06 },
	{ CCI_REG8(0xf897), 0xd8 },
	{ CCI_REG8(0xf898), 0xf4 },
	{ CCI_REG8(0xf899), 0x10 },
	{ CCI_REG8(0xf89a), 0x06 },
	{ CCI_REG8(0xf89b), 0xdc },
	{ CCI_REG8(0xf89c), 0xa8 },
	{ CCI_REG8(0xf89d), 0x17 },
	{ CCI_REG8(0xf89e), 0xa0 },
	{ CCI_REG8(0xf89f), 0x05 },
	{ CCI_REG8(0xf8a0), 0x4f },
	{ CCI_REG8(0xf8a1), 0x06 },
	{ CCI_REG8(0xf8a2), 0xac },
	{ CCI_REG8(0xf8a3), 0x27 },
	{ CCI_REG8(0xf8a4), 0xac },
	{ CCI_REG8(0xf8a5), 0x1d },
	{ CCI_REG8(0xf8a6), 0xac },
	{ CCI_REG8(0xf8a7), 0x14 },
	{ CCI_REG8(0xf8a8), 0xa8 },
	{ CCI_REG8(0xf8a9), 0x35 },
	{ CCI_REG8(0xf8aa), 0xa8 },
	{ CCI_REG8(0xf8ab), 0x2c },
	{ CCI_REG8(0xf8ac), 0x28 },
	{ CCI_REG8(0xf8ad), 0x04 },
	{ CCI_REG8(0xf8ae), 0x90 },
	{ CCI_REG8(0xf8af), 0x14 },
	{ CCI_REG8(0xf8b0), 0x01 },
	{ CCI_REG8(0xf8b1), 0x20 },
	{ CCI_REG8(0xf8b2), 0x10 },
	{ CCI_REG8(0xf8b3), 0x11 },
	{ CCI_REG8(0xf8b4), 0x01 },
	{ CCI_REG8(0xf8b5), 0x01 },
	{ CCI_REG8(0xf8b6), 0x0f },
	{ CCI_REG8(0xf8b7), 0x31 },
	{ CCI_REG8(0xf8b8), 0x00 },
	{ CCI_REG8(0xf8b9), 0x10 },
	{ CCI_REG8(0xf8ba), 0x00 },
	{ CCI_REG8(0xf8bb), 0x41 },
	{ CCI_REG8(0xf8bc), 0xfa },
	{ CCI_REG8(0xf8bd), 0xe8 },
	{ CCI_REG8(0xf8be), 0x09 },
	{ CCI_REG8(0xf8bf), 0x14 },
	{ CCI_REG8(0xf8c0), 0x80 },
	{ CCI_REG8(0xf8c1), 0x04 },
	{ CCI_REG8(0xf8c2), 0x5f },
	{ CCI_REG8(0xf8c3), 0xf0 },
	{ CCI_REG8(0xf8c4), 0x01 },
	{ CCI_REG8(0xf8c5), 0x50 },
	{ CCI_REG8(0xf8c6), 0xfc },
	{ CCI_REG8(0xf8c7), 0x41 },
	{ CCI_REG8(0xf8c8), 0x01 },
	{ CCI_REG8(0xf8c9), 0xff },
	{ CCI_REG8(0xf8ca), 0xfc },
	{ CCI_REG8(0xf8cb), 0x31 },
	{ CCI_REG8(0xf8cc), 0xff },
	{ CCI_REG8(0xf8cd), 0xfc },
	{ CCI_REG8(0xf8ce), 0x22 },
	{ CCI_REG8(0xf8cf), 0x01 },
	{ CCI_REG8(0xf8d0), 0xa5 },
	{ CCI_REG8(0xf8d1), 0x01 },
	{ CCI_REG8(0xf8d2), 0x40 },
	{ CCI_REG8(0xf8d3), 0x31 },
	{ CCI_REG8(0xf8d4), 0x5f },
	{ CCI_REG8(0xf8d5), 0xc0 },
	{ CCI_REG8(0xf8d6), 0x04 },
	{ CCI_REG8(0xf8d7), 0x10 },
	{ CCI_REG8(0xf8d8), 0xa8 },
	{ CCI_REG8(0xf8d9), 0x14 },
	{ CCI_REG8(0xf8da), 0xa8 },
	{ CCI_REG8(0xf8db), 0x1d },
	{ CCI_REG8(0xf8dc), 0xa8 },
	{ CCI_REG8(0xf8dd), 0x27 },
	{ CCI_REG8(0xf8de), 0xa0 },
	{ CCI_REG8(0xf8df), 0x09 },
	{ CCI_REG8(0xf8e0), 0x4f },
	{ CCI_REG8(0xf8e1), 0x86 },
	{ CCI_REG8(0xf8e2), 0xac },
	{ CCI_REG8(0xf8e3), 0x14 },
	{ CCI_REG8(0xf8e4), 0xd0 },
	{ CCI_REG8(0xf8e5), 0x20 },
	{ CCI_REG8(0xf8e6), 0x44 },
	{ CCI_REG8(0xf8e7), 0x2c },
	{ CCI_REG8(0xf8e8), 0x18 },
	{ CCI_REG8(0xf8e9), 0x01 },
	{ CCI_REG8(0xf8ea), 0xd2 },
	{ CCI_REG8(0xf8eb), 0x20 },
	{ CCI_REG8(0xf8ec), 0x4e },
	{ CCI_REG8(0xf8ed), 0x78 },
	{ CCI_REG8(0xf8ee), 0x1e },
	{ CCI_REG8(0xf8ef), 0x22 },
	{ CCI_REG8(0xf8f0), 0xbf },
	{ CCI_REG8(0xf8f1), 0x20 },
	{ CCI_REG8(0xf8f2), 0xd2 },
	{ CCI_REG8(0xf8f3), 0x18 },
	{ CCI_REG8(0xf8f4), 0x08 },
	{ CCI_REG8(0xf8f5), 0x00 },
	{ CCI_REG8(0xf8f6), 0xf6 },
	{ CCI_REG8(0xf8f7), 0x23 },
	{ CCI_REG8(0xf8f8), 0x06 },
	{ CCI_REG8(0xf8f9), 0x9c },
	{ CCI_REG8(0xf8fa), 0x6f },
	{ CCI_REG8(0xf8fb), 0xf3 },
	{ CCI_REG8(0xf8fc), 0xbd },
	{ CCI_REG8(0xf8fd), 0x9a },
	{ CCI_REG8(0xf8fe), 0xf2 },
	{ CCI_REG8(0xf8ff), 0x23 },
	{ CCI_REG8(0xf900), 0x06 },
	{ CCI_REG8(0xf901), 0xd6 },
	{ CCI_REG8(0xf902), 0x00 },
	{ CCI_REG8(0xf903), 0x14 },
	{ CCI_REG8(0xf904), 0x4f },
	{ CCI_REG8(0xf905), 0x04 },
	{ CCI_REG8(0xf906), 0x02 },
	{ CCI_REG8(0xf907), 0x34 },
	{ CCI_REG8(0xf908), 0xf0 },
	{ CCI_REG8(0xf909), 0x22 },
	{ CCI_REG8(0xf90a), 0x06 },
	{ CCI_REG8(0xf90b), 0xdc },
	{ CCI_REG8(0xf90c), 0x25 },
	{ CCI_REG8(0xf90d), 0x42 },
	{ CCI_REG8(0xf90e), 0x90 },
	{ CCI_REG8(0xf90f), 0x22 },
	{ CCI_REG8(0xf910), 0x00 },
	{ CCI_REG8(0xf911), 0x34 },
	{ CCI_REG8(0xf912), 0x01 },
	{ CCI_REG8(0xf913), 0x14 },
	{ CCI_REG8(0xf914), 0x25 },
	{ CCI_REG8(0xf915), 0x24 },
	{ CCI_REG8(0xf916), 0x90 },
	{ CCI_REG8(0xf917), 0x1a },
	{ CCI_REG8(0xf918), 0x01 },
	{ CCI_REG8(0xf919), 0x32 },
	{ CCI_REG8(0xf91a), 0x41 },
	{ CCI_REG8(0xf91b), 0x02 },
	{ CCI_REG8(0xf91c), 0xfc },
	{ CCI_REG8(0xf91d), 0x41 },
	{ CCI_REG8(0xf91e), 0x01 },
	{ CCI_REG8(0xf91f), 0xff },
	{ CCI_REG8(0xf920), 0xfc },
	{ CCI_REG8(0xf921), 0x31 },
	{ CCI_REG8(0xf922), 0xff },
	{ CCI_REG8(0xf923), 0xfc },
	{ CCI_REG8(0xf924), 0x24 },
	{ CCI_REG8(0xf925), 0x21 },
	{ CCI_REG8(0xf926), 0xa4 },
	{ CCI_REG8(0xf927), 0x13 },
	{ CCI_REG8(0xf928), 0xa5 },
	{ CCI_REG8(0xf929), 0x23 },
	{ CCI_REG8(0xf92a), 0x40 },
	{ CCI_REG8(0xf92b), 0x33 },
	{ CCI_REG8(0xf92c), 0x5f },
	{ CCI_REG8(0xf92d), 0xc1 },
	{ CCI_REG8(0xf92e), 0x04 },
	{ CCI_REG8(0xf92f), 0x31 },
	{ CCI_REG8(0xf930), 0x1c },
	{ CCI_REG8(0xf931), 0x01 },
	{ CCI_REG8(0xf932), 0xa8 },
	{ CCI_REG8(0xf933), 0x14 },
	{ CCI_REG8(0xf934), 0xa0 },
	{ CCI_REG8(0xf935), 0x05 },
	{ CCI_REG8(0xf936), 0x4e },
	{ CCI_REG8(0xf937), 0x06 },
	{ CCI_REG8(0xf938), 0xb0 },
	{ CCI_REG8(0xf939), 0xa1 },
	{ CCI_REG8(0xf93a), 0xb0 },
	{ CCI_REG8(0xf93b), 0x9c },
	{ CCI_REG8(0xf93c), 0xac },
	{ CCI_REG8(0xf93d), 0x37 },
	{ CCI_REG8(0xf93e), 0xac },
	{ CCI_REG8(0xf93f), 0x2d },
	{ CCI_REG8(0xf940), 0xac },
	{ CCI_REG8(0xf941), 0x24 },
	{ CCI_REG8(0xf942), 0x00 },
	{ CCI_REG8(0xf943), 0x04 },
	{ CCI_REG8(0xf944), 0xfa },
	{ CCI_REG8(0xf945), 0xe8 },
	{ CCI_REG8(0xf946), 0xad },
	{ CCI_REG8(0xf947), 0x84 },
	{ CCI_REG8(0xf948), 0x6f },
	{ CCI_REG8(0xf949), 0xf0 },
	{ CCI_REG8(0xf94a), 0x50 },
	{ CCI_REG8(0xf94b), 0x71 },
	{ CCI_REG8(0xf94c), 0x0f },
	{ CCI_REG8(0xf94d), 0x01 },
	{ CCI_REG8(0xf94e), 0x10 },
	{ CCI_REG8(0xf94f), 0x21 },
	{ CCI_REG8(0xf950), 0xd0 },
	{ CCI_REG8(0xf951), 0x21 },
	{ CCI_REG8(0xf952), 0x40 },
	{ CCI_REG8(0xf953), 0x28 },
	{ CCI_REG8(0xf954), 0x1a },
	{ CCI_REG8(0xf955), 0x05 },
	{ CCI_REG8(0xf956), 0xd8 },
	{ CCI_REG8(0xf957), 0x18 },
	{ CCI_REG8(0xf958), 0x08 },
	{ CCI_REG8(0xf959), 0x00 },
	{ CCI_REG8(0xf95a), 0xf0 },
	{ CCI_REG8(0xf95b), 0x82 },
	{ CCI_REG8(0xf95c), 0x06 },
	{ CCI_REG8(0xf95d), 0x88 },
	{ CCI_REG8(0xf95e), 0x01 },
	{ CCI_REG8(0xf95f), 0x12 },
	{ CCI_REG8(0xf960), 0x3c },
	{ CCI_REG8(0xf961), 0x26 },
	{ CCI_REG8(0xf962), 0x1e },
	{ CCI_REG8(0xf963), 0x01 },
	{ CCI_REG8(0xf964), 0x3c },
	{ CCI_REG8(0xf965), 0x24 },
	{ CCI_REG8(0xf966), 0x1e },
	{ CCI_REG8(0xf967), 0x03 },
	{ CCI_REG8(0xf968), 0x3c },
	{ CCI_REG8(0xf969), 0x23 },
	{ CCI_REG8(0xf96a), 0x1e },
	{ CCI_REG8(0xf96b), 0x00 },
	{ CCI_REG8(0xf96c), 0x3d },
	{ CCI_REG8(0xf96d), 0x25 },
	{ CCI_REG8(0xf96e), 0x1e },
	{ CCI_REG8(0xf96f), 0x27 },
	{ CCI_REG8(0xf970), 0xd2 },
	{ CCI_REG8(0xf971), 0x21 },
	{ CCI_REG8(0xf972), 0x40 },
	{ CCI_REG8(0xf973), 0x32 },
	{ CCI_REG8(0xf974), 0x1a },
	{ CCI_REG8(0xf975), 0x22 },
	{ CCI_REG8(0xf976), 0xd9 },
	{ CCI_REG8(0xf977), 0x21 },
	{ CCI_REG8(0xf978), 0x40 },
	{ CCI_REG8(0xf979), 0x2e },
	{ CCI_REG8(0xf97a), 0x1a },
	{ CCI_REG8(0xf97b), 0x99 },
	{ CCI_REG8(0xf97c), 0xac },
	{ CCI_REG8(0xf97d), 0x14 },
	{ CCI_REG8(0xf97e), 0xac },
	{ CCI_REG8(0xf97f), 0x0f },
	{ CCI_REG8(0xf980), 0x40 },
	{ CCI_REG8(0xf981), 0x12 },
	{ CCI_REG8(0xf982), 0x02 },
	{ CCI_REG8(0xf983), 0x92 },
	{ CCI_REG8(0xf984), 0xfa },
	{ CCI_REG8(0xf985), 0xff },
	{ CCI_REG8(0xf986), 0xff },
	{ CCI_REG8(0xf987), 0x1c },
	{ CCI_REG8(0xf988), 0xf4 },
	{ CCI_REG8(0xf989), 0x80 },
	{ CCI_REG8(0xf98a), 0x06 },
	{ CCI_REG8(0xf98b), 0xd8 },
	{ CCI_REG8(0xf98c), 0xd1 },
	{ CCI_REG8(0xf98d), 0x21 },
	{ CCI_REG8(0xf98e), 0x42 },
	{ CCI_REG8(0xf98f), 0x90 },
	{ CCI_REG8(0xf990), 0x1e },
	{ CCI_REG8(0xf991), 0x11 },
	{ CCI_REG8(0xf992), 0xbe },
	{ CCI_REG8(0xf993), 0x9c },
	{ CCI_REG8(0xf994), 0xf6 },
	{ CCI_REG8(0xf995), 0x81 },
	{ CCI_REG8(0xf996), 0x06 },
	{ CCI_REG8(0xf997), 0x9c },
	{ CCI_REG8(0xf998), 0x6f },
	{ CCI_REG8(0xf999), 0xf1 },
	{ CCI_REG8(0xf99a), 0xbe },
	{ CCI_REG8(0xf99b), 0x98 },
	{ CCI_REG8(0xf99c), 0xf2 },
	{ CCI_REG8(0xf99d), 0x81 },
	{ CCI_REG8(0xf99e), 0x06 },
	{ CCI_REG8(0xf99f), 0xd6 },
	{ CCI_REG8(0xf9a0), 0x00 },
	{ CCI_REG8(0xf9a1), 0x52 },
	{ CCI_REG8(0xf9a2), 0x4f },
	{ CCI_REG8(0xf9a3), 0x02 },
	{ CCI_REG8(0xf9a4), 0x02 },
	{ CCI_REG8(0xf9a5), 0x12 },
	{ CCI_REG8(0xf9a6), 0x25 },
	{ CCI_REG8(0xf9a7), 0x20 },
	{ CCI_REG8(0xf9a8), 0x90 },
	{ CCI_REG8(0xf9a9), 0x22 },
	{ CCI_REG8(0xf9aa), 0x00 },
	{ CCI_REG8(0xf9ab), 0x12 },
	{ CCI_REG8(0xf9ac), 0x01 },
	{ CCI_REG8(0xf9ad), 0x52 },
	{ CCI_REG8(0xf9ae), 0x25 },
	{ CCI_REG8(0xf9af), 0x02 },
	{ CCI_REG8(0xf9b0), 0x90 },
	{ CCI_REG8(0xf9b1), 0x1a },
	{ CCI_REG8(0xf9b2), 0x01 },
	{ CCI_REG8(0xf9b3), 0x45 },
	{ CCI_REG8(0xf9b4), 0x02 },
	{ CCI_REG8(0xf9b5), 0x05 },
	{ CCI_REG8(0xf9b6), 0x01 },
	{ CCI_REG8(0xf9b7), 0x15 },
	{ CCI_REG8(0xf9b8), 0xfc },
	{ CCI_REG8(0xf9b9), 0x40 },
	{ CCI_REG8(0xf9ba), 0x01 },
	{ CCI_REG8(0xf9bb), 0xff },
	{ CCI_REG8(0xf9bc), 0xfc },
	{ CCI_REG8(0xf9bd), 0x30 },
	{ CCI_REG8(0xf9be), 0xff },
	{ CCI_REG8(0xf9bf), 0xfc },
	{ CCI_REG8(0xf9c0), 0x22 },
	{ CCI_REG8(0xf9c1), 0x50 },
	{ CCI_REG8(0xf9c2), 0xa5 },
	{ CCI_REG8(0xf9c3), 0x50 },
	{ CCI_REG8(0xf9c4), 0x40 },
	{ CCI_REG8(0xf9c5), 0x30 },
	{ CCI_REG8(0xf9c6), 0x5f },
	{ CCI_REG8(0xf9c7), 0xc4 },
	{ CCI_REG8(0xf9c8), 0x04 },
	{ CCI_REG8(0xf9c9), 0x04 },
	{ CCI_REG8(0xf9ca), 0xd0 },
	{ CCI_REG8(0xf9cb), 0x20 },
	{ CCI_REG8(0xf9cc), 0x44 },
	{ CCI_REG8(0xf9cd), 0x34 },
	{ CCI_REG8(0xf9ce), 0x1c },
	{ CCI_REG8(0xf9cf), 0x04 },
	{ CCI_REG8(0xf9d0), 0xa8 },
	{ CCI_REG8(0xf9d1), 0x24 },
	{ CCI_REG8(0xf9d2), 0xa8 },
	{ CCI_REG8(0xf9d3), 0x2d },
	{ CCI_REG8(0xf9d4), 0xa8 },
	{ CCI_REG8(0xf9d5), 0x37 },
	{ CCI_REG8(0xf9d6), 0xb0 },
	{ CCI_REG8(0xf9d7), 0x1c },
	{ CCI_REG8(0xf9d8), 0xb0 },
	{ CCI_REG8(0xf9d9), 0x21 },
	{ CCI_REG8(0xf9da), 0xa0 },
	{ CCI_REG8(0xf9db), 0x11 },
	{ CCI_REG8(0xf9dc), 0x4f },
	{ CCI_REG8(0xf9dd), 0x86 },
	{ CCI_REG8(0xf9de), 0xac },
	{ CCI_REG8(0xf9df), 0x17 },
	{ CCI_REG8(0xf9e0), 0xd0 },
	{ CCI_REG8(0xf9e1), 0x18 },
	{ CCI_REG8(0xf9e2), 0x08 },
	{ CCI_REG8(0xf9e3), 0x00 },
	{ CCI_REG8(0xf9e4), 0xf0 },
	{ CCI_REG8(0xf9e5), 0x00 },
	{ CCI_REG8(0xf9e6), 0x06 },
	{ CCI_REG8(0xf9e7), 0xac },
	{ CCI_REG8(0xf9e8), 0xa1 },
	{ CCI_REG8(0xf9e9), 0x00 },
	{ CCI_REG8(0xf9ea), 0xfa },
	{ CCI_REG8(0xf9eb), 0xff },
	{ CCI_REG8(0xf9ec), 0xfe },
	{ CCI_REG8(0xf9ed), 0x9a },
	{ CCI_REG8(0xf9ee), 0xa8 },
	{ CCI_REG8(0xf9ef), 0x17 },
	{ CCI_REG8(0xf9f0), 0xa0 },
	{ CCI_REG8(0xf9f1), 0x05 },
	{ CCI_REG8(0xf9f2), 0x4f },
	{ CCI_REG8(0xf9f3), 0x86 },
	{ CCI_REG8(0xf9f4), 0xac },
	{ CCI_REG8(0xf9f5), 0x17 },
	{ CCI_REG8(0xf9f6), 0xd0 },
	{ CCI_REG8(0xf9f7), 0x18 },
	{ CCI_REG8(0xf9f8), 0x08 },
	{ CCI_REG8(0xf9f9), 0x00 },
	{ CCI_REG8(0xf9fa), 0xf0 },
	{ CCI_REG8(0xf9fb), 0x00 },
	{ CCI_REG8(0xf9fc), 0x06 },
	{ CCI_REG8(0xf9fd), 0xac },
	{ CCI_REG8(0xf9fe), 0xa1 },
	{ CCI_REG8(0xf9ff), 0x00 },
	{ CCI_REG8(0xfa00), 0xfa },
	{ CCI_REG8(0xfa01), 0xff },
	{ CCI_REG8(0xfa02), 0xfe },
	{ CCI_REG8(0xfa03), 0xe0 },
	{ CCI_REG8(0xfa04), 0xa8 },
	{ CCI_REG8(0xfa05), 0x17 },
	{ CCI_REG8(0xfa06), 0xa0 },
	{ CCI_REG8(0xfa07), 0x05 },
	{ CCI_REG8(0x97ac), 0x00 },
	{ CCI_REG8(0x97ad), 0x00 },
	{ CCI_REG8(0x97ae), 0x92 },
	{ CCI_REG8(0x97af), 0x84 },
	{ CCI_REG8(0x9788), 0x00 },
	{ CCI_REG8(0x9789), 0x02 },
	{ CCI_REG8(0x978a), 0x57 },
	{ CCI_REG8(0x978b), 0x5c },
	{ CCI_REG8(0x4331), 0x01 },
	{ CCI_REG8(0x2e1f), 0x00 },
	{ CCI_REG8(0x33f0), 0x07 },
	{ CCI_REG8(0x33f1), 0x03 },
	{ CCI_REG8(0x0111), 0x03 },
	{ CCI_REG8(0x3218), 0x01 },
	{ CCI_REG8(0x3953), 0x01 },
	{ CCI_REG8(0x3954), 0x01 },
	{ CCI_REG8(0x3955), 0x01 },
	{ CCI_REG8(0x3bc0), 0xe6 },
	{ CCI_REG8(0x3bc4), 0xe6 },
	{ CCI_REG8(0x3bc8), 0xe6 },
	{ CCI_REG8(0x3bcc), 0xe6 },
	{ CCI_REG8(0x5315), 0x0f },
	{ CCI_REG8(0x571b), 0x2c },
	{ CCI_REG8(0x5f63), 0xe7 },
	{ CCI_REG8(0x5f64), 0xe8 },
	{ CCI_REG8(0x6d2b), 0x01 },
	{ CCI_REG8(0x6d43), 0x01 },
	{ CCI_REG8(0x6d5b), 0x01 },
	{ CCI_REG8(0x6d73), 0x01 },
	{ CCI_REG8(0x6d8b), 0x01 },
	{ CCI_REG8(0x6da3), 0x01 },
	{ CCI_REG8(0x6dbb), 0x01 },
	{ CCI_REG8(0x6dd3), 0x01 },
	{ CCI_REG8(0x6e1b), 0x01 },
	{ CCI_REG8(0x6e33), 0x01 },
	{ CCI_REG8(0x6e4b), 0x01 },
	{ CCI_REG8(0x767c), 0x0b },
	{ CCI_REG8(0x7685), 0x01 },
	{ CCI_REG8(0x76a6), 0x08 },
	{ CCI_REG8(0x76a7), 0x08 },
	{ CCI_REG8(0x76a8), 0x08 },
	{ CCI_REG8(0x76a9), 0x08 },
	{ CCI_REG8(0x76aa), 0x08 },
	{ CCI_REG8(0x76ab), 0x08 },
	{ CCI_REG8(0x76ae), 0x08 },
	{ CCI_REG8(0x76af), 0x08 },
	{ CCI_REG8(0x76b0), 0x08 },
	{ CCI_REG8(0x76b5), 0x08 },
	{ CCI_REG8(0x76b6), 0x08 },
	{ CCI_REG8(0x76b7), 0x08 },
	{ CCI_REG8(0x76b8), 0x08 },
	{ CCI_REG8(0x76b9), 0x08 },
	{ CCI_REG8(0x76ba), 0x08 },
	{ CCI_REG8(0x76bd), 0x08 },
	{ CCI_REG8(0x76be), 0x08 },
	{ CCI_REG8(0x76bf), 0x08 },
	{ CCI_REG8(0x76cd), 0x02 },
	{ CCI_REG8(0x78a5), 0x04 },
	{ CCI_REG8(0x78a6), 0x04 },
	{ CCI_REG8(0x78a7), 0x04 },
	{ CCI_REG8(0x78a8), 0x04 },
	{ CCI_REG8(0x78a9), 0x04 },
	{ CCI_REG8(0x7a0c), 0x40 },
	{ CCI_REG8(0x7a0d), 0x40 },
	{ CCI_REG8(0x7a1a), 0x39 },
	{ CCI_REG8(0x7a1b), 0x39 },
	{ CCI_REG8(0x7a1c), 0x37 },
	{ CCI_REG8(0x7a1d), 0x37 },
	{ CCI_REG8(0x7a1e), 0x37 },
	{ CCI_REG8(0x7a1f), 0x37 },
	{ CCI_REG8(0x7a20), 0x37 },
	{ CCI_REG8(0x7a21), 0x37 },
	{ CCI_REG8(0x7a22), 0x37 },
	{ CCI_REG8(0x7a23), 0x37 },
	{ CCI_REG8(0x7a24), 0x37 },
	{ CCI_REG8(0x7a25), 0x37 },
	{ CCI_REG8(0x7a26), 0x37 },
	{ CCI_REG8(0x7a27), 0x37 },
	{ CCI_REG8(0x7a28), 0x39 },
	{ CCI_REG8(0x7a29), 0x39 },
	{ CCI_REG8(0x7a2a), 0x39 },
	{ CCI_REG8(0x7a2b), 0x39 },
	{ CCI_REG8(0x7a2c), 0x37 },
	{ CCI_REG8(0x7a2d), 0x37 },
	{ CCI_REG8(0x7a2e), 0x37 },
	{ CCI_REG8(0x7a2f), 0x37 },
	{ CCI_REG8(0x7a30), 0x37 },
	{ CCI_REG8(0x7a31), 0x37 },
	{ CCI_REG8(0x7a32), 0x37 },
	{ CCI_REG8(0x7a33), 0x37 },
	{ CCI_REG8(0x7a34), 0x37 },
	{ CCI_REG8(0x7a35), 0x37 },
	{ CCI_REG8(0x7a36), 0x37 },
	{ CCI_REG8(0x7a37), 0x37 },
	{ CCI_REG8(0x7a38), 0x37 },
	{ CCI_REG8(0x7a39), 0x37 },
	{ CCI_REG8(0x7a3a), 0x2d },
	{ CCI_REG8(0x7a3b), 0x2d },
	{ CCI_REG8(0x7a3c), 0x37 },
	{ CCI_REG8(0x7a3d), 0x37 },
	{ CCI_REG8(0x7a3e), 0x37 },
	{ CCI_REG8(0x7a3f), 0x2d },
	{ CCI_REG8(0x7a5c), 0x78 },
	{ CCI_REG8(0x7a5d), 0x78 },
	{ CCI_REG8(0x7a6a), 0x78 },
	{ CCI_REG8(0x7a6b), 0x78 },
	{ CCI_REG8(0x7a6c), 0x7d },
	{ CCI_REG8(0x7a6d), 0x7d },
	{ CCI_REG8(0x7a6e), 0x7d },
	{ CCI_REG8(0x7a6f), 0x7d },
	{ CCI_REG8(0x7a70), 0x7d },
	{ CCI_REG8(0x7a71), 0x7d },
	{ CCI_REG8(0x7a72), 0x7d },
	{ CCI_REG8(0x7a73), 0x7d },
	{ CCI_REG8(0x7a74), 0x7d },
	{ CCI_REG8(0x7a75), 0x7d },
	{ CCI_REG8(0x7a76), 0x7d },
	{ CCI_REG8(0x7a77), 0x7d },
	{ CCI_REG8(0x7a78), 0x7d },
	{ CCI_REG8(0x7a79), 0x7d },
	{ CCI_REG8(0x7a7a), 0x7d },
	{ CCI_REG8(0x7a7b), 0x7d },
	{ CCI_REG8(0x7a7c), 0x7d },
	{ CCI_REG8(0x7a7d), 0x7d },
	{ CCI_REG8(0x7a7e), 0x7d },
	{ CCI_REG8(0x7a7f), 0x7d },
	{ CCI_REG8(0x7a80), 0x7c },
	{ CCI_REG8(0x7a81), 0x7c },
	{ CCI_REG8(0x7a82), 0x7c },
	{ CCI_REG8(0x7a83), 0x7d },
	{ CCI_REG8(0x7a84), 0x7d },
	{ CCI_REG8(0x7a85), 0x7d },
	{ CCI_REG8(0x7a86), 0x7d },
	{ CCI_REG8(0x7a87), 0x7d },
	{ CCI_REG8(0x7a88), 0x7d },
	{ CCI_REG8(0x7a89), 0x7d },
	{ CCI_REG8(0x7a8a), 0x7c },
	{ CCI_REG8(0x7a8b), 0x7c },
	{ CCI_REG8(0x7a8c), 0x7d },
	{ CCI_REG8(0x7a8d), 0x7d },
	{ CCI_REG8(0x7a8e), 0x7d },
	{ CCI_REG8(0x7a8f), 0x7c },
	{ CCI_REG8(0x7c08), 0x0a },
	{ CCI_REG8(0x7c09), 0x0a },
	{ CCI_REG8(0x7c0a), 0x0f },
	{ CCI_REG8(0x7c0b), 0x0f },
	{ CCI_REG8(0x7c0c), 0x0f },
	{ CCI_REG8(0x7c0d), 0x0f },
	{ CCI_REG8(0x7c0e), 0x0a },
	{ CCI_REG8(0x7c0f), 0x0a },
	{ CCI_REG8(0x7c10), 0x0a },
	{ CCI_REG8(0x7c11), 0x0a },
	{ CCI_REG8(0x7c12), 0x0a },
	{ CCI_REG8(0x7c13), 0x0a },
	{ CCI_REG8(0x7c14), 0x0a },
	{ CCI_REG8(0x7c15), 0x0a },
	{ CCI_REG8(0x7c16), 0x14 },
	{ CCI_REG8(0x7c17), 0x14 },
	{ CCI_REG8(0x7c18), 0x0f },
	{ CCI_REG8(0x7c19), 0x0f },
	{ CCI_REG8(0x7c1a), 0x0f },
	{ CCI_REG8(0x7c1b), 0x0f },
	{ CCI_REG8(0x7c1c), 0x0a },
	{ CCI_REG8(0x7c1d), 0x0a },
	{ CCI_REG8(0x7c1e), 0x0a },
	{ CCI_REG8(0x7c1f), 0x0a },
	{ CCI_REG8(0x7c20), 0x0a },
	{ CCI_REG8(0x7c21), 0x0a },
	{ CCI_REG8(0x7c22), 0x0a },
	{ CCI_REG8(0x7c23), 0x0a },
	{ CCI_REG8(0x7c26), 0x23 },
	{ CCI_REG8(0x7c27), 0x23 },
	{ CCI_REG8(0x7c28), 0x23 },
	{ CCI_REG8(0x7c29), 0x23 },
	{ CCI_REG8(0x7c2a), 0x23 },
	{ CCI_REG8(0x7c2b), 0x23 },
	{ CCI_REG8(0x7c2c), 0x0a },
	{ CCI_REG8(0x7c2d), 0x0a },
	{ CCI_REG8(0x7c2e), 0x23 },
	{ CCI_REG8(0x7c2f), 0x23 },
	{ CCI_REG8(0x7c30), 0x23 },
	{ CCI_REG8(0x7c31), 0x0a },
	{ CCI_REG8(0x7c63), 0x2d },
	{ CCI_REG8(0x7c64), 0x2d },
	{ CCI_REG8(0x7c65), 0x2d },
	{ CCI_REG8(0xae2f), 0x1a },
	{ CCI_REG8(0xb101), 0x0a },
	{ CCI_REG8(0xb102), 0x0a },
	{ CCI_REG8(0xb104), 0x0a },
	{ CCI_REG8(0xb105), 0x0a },
	{ CCI_REG8(0xb10c), 0x02 },
	{ CCI_REG8(0xb10d), 0x02 },
	{ CCI_REG8(0xb10e), 0x02 },
	{ CCI_REG8(0xb10f), 0x02 },
	{ CCI_REG8(0xb110), 0x02 },
	{ CCI_REG8(0xb111), 0x02 },
	{ CCI_REG8(0xb227), 0x2e },
	{ CCI_REG8(0xb229), 0x0b },
	{ CCI_REG8(0xb22d), 0x2e },
	{ CCI_REG8(0xb22f), 0x0b },
	{ CCI_REG8(0xb233), 0x05 },
	{ CCI_REG8(0xb235), 0x05 },
	{ CCI_REG8(0xb239), 0x05 },
	{ CCI_REG8(0xb23b), 0x05 },
	{ CCI_REG8(0xb23d), 0x16 },
	{ CCI_REG8(0xb23f), 0x45 },
	{ CCI_REG8(0xb241), 0xfc },
	{ CCI_REG8(0xb243), 0x16 },
	{ CCI_REG8(0xb245), 0x45 },
	{ CCI_REG8(0xb247), 0xfc },
	{ CCI_REG8(0xb30f), 0x3c },
	{ CCI_REG8(0xb311), 0x3c },
	{ CCI_REG8(0xb315), 0x3c },
	{ CCI_REG8(0xb317), 0x3c },
	{ CCI_REG8(0xb31b), 0x07 },
	{ CCI_REG8(0xb31d), 0x07 },
	{ CCI_REG8(0xb321), 0x07 },
	{ CCI_REG8(0xb323), 0x07 },
	{ CCI_REG8(0xb325), 0x23 },
	{ CCI_REG8(0xb327), 0x56 },
	{ CCI_REG8(0xb328), 0x01 },
	{ CCI_REG8(0xb329), 0x4a },
	{ CCI_REG8(0xb32b), 0x23 },
	{ CCI_REG8(0xb32d), 0x56 },
	{ CCI_REG8(0xb32e), 0x01 },
	{ CCI_REG8(0xb32f), 0x4a },
};

static const struct cci_reg_sequence mode_4096x3072_regs[] = {
	{ CCI_REG8(0x0106), 0x01 },
	{ CCI_REG8(0x0112), 0x0a },
	{ CCI_REG8(0x0113), 0x0a },
	{ CCI_REG8(0x0114), 0x02 },
	{ CCI_REG8(0x3239), 0x00 },
	{ CCI_REG8(0x0342), 0x25 },
	{ CCI_REG8(0x0343), 0xe0 },
	{ CCI_REG8(0x3850), 0x00 },
	{ CCI_REG8(0x3851), 0x55 },
	{ CCI_REG8(0x0340), 0x1d },
	{ CCI_REG8(0x0341), 0x68 },
	{ CCI_REG8(0x0344), 0x00 },
	{ CCI_REG8(0x0345), 0x00 },
	{ CCI_REG8(0x0346), 0x00 },
	{ CCI_REG8(0x0347), 0x00 },
	{ CCI_REG8(0x0348), 0x1f },
	{ CCI_REG8(0x0349), 0xff },
	{ CCI_REG8(0x034a), 0x17 },
	{ CCI_REG8(0x034b), 0xff },
	{ CCI_REG8(0x0900), 0x01 },
	{ CCI_REG8(0x0901), 0x22 },
	{ CCI_REG8(0x0902), 0x00 },
	{ CCI_REG8(0x3005), 0x02 },
	{ CCI_REG8(0x3006), 0x02 },
	{ CCI_REG8(0x3140), 0x0a },
	{ CCI_REG8(0x3144), 0x00 },
	{ CCI_REG8(0x3146), 0x00 },
	{ CCI_REG8(0x3148), 0x04 },
	{ CCI_REG8(0x3149), 0x01 },
	{ CCI_REG8(0x31c0), 0x41 },
	{ CCI_REG8(0x31c1), 0x41 },
	{ CCI_REG8(0x0408), 0x00 },
	{ CCI_REG8(0x0409), 0x00 },
	{ CCI_REG8(0x040a), 0x00 },
	{ CCI_REG8(0x040b), 0x00 },
	{ CCI_REG8(0x040c), 0x10 },
	{ CCI_REG8(0x040d), 0x00 },
	{ CCI_REG8(0x040e), 0x0c },
	{ CCI_REG8(0x040f), 0x00 },
	{ CCI_REG8(0x034c), 0x10 },
	{ CCI_REG8(0x034d), 0x00 },
	{ CCI_REG8(0x034e), 0x0c },
	{ CCI_REG8(0x034f), 0x00 },
	{ CCI_REG8(0x0301), 0x08 },
	{ CCI_REG8(0x0303), 0x02 },
	{ CCI_REG8(0x0305), 0x03 },
	{ CCI_REG8(0x0306), 0x01 },
	{ CCI_REG8(0x0307), 0x57 },
	{ CCI_REG8(0x030b), 0x01 },
	{ CCI_REG8(0x030d), 0x03 },
	{ CCI_REG8(0x030e), 0x01 },
	{ CCI_REG8(0x030f), 0x79 },
	{ CCI_REG8(0x3205), 0x00 },
	{ CCI_REG8(0x3206), 0x01 },
	{ CCI_REG8(0x3211), 0x01 },
	{ CCI_REG8(0x3855), 0x00 },
	{ CCI_REG8(0x38d0), 0x01 },
	{ CCI_REG8(0x38d1), 0x18 },
	{ CCI_REG8(0x38d2), 0x01 },
	{ CCI_REG8(0x38d3), 0x18 },
	{ CCI_REG8(0x39ac), 0x01 },
	{ CCI_REG8(0x0202), 0x03 },
	{ CCI_REG8(0x0203), 0xe8 },
	{ CCI_REG8(0x0224), 0x01 },
	{ CCI_REG8(0x0225), 0xf4 },
	{ CCI_REG8(0x0204), 0x00 },
	{ CCI_REG8(0x0205), 0x00 },
	{ CCI_REG8(0x020e), 0x01 },
	{ CCI_REG8(0x020f), 0x00 },
	{ CCI_REG8(0x0216), 0x00 },
	{ CCI_REG8(0x0217), 0x00 },
	{ CCI_REG8(0x0218), 0x01 },
	{ CCI_REG8(0x0219), 0x00 },
	{ CCI_REG8(0x0221), 0x11 },
	{ CCI_REG8(0x3180), 0x00 },
	{ CCI_REG8(0x3181), 0x00 },
	{ CCI_REG8(0x3182), 0x04 },
	{ CCI_REG8(0x320b), 0x00 },
	{ CCI_REG8(0x4200), 0x01 },
	{ CCI_REG8(0x4201), 0x01 },
	{ CCI_REG8(0x3108), 0x01 },
	{ CCI_REG8(0x3104), 0x01 },
	{ CCI_REG8(0x3103), 0x00 },
	{ CCI_REG8(0x3422), 0x01 },
	{ CCI_REG8(0x3423), 0xfc },
	{ CCI_REG8(0x3978), 0x00 },
	{ CCI_REG8(0x3190), 0x00 },
	{ CCI_REG8(0x3191), 0x00 },
	{ CCI_REG8(0x0e00), 0x00 },
	{ CCI_REG8(0x0e06), 0x03 },
	{ CCI_REG8(0x0110), 0x00 },
	{ CCI_REG8(0x30a4), 0x00 },
	{ CCI_REG8(0x30a6), 0x00 },
	{ CCI_REG8(0x0808), 0x02 },
	{ CCI_REG8(0x084e), 0x00 },
	{ CCI_REG8(0x084f), 0x15 },
	{ CCI_REG8(0x0850), 0x00 },
	{ CCI_REG8(0x0851), 0x11 },
	{ CCI_REG8(0x0852), 0x00 },
	{ CCI_REG8(0x0853), 0x23 },
	{ CCI_REG8(0x0854), 0x00 },
	{ CCI_REG8(0x0855), 0x29 },
	{ CCI_REG8(0x0858), 0x00 },
	{ CCI_REG8(0x0859), 0x1b },
	{ CCI_REG8(0x34e2), 0x0f },
	{ CCI_REG8(0x34e3), 0xff },
	{ CCI_REG8(0x34e8), 0x00 },
	{ CCI_REG8(0x34ea), 0x0f },
};

static const struct cci_reg_sequence mode_8192x6144_regs[] = {
	{ CCI_REG8(0x0106), 0x01 },
	{ CCI_REG8(0x0112), 0x0a },
	{ CCI_REG8(0x0113), 0x0a },
	{ CCI_REG8(0x0114), 0x02 },
	{ CCI_REG8(0x3239), 0x00 },
	{ CCI_REG8(0x0342), 0x2c },
	{ CCI_REG8(0x0343), 0x50 },
	{ CCI_REG8(0x3850), 0x00 },
	{ CCI_REG8(0x3851), 0x66 },
	{ CCI_REG8(0x0340), 0x18 },
	{ CCI_REG8(0x0341), 0x82 },
	{ CCI_REG8(0x0344), 0x00 },
	{ CCI_REG8(0x0345), 0x00 },
	{ CCI_REG8(0x0346), 0x00 },
	{ CCI_REG8(0x0347), 0x00 },
	{ CCI_REG8(0x0348), 0x1f },
	{ CCI_REG8(0x0349), 0xff },
	{ CCI_REG8(0x034a), 0x17 },
	{ CCI_REG8(0x034b), 0xff },
	{ CCI_REG8(0x0900), 0x00 },
	{ CCI_REG8(0x0901), 0x11 },
	{ CCI_REG8(0x0902), 0x00 },
	{ CCI_REG8(0x3005), 0x00 },
	{ CCI_REG8(0x3006), 0x00 },
	{ CCI_REG8(0x3140), 0x0a },
	{ CCI_REG8(0x3144), 0x00 },
	{ CCI_REG8(0x3146), 0x00 },
	{ CCI_REG8(0x3148), 0x00 },
	{ CCI_REG8(0x3149), 0x01 },
	{ CCI_REG8(0x31c0), 0x01 },
	{ CCI_REG8(0x31c1), 0x01 },
	{ CCI_REG8(0x0408), 0x00 },
	{ CCI_REG8(0x0409), 0x00 },
	{ CCI_REG8(0x040a), 0x00 },
	{ CCI_REG8(0x040b), 0x00 },
	{ CCI_REG8(0x040c), 0x20 },
	{ CCI_REG8(0x040d), 0x00 },
	{ CCI_REG8(0x040e), 0x18 },
	{ CCI_REG8(0x040f), 0x00 },
	{ CCI_REG8(0x034c), 0x20 },
	{ CCI_REG8(0x034d), 0x00 },
	{ CCI_REG8(0x034e), 0x18 },
	{ CCI_REG8(0x034f), 0x00 },
	{ CCI_REG8(0x0301), 0x08 },
	{ CCI_REG8(0x0303), 0x02 },
	{ CCI_REG8(0x0305), 0x03 },
	{ CCI_REG8(0x0306), 0x01 },
	{ CCI_REG8(0x0307), 0x4e },
	{ CCI_REG8(0x030b), 0x01 },
	{ CCI_REG8(0x030d), 0x03 },
	{ CCI_REG8(0x030e), 0x01 },
	{ CCI_REG8(0x030f), 0xf6 },
	{ CCI_REG8(0x3205), 0x01 },
	{ CCI_REG8(0x3206), 0x01 },
	{ CCI_REG8(0x3211), 0x01 },
	{ CCI_REG8(0x3855), 0x01 },
	{ CCI_REG8(0x38d0), 0x01 },
	{ CCI_REG8(0x38d1), 0x2c },
	{ CCI_REG8(0x38d2), 0x01 },
	{ CCI_REG8(0x38d3), 0x2c },
	{ CCI_REG8(0x39ac), 0x01 },
	{ CCI_REG8(0x0202), 0x03 },
	{ CCI_REG8(0x0203), 0xe8 },
	{ CCI_REG8(0x0224), 0x01 },
	{ CCI_REG8(0x0225), 0xf4 },
	{ CCI_REG8(0x0204), 0x00 },
	{ CCI_REG8(0x0205), 0x00 },
	{ CCI_REG8(0x020e), 0x01 },
	{ CCI_REG8(0x020f), 0x00 },
	{ CCI_REG8(0x0216), 0x00 },
	{ CCI_REG8(0x0217), 0x00 },
	{ CCI_REG8(0x0218), 0x01 },
	{ CCI_REG8(0x0219), 0x00 },
	{ CCI_REG8(0x0221), 0x11 },
	{ CCI_REG8(0x3180), 0x00 },
	{ CCI_REG8(0x3181), 0x00 },
	{ CCI_REG8(0x3182), 0x04 },
	{ CCI_REG8(0x320b), 0x00 },
	{ CCI_REG8(0x4200), 0x01 },
	{ CCI_REG8(0x4201), 0x01 },
	{ CCI_REG8(0x3108), 0x01 },
	{ CCI_REG8(0x3104), 0x01 },
	{ CCI_REG8(0x3103), 0x00 },
	{ CCI_REG8(0x3422), 0x01 },
	{ CCI_REG8(0x3423), 0xfc },
	{ CCI_REG8(0x3978), 0x00 },
	{ CCI_REG8(0x3190), 0x00 },
	{ CCI_REG8(0x3191), 0x00 },
	{ CCI_REG8(0x0e00), 0x00 },
	{ CCI_REG8(0x0e06), 0x03 },
	{ CCI_REG8(0x0110), 0x00 },
	{ CCI_REG8(0x30a4), 0x00 },
	{ CCI_REG8(0x30a6), 0x00 },
	{ CCI_REG8(0x0808), 0x02 },
	{ CCI_REG8(0x084e), 0x00 },
	{ CCI_REG8(0x084f), 0x1d },
	{ CCI_REG8(0x0850), 0x00 },
	{ CCI_REG8(0x0851), 0x19 },
	{ CCI_REG8(0x0852), 0x00 },
	{ CCI_REG8(0x0853), 0x2f },
	{ CCI_REG8(0x0854), 0x00 },
	{ CCI_REG8(0x0855), 0x29 },
	{ CCI_REG8(0x0858), 0x00 },
	{ CCI_REG8(0x0859), 0x1b },
	{ CCI_REG8(0x34e2), 0x0f },
	{ CCI_REG8(0x34e3), 0xff },
	{ CCI_REG8(0x34e8), 0x00 },
	{ CCI_REG8(0x34ea), 0x0f },
};

static const struct lyt808_mode supported_modes[] = {
	{
		.width = 4096,
		.height = 3072,
		.code = MEDIA_BUS_FMT_SRGGB10_1X10,
		.hblank = 9696 - 4096,
		.vblank = 7528 - 3072,
		.vblank_min = 3764 - 3072,
		.vblank_max = 0xffff - 3072,
		/* Measured 30 fps with LL 9696 and FLL 7528. */
		.pclk = 2189761920,
		.link_freq_idx = 0,
		.reg_list = {
			.num_of_regs = ARRAY_SIZE(mode_4096x3072_regs),
			.regs = mode_4096x3072_regs,
		},
	},
	{
		.width = 8192,
		.height = 6144,
		.code = MEDIA_BUS_FMT_SRGGB10_1X10,
		.hblank = 11344 - 8192,
		.vblank = 6274 - 6144,
		.vblank_min = 6274 - 6144,
		.vblank_max = 0xffff - 6144,
		.pclk = 1094880960,
		.link_freq_idx = 0,
		.reg_list = {
			.num_of_regs = ARRAY_SIZE(mode_8192x6144_regs),
			.regs = mode_8192x6144_regs,
		},
	},
};
static inline struct lyt808 *to_lyt808(struct v4l2_subdev *subdev)
{
	return container_of(subdev, struct lyt808, sd);
}

static int lyt808_write_reg(struct lyt808 *lyt808, u16 reg,
			    u32 len, u32 val)
{
	if (len == 1)
		return cci_write(lyt808->regmap, CCI_REG8(reg), val, NULL);
	if (len == 2)
		return cci_write(lyt808->regmap, CCI_REG16(reg), val, NULL);
	return -EINVAL;
}

static int lyt808_write_regs(struct lyt808 *lyt808,
			     const struct cci_reg_sequence *regs, u32 len)
{
	return cci_multi_reg_write(lyt808->regmap, regs, len, NULL);
}

static int lyt808_update_controls(struct lyt808 *lyt808,
				  const struct lyt808_mode *mode)
{
	int ret;

	ret = __v4l2_ctrl_s_ctrl(lyt808->link_freq_ctrl, mode->link_freq_idx);
	if (ret)
		return ret;

	ret = __v4l2_ctrl_s_ctrl_int64(lyt808->pclk_ctrl, mode->pclk);
	if (ret)
		return ret;

	ret = __v4l2_ctrl_modify_range(lyt808->hblank_ctrl, mode->hblank,
				       mode->hblank, 1, mode->hblank);
	if (ret)
		return ret;

	ret = __v4l2_ctrl_modify_range(lyt808->vblank_ctrl, mode->vblank_min,
				       mode->vblank_max, 1, mode->vblank);
	if (ret)
		return ret;

	ret = __v4l2_ctrl_s_ctrl(lyt808->vblank_ctrl, mode->vblank);
	if (ret)
		return ret;

	/* The height may change even when the VBLANK value stays the same. */
	return __v4l2_ctrl_modify_range(lyt808->exp_ctrl, LYT808_EXPOSURE_MIN,
			mode->height + mode->vblank - LYT808_EXPOSURE_OFFSET,
			1, LYT808_EXPOSURE_DEFAULT);
}

static int lyt808_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct lyt808 *lyt808 =
		container_of(ctrl->handler, struct lyt808, ctrl_handler);
	u32 lpfr = lyt808->cur_mode->height + lyt808->vblank_ctrl->val;
	int ret, release_ret;

	if (ctrl->id == V4L2_CID_VBLANK) {
		ret = __v4l2_ctrl_modify_range(lyt808->exp_ctrl,
				LYT808_EXPOSURE_MIN, lpfr - LYT808_EXPOSURE_OFFSET,
				1, min_t(u32, LYT808_EXPOSURE_DEFAULT,
					 lpfr - LYT808_EXPOSURE_OFFSET));
		if (ret)
			return ret;
	}

	ret = pm_runtime_get_if_in_use(lyt808->dev);
	if (ret <= 0)
		return ret;

	if (ctrl->id == V4L2_CID_TEST_PATTERN) {
		ret = lyt808_write_reg(lyt808, LYT808_REG_TEST_PATTERN, 1,
				       ctrl->val ? 2 : 0);
		goto out_pm;
	}

	/* Frame length, exposure and gain must take effect in the same frame. */
	ret = lyt808_write_reg(lyt808, LYT808_REG_HOLD, 1, 1);
	if (ret)
		goto out_pm;

	switch (ctrl->id) {
	case V4L2_CID_VBLANK:
		ret = lyt808_write_reg(lyt808, LYT808_REG_LPFR, 2, lpfr);
		if (ret)
			break;
		fallthrough;
	case V4L2_CID_EXPOSURE:
		/* Exposure is the master of the exposure/analogue gain cluster. */
		ret = lyt808_write_reg(lyt808, LYT808_REG_EXPOSURE_CIT, 2,
				       lyt808->exp_ctrl->val);
		if (!ret)
			ret = lyt808_write_reg(lyt808, LYT808_REG_AGAIN, 2,
					       lyt808->again_ctrl->val);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	release_ret = lyt808_write_reg(lyt808, LYT808_REG_HOLD, 1, 0);
	if (!ret)
		ret = release_ret;
out_pm:
	pm_runtime_put_autosuspend(lyt808->dev);
	return ret;
}

static const struct v4l2_ctrl_ops lyt808_ctrl_ops = {
	.s_ctrl = lyt808_set_ctrl,
};

static int lyt808_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index > 0)
		return -EINVAL;

	code->code = supported_modes[0].code;

	return 0;
}

static int lyt808_enum_frame_size(struct v4l2_subdev *sd,
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

static void lyt808_fill_pad_format(struct lyt808 *lyt808,
				   const struct lyt808_mode *mode,
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

static int lyt808_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *sd_state)
{
	struct lyt808 *lyt808 = to_lyt808(sd);
	struct v4l2_subdev_format fmt = { 0 };

	fmt.which = sd_state ? V4L2_SUBDEV_FORMAT_TRY : V4L2_SUBDEV_FORMAT_ACTIVE;
	lyt808_fill_pad_format(lyt808, &supported_modes[0], &fmt);
	*v4l2_subdev_state_get_format(sd_state, 0) = fmt.format;

	return 0;
}

static int lyt808_get_pad_format(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_format *fmt)
{
	struct lyt808 *lyt808 = to_lyt808(sd);

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
		struct v4l2_mbus_framefmt *try_fmt =
			v4l2_subdev_state_get_format(sd_state, fmt->pad);

		fmt->format = *try_fmt;
	} else {
		lyt808_fill_pad_format(lyt808, lyt808->cur_mode, fmt);
	}

	return 0;
}

static int lyt808_set_pad_format(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_format *fmt)
{
	struct lyt808 *lyt808 = to_lyt808(sd);
	const struct lyt808_mode *mode, *old_mode;
	int ret = 0;

	mutex_lock(&lyt808->mutex);

	mode = v4l2_find_nearest_size(supported_modes,
				      ARRAY_SIZE(supported_modes),
				      width, height,
				      fmt->format.width, fmt->format.height);

	lyt808_fill_pad_format(lyt808, mode, fmt);

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
		struct v4l2_mbus_framefmt *try_fmt =
			v4l2_subdev_state_get_format(sd_state, fmt->pad);

		*try_fmt = fmt->format;
	} else {
		if (lyt808->streaming) {
			ret = -EBUSY;
			goto unlock;
		}
		old_mode = lyt808->cur_mode;
		lyt808->cur_mode = mode;
		ret = lyt808_update_controls(lyt808, mode);
		if (ret) {
			lyt808->cur_mode = old_mode;
			lyt808_update_controls(lyt808, old_mode);
		}
	}

unlock:
	mutex_unlock(&lyt808->mutex);

	return ret;
}

static int lyt808_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *sd_state,
				struct v4l2_subdev_selection *sel)
{
	struct lyt808 *lyt808 = to_lyt808(sd);
	const struct lyt808_mode *mode;
	struct v4l2_mbus_framefmt *fmt;

	if (sel->pad)
		return -EINVAL;

	switch (sel->target) {
	case V4L2_SEL_TGT_NATIVE_SIZE:
	case V4L2_SEL_TGT_CROP_BOUNDS:
	case V4L2_SEL_TGT_CROP_DEFAULT:
		sel->r = (struct v4l2_rect) {
			.width = LYT808_PIXEL_ARRAY_WIDTH,
			.height = LYT808_PIXEL_ARRAY_HEIGHT,
		};
		return 0;
	case V4L2_SEL_TGT_CROP:
		mutex_lock(&lyt808->mutex);
		if (sel->which == V4L2_SUBDEV_FORMAT_TRY) {
			fmt = v4l2_subdev_state_get_format(sd_state, sel->pad);
			mode = v4l2_find_nearest_size(supported_modes,
					ARRAY_SIZE(supported_modes), width, height,
					fmt->width, fmt->height);
		} else {
			mode = lyt808->cur_mode;
		}
		/* Mode 0 (4096x3072) uses 2x2 binning; mode 1 (8192x6144) is full array. */
		if (mode->width == 4096) {
			sel->r.width = mode->width * 2;
			sel->r.height = mode->height * 2;
		} else {
			sel->r.width = mode->width;
			sel->r.height = mode->height;
		}
		sel->r.left = (LYT808_PIXEL_ARRAY_WIDTH - sel->r.width) / 2;
		sel->r.top = (LYT808_PIXEL_ARRAY_HEIGHT - sel->r.height) / 2;
		mutex_unlock(&lyt808->mutex);
		return 0;
	default:
		return -EINVAL;
	}
}

static int lyt808_start_streaming(struct lyt808 *lyt808)
{
	const struct lyt808_reg_list *reg_list;
	int ret;

	/* Restore the sensor tuning after each power cycle. */
	ret = lyt808_write_regs(lyt808, lyt808_init_regs,
				ARRAY_SIZE(lyt808_init_regs));
	if (ret) {
		dev_err(lyt808->dev, "failed to set init registers: %d\n", ret);
		return ret;
	}

	/* Apply mode registers */
	reg_list = &lyt808->cur_mode->reg_list;
	ret = lyt808_write_regs(lyt808, reg_list->regs, reg_list->num_of_regs);
	if (ret) {
		dev_err(lyt808->dev, "failed to set mode registers: %d\n", ret);
		return ret;
	}

	/* Set V4L2 controls */
	ret = __v4l2_ctrl_handler_setup(&lyt808->ctrl_handler);
	if (ret)
		return ret;

	/* Start streaming */
	ret = lyt808_write_reg(lyt808, LYT808_REG_MODE_SELECT, 1,
			       LYT808_MODE_STREAMING);
	if (ret) {
		dev_err(lyt808->dev, "failed to start streaming: %d\n", ret);
		return ret;
	}

	/* Sensor stabilization delay */
	usleep_range(10000, 12000);

	return 0;
}

static int lyt808_stop_streaming(struct lyt808 *lyt808)
{
	return lyt808_write_reg(lyt808, LYT808_REG_MODE_SELECT,
				1, LYT808_MODE_STANDBY);
}

static int lyt808_set_stream(struct v4l2_subdev *sd, int enable)
{
	struct lyt808 *lyt808 = to_lyt808(sd);
	int ret = 0;

	mutex_lock(&lyt808->mutex);
	if (lyt808->streaming == !!enable)
		goto unlock;

	if (enable) {
		ret = pm_runtime_resume_and_get(lyt808->dev);
		if (ret)
			goto unlock;
		ret = lyt808_start_streaming(lyt808);
		if (ret) {
			lyt808_stop_streaming(lyt808);
			pm_runtime_put_autosuspend(lyt808->dev);
			goto unlock;
		}
	} else {
		ret = lyt808_stop_streaming(lyt808);
		/* Release the streaming reference even if the I2C write fails. */
		pm_runtime_put_autosuspend(lyt808->dev);
	}
	lyt808->streaming = !!enable;
unlock:
	mutex_unlock(&lyt808->mutex);
	return ret;
}

static int lyt808_detect(struct lyt808 *lyt808)
{
	u64 val;
	int ret;

	dev_dbg(lyt808->dev, "reading sensor ID over CCI\n");
	ret = cci_read(lyt808->regmap, CCI_REG16(LYT808_REG_ID), &val, NULL);
	if (ret)
		return ret;
	if (val != LYT808_ID)
		return dev_err_probe(lyt808->dev, -ENODEV,
				     "unexpected sensor ID 0x%04llx\n", val);

	dev_info(lyt808->dev, "Sony LYT808 identified (id 0x%04llx)\n", val);
	return 0;
}

static int lyt808_parse_hw_config(struct lyt808 *lyt808)
{
	struct fwnode_handle *fwnode = dev_fwnode(lyt808->dev);
	struct v4l2_fwnode_endpoint bus_cfg = {
		.bus_type = V4L2_MBUS_CSI2_CPHY
	};
	struct fwnode_handle *ep;
	unsigned long rate;
	unsigned int i;
	int ret;

	if (!fwnode)
		return -ENXIO;

	lyt808->reset_gpio = devm_gpiod_get_optional(lyt808->dev, "reset",
						     GPIOD_OUT_HIGH);
	if (IS_ERR(lyt808->reset_gpio)) {
		dev_err(lyt808->dev, "failed to get reset gpio %pe\n",
			lyt808->reset_gpio);
		return PTR_ERR(lyt808->reset_gpio);
	}

	lyt808->inclk = devm_v4l2_sensor_clk_get(lyt808->dev, NULL);
	if (IS_ERR(lyt808->inclk))
		return dev_err_probe(lyt808->dev, PTR_ERR(lyt808->inclk),
				     "could not get inclk\n");

	rate = clk_get_rate(lyt808->inclk);
	if (rate != LYT808_INCLK_RATE) {
		dev_err(lyt808->dev, "inclk frequency mismatch: expected %d, got %lu\n",
			LYT808_INCLK_RATE, rate);
		return -EINVAL;
	}

	for (i = 0; i < ARRAY_SIZE(lyt808_supply_names); i++)
		lyt808->supplies[i].supply = lyt808_supply_names[i];

	ret = devm_regulator_bulk_get(lyt808->dev,
				      ARRAY_SIZE(lyt808_supply_names),
				      lyt808->supplies);
	if (ret)
		return ret;

	ep = fwnode_graph_get_next_endpoint(fwnode, NULL);
	if (!ep)
		return -ENXIO;

	ret = v4l2_fwnode_endpoint_alloc_parse(ep, &bus_cfg);
	fwnode_handle_put(ep);
	if (ret)
		return ret;

	if (bus_cfg.bus.mipi_csi2.num_data_lanes != LYT808_NUM_DATA_LANES) {
		dev_err(lyt808->dev,
			"number of CSI2 data lanes %d is not supported\n",
			bus_cfg.bus.mipi_csi2.num_data_lanes);
		ret = -EINVAL;
		goto done_endpoint_free;
	}

	if (!bus_cfg.nr_of_link_frequencies) {
		dev_err(lyt808->dev, "no link frequencies defined\n");
		ret = -EINVAL;
		goto done_endpoint_free;
	}

	for (i = 0; i < bus_cfg.nr_of_link_frequencies; i++) {
		if (bus_cfg.link_frequencies[i] == LYT808_LINK_FREQ)
			goto done_endpoint_free;
	}

	ret = -EINVAL;

done_endpoint_free:
	v4l2_fwnode_endpoint_free(&bus_cfg);

	return ret;
}

static const struct v4l2_subdev_video_ops lyt808_video_ops = {
	.s_stream = lyt808_set_stream,
};

static const struct v4l2_subdev_pad_ops lyt808_pad_ops = {
	.enum_mbus_code = lyt808_enum_mbus_code,
	.enum_frame_size = lyt808_enum_frame_size,
	.get_fmt = lyt808_get_pad_format,
	.set_fmt = lyt808_set_pad_format,
	.get_selection = lyt808_get_selection,
};

static const struct v4l2_subdev_ops lyt808_subdev_ops = {
	.video = &lyt808_video_ops,
	.pad = &lyt808_pad_ops,
};

static const struct v4l2_subdev_internal_ops lyt808_internal_ops = {
	.init_state = lyt808_init_state,
};

static void lyt808_disable_supplies(struct lyt808 *lyt808)
{
	static const unsigned int order[] = { 3, 2, 4, 1, 0 };
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(order); i++) {
		dev_dbg(lyt808->dev, "disabling %s supply\n",
			lyt808_supply_names[order[i]]);
		regulator_disable(lyt808->supplies[order[i]].consumer);
	}
}

static int lyt808_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct lyt808 *lyt808 = to_lyt808(sd);
	unsigned int i;
	int ret;

	gpiod_set_value_cansleep(lyt808->reset_gpio, 1);

	/* VIO, auxiliary, analog, digital, then autofocus module supplies. */
	for (i = 0; i < ARRAY_SIZE(lyt808_supply_names); i++) {
		dev_dbg(dev, "enabling %s supply\n", lyt808_supply_names[i]);
		ret = regulator_enable(lyt808->supplies[i].consumer);
		if (ret) {
			dev_err(dev, "failed to enable %s: %d\n",
				lyt808_supply_names[i], ret);
			goto error_supplies;
		}
		dev_dbg(dev, "%s supply enabled\n", lyt808_supply_names[i]);
		if (i >= 2)
			usleep_range(1000, 2000);
	}

	dev_dbg(dev, "enabling input clock\n");
	ret = clk_prepare_enable(lyt808->inclk);
	if (ret)
		goto error_supplies;
	usleep_range(2000, 3000);

	dev_dbg(dev, "releasing sensor reset\n");
	gpiod_set_value_cansleep(lyt808->reset_gpio, 0);
	usleep_range(12000, 15000);
	dev_dbg(dev, "sensor power sequence complete\n");

	return 0;

error_supplies:
	while (i--)
		regulator_disable(lyt808->supplies[i].consumer);
	return ret;
}

static int lyt808_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct lyt808 *lyt808 = to_lyt808(sd);

	dev_dbg(dev, "asserting sensor reset\n");
	gpiod_set_value_cansleep(lyt808->reset_gpio, 1);
	usleep_range(1000, 2000);
	dev_dbg(dev, "disabling input clock\n");
	clk_disable_unprepare(lyt808->inclk);
	usleep_range(1000, 2000);
	lyt808_disable_supplies(lyt808);

	return 0;
}

static int lyt808_init_controls(struct lyt808 *lyt808)
{
	struct v4l2_ctrl_handler *ctrl_hdlr = &lyt808->ctrl_handler;
	const struct lyt808_mode *mode = lyt808->cur_mode;
	struct v4l2_fwnode_device_properties props;
	u32 lpfr;
	int ret;

	ret = v4l2_ctrl_handler_init(ctrl_hdlr, 7);
	if (ret)
		return ret;

	ctrl_hdlr->lock = &lyt808->mutex;

	lpfr = mode->vblank + mode->height;
	lyt808->exp_ctrl = v4l2_ctrl_new_std(ctrl_hdlr,
					     &lyt808_ctrl_ops,
					     V4L2_CID_EXPOSURE,
					     LYT808_EXPOSURE_MIN,
					     lpfr - LYT808_EXPOSURE_OFFSET,
					     LYT808_EXPOSURE_STEP,
					     LYT808_EXPOSURE_DEFAULT);

	lyt808->again_ctrl = v4l2_ctrl_new_std(ctrl_hdlr,
					       &lyt808_ctrl_ops,
					       V4L2_CID_ANALOGUE_GAIN,
					       LYT808_AGAIN_MIN,
					       LYT808_AGAIN_MAX,
					       LYT808_AGAIN_STEP,
					       LYT808_AGAIN_DEFAULT);

	v4l2_ctrl_cluster(2, &lyt808->exp_ctrl);

	v4l2_ctrl_new_std_menu_items(ctrl_hdlr, &lyt808_ctrl_ops,
				     V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(lyt808_test_pattern_menu) - 1,
				     0, 0, lyt808_test_pattern_menu);

	lyt808->vblank_ctrl = v4l2_ctrl_new_std(ctrl_hdlr,
						&lyt808_ctrl_ops,
						V4L2_CID_VBLANK,
						mode->vblank_min,
						mode->vblank_max,
						1, mode->vblank);

	lyt808->pclk_ctrl = v4l2_ctrl_new_std(ctrl_hdlr,
					      NULL,
					      V4L2_CID_PIXEL_RATE,
					      mode->pclk, mode->pclk,
					      1, mode->pclk);

	lyt808->hblank_ctrl = v4l2_ctrl_new_std(ctrl_hdlr,
						NULL,
						V4L2_CID_HBLANK,
						mode->hblank, mode->hblank,
						1, mode->hblank);

	if (lyt808->hblank_ctrl)
		lyt808->hblank_ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	lyt808->link_freq_ctrl =
		v4l2_ctrl_new_int_menu(ctrl_hdlr,
				       NULL,
				       V4L2_CID_LINK_FREQ,
				       ARRAY_SIZE(link_freq) - 1,
				       mode->link_freq_idx,
				       link_freq);

	if (lyt808->link_freq_ctrl)
		lyt808->link_freq_ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	ret = ctrl_hdlr->error;
	if (ret)
		goto error;

	ret = v4l2_fwnode_device_parse(lyt808->dev, &props);
	if (ret)
		goto error;

	ret = v4l2_ctrl_new_fwnode_properties(ctrl_hdlr, &lyt808_ctrl_ops,
					      &props);
	if (ret)
		goto error;

	return 0;

error:
	v4l2_ctrl_handler_free(ctrl_hdlr);
	return ret;
}

static int lyt808_probe(struct i2c_client *client)
{
	struct lyt808 *lyt808;
	const char *name = "lyt808";
	int ret;

	lyt808 = devm_kzalloc(&client->dev, sizeof(*lyt808), GFP_KERNEL);
	if (!lyt808)
		return -ENOMEM;

	lyt808->dev = &client->dev;
	dev_dbg(lyt808->dev, "begin sensor probe\n");
	lyt808->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(lyt808->regmap))
		return dev_err_probe(lyt808->dev, PTR_ERR(lyt808->regmap),
				     "failed to initialize CCI regmap\n");

	v4l2_i2c_subdev_init(&lyt808->sd, client, &lyt808_subdev_ops);
	lyt808->sd.internal_ops = &lyt808_internal_ops;

	dev_dbg(lyt808->dev, "parsing sensor resources\n");
	ret = lyt808_parse_hw_config(lyt808);
	if (ret) {
		dev_err(lyt808->dev, "HW configuration is not supported: %d\n", ret);
		return ret;
	}

	dev_dbg(lyt808->dev, "sensor resources acquired\n");
	mutex_init(&lyt808->mutex);

	ret = lyt808_power_on(lyt808->dev);
	if (ret) {
		dev_err(lyt808->dev, "failed to power-on the sensor\n");
		goto error_mutex_destroy;
	}

	/* Check sensor module identity (0x9186) */
	ret = lyt808_detect(lyt808);
	if (ret) {
		dev_err(lyt808->dev, "failed to find sensor: %d\n", ret);
		goto error_power_off;
	}

	/* Set default mode */
	lyt808->cur_mode = &supported_modes[0];

	ret = lyt808_init_controls(lyt808);
	if (ret) {
		dev_err(lyt808->dev, "failed to init controls: %d\n", ret);
		goto error_power_off;
	}

	lyt808->sd.ctrl_handler = &lyt808->ctrl_handler;
	lyt808->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	lyt808->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	v4l2_i2c_subdev_set_name(&lyt808->sd, client, name, NULL);

	lyt808->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&lyt808->sd.entity, 1, &lyt808->pad);
	if (ret) {
		dev_err(lyt808->dev, "failed to init entity pads: %d\n", ret);
		goto error_handler_free;
	}

	pm_runtime_set_active(lyt808->dev);
	pm_runtime_get_noresume(lyt808->dev);
	pm_runtime_set_autosuspend_delay(lyt808->dev, 1000);
	pm_runtime_use_autosuspend(lyt808->dev);
	pm_runtime_enable(lyt808->dev);

	dev_dbg(lyt808->dev, "registering sensor subdevice\n");
	ret = v4l2_async_register_subdev_sensor(&lyt808->sd);
	if (ret < 0) {
		dev_err(lyt808->dev,
			"failed to register async subdev: %d\n", ret);
		goto error_media_entity;
	}

	dev_dbg(lyt808->dev, "releasing sensor probe power reference\n");
	pm_runtime_put_autosuspend(lyt808->dev);
	dev_dbg(lyt808->dev, "sensor probe complete\n");

	return 0;

error_media_entity:
	pm_runtime_disable(lyt808->dev);
	pm_runtime_put_noidle(lyt808->dev);
	pm_runtime_set_suspended(lyt808->dev);
	media_entity_cleanup(&lyt808->sd.entity);
error_handler_free:
	v4l2_ctrl_handler_free(lyt808->sd.ctrl_handler);
error_power_off:
	lyt808_power_off(lyt808->dev);
error_mutex_destroy:
	mutex_destroy(&lyt808->mutex);

	return ret;
}

static void lyt808_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct lyt808 *lyt808 = to_lyt808(sd);

	v4l2_async_unregister_subdev(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(sd->ctrl_handler);

	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		lyt808_power_off(&client->dev);
	pm_runtime_set_suspended(&client->dev);

	mutex_destroy(&lyt808->mutex);
}

static const struct dev_pm_ops lyt808_pm_ops = {
	SET_RUNTIME_PM_OPS(lyt808_power_off, lyt808_power_on, NULL)
};

static const struct of_device_id lyt808_of_match[] = {
	{ .compatible = "sony,lyt808" },
	{ }
};
MODULE_DEVICE_TABLE(of, lyt808_of_match);

static struct i2c_driver lyt808_driver = {
	.probe = lyt808_probe,
	.remove = lyt808_remove,
	.driver = {
		.name = "lyt808",
		.pm = &lyt808_pm_ops,
		.of_match_table = lyt808_of_match,
	},
};

module_i2c_driver(lyt808_driver);

MODULE_DESCRIPTION("Sony LYT808 sensor driver");
MODULE_LICENSE("GPL");
