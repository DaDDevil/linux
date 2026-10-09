// SPDX-License-Identifier: GPL-2.0
/*
 * Sony IMX581 Camera Sensor Driver
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
#define IMX581_REG_MODE_SELECT		0x0100
#define IMX581_MODE_STANDBY		0x00
#define IMX581_MODE_STREAMING		0x01

/* Lines per frame (FLL) */
#define IMX581_REG_LPFR			0x0340

/* Chip ID */
#define IMX581_REG_ID			0x0016
#define IMX581_ID			0x0581

/* Exposure control */
#define IMX581_REG_EXPOSURE_CIT		0x0202
#define IMX581_EXPOSURE_MIN		16
#define IMX581_EXPOSURE_OFFSET		48
#define IMX581_EXPOSURE_STEP		1
#define IMX581_EXPOSURE_DEFAULT		0x0648

/* Analog gain control */
#define IMX581_REG_AGAIN		0x0204
#define IMX581_AGAIN_MIN		0
#define IMX581_AGAIN_MAX		978
#define IMX581_AGAIN_STEP		1
#define IMX581_AGAIN_DEFAULT		0

/* Group hold register */
#define IMX581_REG_HOLD			0x0104

#define IMX581_REG_TEST_PATTERN		0x0601

/* Input clock rate */
#define IMX581_INCLK_RATE		19200000

/* CSI2 HW configuration */
#define IMX581_LINK_FREQ		1422400000LL
#define IMX581_NUM_DATA_LANES		3

#define IMX581_PIXEL_ARRAY_WIDTH		8000
#define IMX581_PIXEL_ARRAY_HEIGHT		6000

struct imx581_reg_list {
	u32 num_of_regs;
	const struct cci_reg_sequence *regs;
};

struct imx581_mode {
	u32 width;
	u32 height;
	u32 code;
	u32 hblank;
	u32 vblank;
	u32 vblank_min;
	u32 vblank_max;
	u64 pclk;
	u32 link_freq_idx;
	struct imx581_reg_list reg_list;
};

static const char * const imx581_supply_names[] = {
	"dovdd",
	"auxvdd",
	"avdd",
	"dvdd",
	"afvdd",
};

struct imx581 {
	struct device *dev;
	struct regmap *regmap;
	struct v4l2_subdev sd;
	struct media_pad pad;
	struct gpio_desc *reset_gpio;
	struct clk *inclk;
	struct regulator_bulk_data supplies[ARRAY_SIZE(imx581_supply_names)];
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
	const struct imx581_mode *cur_mode;
	struct mutex mutex;
};

static const s64 link_freq[] = {
	IMX581_LINK_FREQ,
};

static const char * const imx581_test_pattern_menu[] = {
	"Disabled",
	"Color Bars",
};

/* OEM tuning, with the PHY interface selected for four-lane D-PHY. */
static const struct cci_reg_sequence imx581_init_regs[] = {
	{ CCI_REG8(0x0136), 0x13 },
	{ CCI_REG8(0x0137), 0x33 },
	{ CCI_REG8(0x3c7e), 0x02 },
	{ CCI_REG8(0x3c7f), 0x0a },
	{ CCI_REG8(0x0111), 0x03 },
	{ CCI_REG8(0x380c), 0x00 },
	{ CCI_REG8(0x3c00), 0x10 },
	{ CCI_REG8(0x3c01), 0x10 },
	{ CCI_REG8(0x3c02), 0x10 },
	{ CCI_REG8(0x3c03), 0x10 },
	{ CCI_REG8(0x3c04), 0x10 },
	{ CCI_REG8(0x3c05), 0x01 },
	{ CCI_REG8(0x3c06), 0x00 },
	{ CCI_REG8(0x3c07), 0x00 },
	{ CCI_REG8(0x3c08), 0x03 },
	{ CCI_REG8(0x3c09), 0xff },
	{ CCI_REG8(0x3c0a), 0x01 },
	{ CCI_REG8(0x3c0b), 0x00 },
	{ CCI_REG8(0x3c0c), 0x00 },
	{ CCI_REG8(0x3c0d), 0x03 },
	{ CCI_REG8(0x3c0e), 0xff },
	{ CCI_REG8(0x3c0f), 0x20 },
	{ CCI_REG8(0x3f88), 0x00 },
	{ CCI_REG8(0x3f8e), 0x00 },
	{ CCI_REG8(0x5282), 0x01 },
	{ CCI_REG8(0x9004), 0x14 },
	{ CCI_REG8(0x9200), 0xf4 },
	{ CCI_REG8(0x9201), 0xa7 },
	{ CCI_REG8(0x9202), 0xf4 },
	{ CCI_REG8(0x9203), 0xaa },
	{ CCI_REG8(0x9204), 0xf4 },
	{ CCI_REG8(0x9205), 0xad },
	{ CCI_REG8(0x9206), 0xf4 },
	{ CCI_REG8(0x9207), 0xb0 },
	{ CCI_REG8(0x9208), 0xf4 },
	{ CCI_REG8(0x9209), 0xb3 },
	{ CCI_REG8(0x920a), 0xb7 },
	{ CCI_REG8(0x920b), 0x34 },
	{ CCI_REG8(0x920c), 0xb7 },
	{ CCI_REG8(0x920d), 0x36 },
	{ CCI_REG8(0x920e), 0xb7 },
	{ CCI_REG8(0x920f), 0x37 },
	{ CCI_REG8(0x9210), 0xb7 },
	{ CCI_REG8(0x9211), 0x38 },
	{ CCI_REG8(0x9212), 0xb7 },
	{ CCI_REG8(0x9213), 0x39 },
	{ CCI_REG8(0x9214), 0xb7 },
	{ CCI_REG8(0x9215), 0x3a },
	{ CCI_REG8(0x9216), 0xb7 },
	{ CCI_REG8(0x9217), 0x3c },
	{ CCI_REG8(0x9218), 0xb7 },
	{ CCI_REG8(0x9219), 0x3d },
	{ CCI_REG8(0x921a), 0xb7 },
	{ CCI_REG8(0x921b), 0x3e },
	{ CCI_REG8(0x921c), 0xb7 },
	{ CCI_REG8(0x921d), 0x3f },
	{ CCI_REG8(0x921e), 0x77 },
	{ CCI_REG8(0x921f), 0x77 },
	{ CCI_REG8(0x9222), 0xc4 },
	{ CCI_REG8(0x9223), 0x4b },
	{ CCI_REG8(0x9224), 0xc4 },
	{ CCI_REG8(0x9225), 0x4c },
	{ CCI_REG8(0x9226), 0xc4 },
	{ CCI_REG8(0x9227), 0x4d },
	{ CCI_REG8(0x9810), 0x14 },
	{ CCI_REG8(0x9814), 0x14 },
	{ CCI_REG8(0x99b2), 0x20 },
	{ CCI_REG8(0x99b3), 0x0f },
	{ CCI_REG8(0x99b4), 0x0f },
	{ CCI_REG8(0x99b5), 0x0f },
	{ CCI_REG8(0x99b6), 0x0f },
	{ CCI_REG8(0x99e4), 0x0f },
	{ CCI_REG8(0x99e5), 0x0f },
	{ CCI_REG8(0x99e6), 0x0f },
	{ CCI_REG8(0x99e7), 0x0f },
	{ CCI_REG8(0x99e8), 0x0f },
	{ CCI_REG8(0x99e9), 0x0f },
	{ CCI_REG8(0x99ea), 0x0f },
	{ CCI_REG8(0x99eb), 0x0f },
	{ CCI_REG8(0x99ec), 0x0f },
	{ CCI_REG8(0x99ed), 0x0f },
	{ CCI_REG8(0xa569), 0x06 },
	{ CCI_REG8(0xa56a), 0x13 },
	{ CCI_REG8(0xa56b), 0x13 },
	{ CCI_REG8(0xa679), 0x20 },
	{ CCI_REG8(0xa830), 0x68 },
	{ CCI_REG8(0xa831), 0x56 },
	{ CCI_REG8(0xa832), 0x2b },
	{ CCI_REG8(0xa833), 0x55 },
	{ CCI_REG8(0xa834), 0x55 },
	{ CCI_REG8(0xa835), 0x16 },
	{ CCI_REG8(0xa837), 0x51 },
	{ CCI_REG8(0xa838), 0x34 },
	{ CCI_REG8(0xa854), 0x4f },
	{ CCI_REG8(0xa855), 0x48 },
	{ CCI_REG8(0xa856), 0x45 },
	{ CCI_REG8(0xa857), 0x02 },
	{ CCI_REG8(0xa85a), 0x23 },
	{ CCI_REG8(0xa85b), 0x16 },
	{ CCI_REG8(0xa85c), 0x12 },
	{ CCI_REG8(0xa85d), 0x02 },
	{ CCI_REG8(0xac72), 0x01 },
	{ CCI_REG8(0xac73), 0x26 },
	{ CCI_REG8(0xac74), 0x01 },
	{ CCI_REG8(0xac75), 0x26 },
	{ CCI_REG8(0xac76), 0x00 },
	{ CCI_REG8(0xac77), 0xc4 },
	{ CCI_REG8(0xb051), 0x02 },
	{ CCI_REG8(0xc020), 0x01 },
	{ CCI_REG8(0xc61d), 0x00 },
	{ CCI_REG8(0xc625), 0x00 },
	{ CCI_REG8(0xc638), 0x03 },
	{ CCI_REG8(0xc63b), 0x01 },
	{ CCI_REG8(0xe286), 0x31 },
	{ CCI_REG8(0xe2a6), 0x32 },
	{ CCI_REG8(0xe2c6), 0x33 },
	{ CCI_REG8(0xea4b), 0x00 },
	{ CCI_REG8(0xea4c), 0x00 },
	{ CCI_REG8(0xea4d), 0x00 },
	{ CCI_REG8(0xea4e), 0x00 },
	{ CCI_REG8(0xf000), 0x00 },
	{ CCI_REG8(0xf001), 0x10 },
	{ CCI_REG8(0xf00c), 0x00 },
	{ CCI_REG8(0xf00d), 0x40 },
	{ CCI_REG8(0xf030), 0x00 },
	{ CCI_REG8(0xf031), 0x10 },
	{ CCI_REG8(0xf03c), 0x00 },
	{ CCI_REG8(0xf03d), 0x40 },
	{ CCI_REG8(0xf44b), 0x80 },
	{ CCI_REG8(0xf44c), 0x10 },
	{ CCI_REG8(0xf44d), 0x06 },
	{ CCI_REG8(0xf44e), 0x80 },
	{ CCI_REG8(0xf44f), 0x10 },
	{ CCI_REG8(0xf450), 0x06 },
	{ CCI_REG8(0xf451), 0x80 },
	{ CCI_REG8(0xf452), 0x10 },
	{ CCI_REG8(0xf453), 0x06 },
	{ CCI_REG8(0xf454), 0x80 },
	{ CCI_REG8(0xf455), 0x10 },
	{ CCI_REG8(0xf456), 0x06 },
	{ CCI_REG8(0xf457), 0x80 },
	{ CCI_REG8(0xf458), 0x10 },
	{ CCI_REG8(0xf459), 0x06 },
	{ CCI_REG8(0xf478), 0x20 },
	{ CCI_REG8(0xf479), 0x80 },
	{ CCI_REG8(0xf47a), 0x80 },
	{ CCI_REG8(0xf47b), 0x20 },
	{ CCI_REG8(0xf47c), 0x80 },
	{ CCI_REG8(0xf47d), 0x80 },
	{ CCI_REG8(0xf47e), 0x20 },
	{ CCI_REG8(0xf47f), 0x80 },
	{ CCI_REG8(0xf480), 0x80 },
	{ CCI_REG8(0xf481), 0x20 },
	{ CCI_REG8(0xf482), 0x60 },
	{ CCI_REG8(0xf483), 0x80 },
	{ CCI_REG8(0xf484), 0x20 },
	{ CCI_REG8(0xf485), 0x60 },
	{ CCI_REG8(0xf486), 0x80 },
	{ CCI_REG8(0x9852), 0x00 },
	{ CCI_REG8(0x9954), 0x0f },
	{ CCI_REG8(0xa7ad), 0x01 },
	{ CCI_REG8(0xa7cb), 0x01 },
	{ CCI_REG8(0xae09), 0xff },
	{ CCI_REG8(0xae0a), 0xff },
	{ CCI_REG8(0xae12), 0x58 },
	{ CCI_REG8(0xae13), 0x58 },
	{ CCI_REG8(0xae15), 0x10 },
	{ CCI_REG8(0xae16), 0x10 },
	{ CCI_REG8(0xaf05), 0x48 },
	{ CCI_REG8(0xb07c), 0x02 },
	{ CCI_REG8(0x0106), 0x01 },
};

static const struct cci_reg_sequence mode_4000x3000_regs[] = {
	{ CCI_REG8(0x0112), 0x0a },
	{ CCI_REG8(0x0113), 0x0a },
	{ CCI_REG8(0x0114), 0x02 },
	{ CCI_REG8(0x0342), 0x1e },
	{ CCI_REG8(0x0343), 0xc0 },
	{ CCI_REG8(0x0340), 0x0c },
	{ CCI_REG8(0x0341), 0x00 },
	{ CCI_REG8(0x0344), 0x00 },
	{ CCI_REG8(0x0345), 0x00 },
	{ CCI_REG8(0x0346), 0x00 },
	{ CCI_REG8(0x0347), 0x00 },
	{ CCI_REG8(0x0348), 0x1f },
	{ CCI_REG8(0x0349), 0x3f },
	{ CCI_REG8(0x034a), 0x17 },
	{ CCI_REG8(0x034b), 0x6f },
	{ CCI_REG8(0x0220), 0x62 },
	{ CCI_REG8(0x0222), 0x01 },
	{ CCI_REG8(0x0900), 0x01 },
	{ CCI_REG8(0x0901), 0x22 },
	{ CCI_REG8(0x0902), 0x08 },
	{ CCI_REG8(0x3140), 0x00 },
	{ CCI_REG8(0x3246), 0x81 },
	{ CCI_REG8(0x3247), 0x81 },
	{ CCI_REG8(0x3f15), 0x00 },
	{ CCI_REG8(0x0401), 0x00 },
	{ CCI_REG8(0x0404), 0x00 },
	{ CCI_REG8(0x0405), 0x10 },
	{ CCI_REG8(0x0408), 0x00 },
	{ CCI_REG8(0x0409), 0x00 },
	{ CCI_REG8(0x040a), 0x00 },
	{ CCI_REG8(0x040b), 0x00 },
	{ CCI_REG8(0x040c), 0x0f },
	{ CCI_REG8(0x040d), 0xa0 },
	{ CCI_REG8(0x040e), 0x0b },
	{ CCI_REG8(0x040f), 0xb8 },
	{ CCI_REG8(0x034c), 0x0f },
	{ CCI_REG8(0x034d), 0xa0 },
	{ CCI_REG8(0x034e), 0x0b },
	{ CCI_REG8(0x034f), 0xb8 },
	{ CCI_REG8(0x0301), 0x05 },
	{ CCI_REG8(0x0303), 0x02 },
	{ CCI_REG8(0x0305), 0x02 },
	{ CCI_REG8(0x0306), 0x00 },
	{ CCI_REG8(0x0307), 0xbd },
	{ CCI_REG8(0x030b), 0x01 },
	{ CCI_REG8(0x030d), 0x0c },
	{ CCI_REG8(0x030e), 0x03 },
	{ CCI_REG8(0x030f), 0x79 },
	{ CCI_REG8(0x0310), 0x01 },
	{ CCI_REG8(0x3620), 0x00 },
	{ CCI_REG8(0x3621), 0x00 },
	{ CCI_REG8(0x3c11), 0x04 },
	{ CCI_REG8(0x3c12), 0x03 },
	{ CCI_REG8(0x3c13), 0x2d },
	{ CCI_REG8(0x3f0c), 0x01 },
	{ CCI_REG8(0x3f14), 0x00 },
	{ CCI_REG8(0x3f80), 0x01 },
	{ CCI_REG8(0x3f81), 0x90 },
	{ CCI_REG8(0x3f8c), 0x00 },
	{ CCI_REG8(0x3f8d), 0x14 },
	{ CCI_REG8(0x3ff8), 0x01 },
	{ CCI_REG8(0x3ff9), 0x2a },
	{ CCI_REG8(0x3ffe), 0x00 },
	{ CCI_REG8(0x3fff), 0x6c },
	{ CCI_REG8(0x0202), 0x0b },
	{ CCI_REG8(0x0203), 0xd0 },
	{ CCI_REG8(0x0224), 0x01 },
	{ CCI_REG8(0x0225), 0xf4 },
	{ CCI_REG8(0x3fe0), 0x01 },
	{ CCI_REG8(0x3fe1), 0xf4 },
	{ CCI_REG8(0x0204), 0x00 },
	{ CCI_REG8(0x0205), 0x70 },
	{ CCI_REG8(0x0216), 0x00 },
	{ CCI_REG8(0x0217), 0x70 },
	{ CCI_REG8(0x0218), 0x01 },
	{ CCI_REG8(0x0219), 0x00 },
	{ CCI_REG8(0x020e), 0x01 },
	{ CCI_REG8(0x020f), 0x00 },
	{ CCI_REG8(0x0210), 0x01 },
	{ CCI_REG8(0x0211), 0x00 },
	{ CCI_REG8(0x0212), 0x01 },
	{ CCI_REG8(0x0213), 0x00 },
	{ CCI_REG8(0x0214), 0x01 },
	{ CCI_REG8(0x0215), 0x00 },
	{ CCI_REG8(0x3fe2), 0x00 },
	{ CCI_REG8(0x3fe3), 0x70 },
	{ CCI_REG8(0x3fe4), 0x01 },
	{ CCI_REG8(0x3fe5), 0x00 },
	{ CCI_REG8(0x3e20), 0x02 },
	{ CCI_REG8(0x3e37), 0x00 },
	{ CCI_REG8(0x3e20), 0x02 },
	{ CCI_REG8(0x3e3b), 0x01 },
	{ CCI_REG8(0x4434), 0x01 },
};

static const struct imx581_mode supported_modes[] = {
	{
		.width = 4000,
		.height = 3000,
		.code = MEDIA_BUS_FMT_SRGGB10_1X10,
		.hblank = 7872 - 4000,
		.vblank = 6144 - 3000,
		.vblank_min = 3072 - 3000,
		.vblank_max = 0xffff - 3000,
		.pclk = 1451520000,
		.link_freq_idx = 0,
		.reg_list = {
			.num_of_regs = ARRAY_SIZE(mode_4000x3000_regs),
			.regs = mode_4000x3000_regs,
		},
	},
};

static inline struct imx581 *to_imx581(struct v4l2_subdev *subdev)
{
	return container_of(subdev, struct imx581, sd);
}

static int imx581_write_reg(struct imx581 *imx581, u16 reg,
			    u32 len, u32 val)
{
	if (len == 1)
		return cci_write(imx581->regmap, CCI_REG8(reg), val, NULL);
	if (len == 2)
		return cci_write(imx581->regmap, CCI_REG16(reg), val, NULL);
	return -EINVAL;
}

static int imx581_write_regs(struct imx581 *imx581,
			     const struct cci_reg_sequence *regs, u32 len)
{
	return cci_multi_reg_write(imx581->regmap, regs, len, NULL);
}

static int imx581_update_controls(struct imx581 *imx581,
				  const struct imx581_mode *mode)
{
	int ret;

	ret = __v4l2_ctrl_s_ctrl(imx581->link_freq_ctrl, mode->link_freq_idx);
	if (ret)
		return ret;

	ret = __v4l2_ctrl_s_ctrl_int64(imx581->pclk_ctrl, mode->pclk);
	if (ret)
		return ret;

	ret = __v4l2_ctrl_modify_range(imx581->hblank_ctrl, mode->hblank,
				       mode->hblank, 1, mode->hblank);
	if (ret)
		return ret;

	ret = __v4l2_ctrl_modify_range(imx581->vblank_ctrl, mode->vblank_min,
				       mode->vblank_max, 1, mode->vblank);
	if (ret)
		return ret;

	ret = __v4l2_ctrl_s_ctrl(imx581->vblank_ctrl, mode->vblank);
	if (ret)
		return ret;

	/* The height may change even when the VBLANK value stays the same. */
	return __v4l2_ctrl_modify_range(imx581->exp_ctrl, IMX581_EXPOSURE_MIN,
			mode->height + mode->vblank - IMX581_EXPOSURE_OFFSET,
			1, IMX581_EXPOSURE_DEFAULT);
}

static int imx581_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct imx581 *imx581 =
		container_of(ctrl->handler, struct imx581, ctrl_handler);
	u32 lpfr = imx581->cur_mode->height + imx581->vblank_ctrl->val;
	int ret, release_ret;

	if (ctrl->id == V4L2_CID_VBLANK) {
		ret = __v4l2_ctrl_modify_range(imx581->exp_ctrl,
				IMX581_EXPOSURE_MIN, lpfr - IMX581_EXPOSURE_OFFSET,
				1, min_t(u32, IMX581_EXPOSURE_DEFAULT,
					 lpfr - IMX581_EXPOSURE_OFFSET));
		if (ret)
			return ret;
	}

	ret = pm_runtime_get_if_in_use(imx581->dev);
	if (ret <= 0)
		return ret;

	if (ctrl->id == V4L2_CID_TEST_PATTERN) {
		ret = imx581_write_reg(imx581, IMX581_REG_TEST_PATTERN, 1,
				       ctrl->val ? 2 : 0);
		goto out_pm;
	}

	/* Frame length, exposure and gain must take effect in the same frame. */
	ret = imx581_write_reg(imx581, IMX581_REG_HOLD, 1, 1);
	if (ret)
		goto out_pm;

	switch (ctrl->id) {
	case V4L2_CID_VBLANK:
		ret = imx581_write_reg(imx581, IMX581_REG_LPFR, 2, lpfr);
		if (ret)
			break;
		fallthrough;
	case V4L2_CID_EXPOSURE:
		/* Exposure is the master of the exposure/analogue gain cluster. */
		ret = imx581_write_reg(imx581, IMX581_REG_EXPOSURE_CIT, 2,
				       imx581->exp_ctrl->val);
		if (!ret)
			ret = imx581_write_reg(imx581, IMX581_REG_AGAIN, 2,
					       imx581->again_ctrl->val);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	release_ret = imx581_write_reg(imx581, IMX581_REG_HOLD, 1, 0);
	if (!ret)
		ret = release_ret;
out_pm:
	pm_runtime_put(imx581->dev);
	return ret;
}

static const struct v4l2_ctrl_ops imx581_ctrl_ops = {
	.s_ctrl = imx581_set_ctrl,
};

static int imx581_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index > 0)
		return -EINVAL;

	code->code = supported_modes[0].code;

	return 0;
}

static int imx581_enum_frame_size(struct v4l2_subdev *sd,
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

static void imx581_fill_pad_format(struct imx581 *imx581,
				   const struct imx581_mode *mode,
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

static int imx581_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *sd_state)
{
	struct imx581 *imx581 = to_imx581(sd);
	struct v4l2_subdev_format fmt = { 0 };

	fmt.which = sd_state ? V4L2_SUBDEV_FORMAT_TRY : V4L2_SUBDEV_FORMAT_ACTIVE;
	imx581_fill_pad_format(imx581, &supported_modes[0], &fmt);
	*v4l2_subdev_state_get_format(sd_state, 0) = fmt.format;

	return 0;
}

static int imx581_get_pad_format(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_format *fmt)
{
	struct imx581 *imx581 = to_imx581(sd);

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
		struct v4l2_mbus_framefmt *try_fmt =
			v4l2_subdev_state_get_format(sd_state, fmt->pad);

		fmt->format = *try_fmt;
	} else {
		imx581_fill_pad_format(imx581, imx581->cur_mode, fmt);
	}

	return 0;
}

static int imx581_set_pad_format(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_format *fmt)
{
	struct imx581 *imx581 = to_imx581(sd);
	const struct imx581_mode *mode, *old_mode;
	int ret = 0;

	mutex_lock(&imx581->mutex);

	mode = v4l2_find_nearest_size(supported_modes,
				      ARRAY_SIZE(supported_modes),
				      width, height,
				      fmt->format.width, fmt->format.height);

	imx581_fill_pad_format(imx581, mode, fmt);

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY) {
		struct v4l2_mbus_framefmt *try_fmt =
			v4l2_subdev_state_get_format(sd_state, fmt->pad);

		*try_fmt = fmt->format;
	} else {
		if (imx581->streaming) {
			ret = -EBUSY;
			goto unlock;
		}
		old_mode = imx581->cur_mode;
		imx581->cur_mode = mode;
		ret = imx581_update_controls(imx581, mode);
		if (ret) {
			imx581->cur_mode = old_mode;
			imx581_update_controls(imx581, old_mode);
		}
	}

unlock:
	mutex_unlock(&imx581->mutex);

	return ret;
}

static int imx581_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *sd_state,
				struct v4l2_subdev_selection *sel)
{
	struct imx581 *imx581 = to_imx581(sd);
	const struct imx581_mode *mode;
	struct v4l2_mbus_framefmt *fmt;

	if (sel->pad)
		return -EINVAL;

	switch (sel->target) {
	case V4L2_SEL_TGT_NATIVE_SIZE:
	case V4L2_SEL_TGT_CROP_BOUNDS:
	case V4L2_SEL_TGT_CROP_DEFAULT:
		sel->r = (struct v4l2_rect) {
			.width = IMX581_PIXEL_ARRAY_WIDTH,
			.height = IMX581_PIXEL_ARRAY_HEIGHT,
		};
		return 0;
	case V4L2_SEL_TGT_CROP:
		mutex_lock(&imx581->mutex);
		if (sel->which == V4L2_SUBDEV_FORMAT_TRY) {
			fmt = v4l2_subdev_state_get_format(sd_state, sel->pad);
			mode = v4l2_find_nearest_size(supported_modes,
					ARRAY_SIZE(supported_modes), width, height,
					fmt->width, fmt->height);
		} else {
			mode = imx581->cur_mode;
		}
		/* The initial mode reads the complete array with 2x2 binning. */
		sel->r.width = mode->width * 2;
		sel->r.height = mode->height * 2;
		sel->r.left = (IMX581_PIXEL_ARRAY_WIDTH - sel->r.width) / 2;
		sel->r.top = (IMX581_PIXEL_ARRAY_HEIGHT - sel->r.height) / 2;
		mutex_unlock(&imx581->mutex);
		return 0;
	default:
		return -EINVAL;
	}
}

static int imx581_start_streaming(struct imx581 *imx581)
{
	const struct imx581_reg_list *reg_list;
	int ret;

	/* Restore the sensor tuning after each power cycle. */
	ret = imx581_write_regs(imx581, imx581_init_regs,
				ARRAY_SIZE(imx581_init_regs));
	if (ret) {
		dev_err(imx581->dev, "failed to set init registers: %d\n", ret);
		return ret;
	}

	/* Apply mode registers */
	reg_list = &imx581->cur_mode->reg_list;
	ret = imx581_write_regs(imx581, reg_list->regs, reg_list->num_of_regs);
	if (ret) {
		dev_err(imx581->dev, "failed to set mode registers: %d\n", ret);
		return ret;
	}

	/* Set V4L2 controls */
	ret = __v4l2_ctrl_handler_setup(&imx581->ctrl_handler);
	if (ret)
		return ret;

	/* Start streaming */
	ret = imx581_write_reg(imx581, IMX581_REG_MODE_SELECT, 1,
			       IMX581_MODE_STREAMING);
	if (ret) {
		dev_err(imx581->dev, "failed to start streaming: %d\n", ret);
		return ret;
	}

	/* Sensor stabilization delay */
	usleep_range(10000, 12000);

	return 0;
}

static int imx581_stop_streaming(struct imx581 *imx581)
{
	return imx581_write_reg(imx581, IMX581_REG_MODE_SELECT,
				1, IMX581_MODE_STANDBY);
}

static int imx581_set_stream(struct v4l2_subdev *sd, int enable)
{
	struct imx581 *imx581 = to_imx581(sd);
	int ret = 0;

	mutex_lock(&imx581->mutex);
	if (imx581->streaming == !!enable)
		goto unlock;

	if (enable) {
		ret = pm_runtime_resume_and_get(imx581->dev);
		if (ret)
			goto unlock;
		ret = imx581_start_streaming(imx581);
		if (ret) {
			imx581_stop_streaming(imx581);
			pm_runtime_put(imx581->dev);
			goto unlock;
		}
	} else {
		ret = imx581_stop_streaming(imx581);
		/* Release the streaming reference even if the I2C write fails. */
		pm_runtime_put(imx581->dev);
	}
	imx581->streaming = !!enable;
unlock:
	mutex_unlock(&imx581->mutex);
	return ret;
}

static int imx581_detect(struct imx581 *imx581)
{
	u64 val;
	int ret;

	dev_dbg(imx581->dev, "reading sensor ID over CCI\n");
	ret = cci_read(imx581->regmap, CCI_REG16(IMX581_REG_ID), &val, NULL);
	if (ret)
		return ret;
	if (val != IMX581_ID)
		return dev_err_probe(imx581->dev, -ENODEV,
				     "unexpected sensor ID 0x%04llx\n", val);

	dev_info(imx581->dev, "Sony IMX581 identified (id 0x%04llx)\n", val);
	return 0;
}

static int imx581_parse_hw_config(struct imx581 *imx581)
{
	struct fwnode_handle *fwnode = dev_fwnode(imx581->dev);
	struct v4l2_fwnode_endpoint bus_cfg = {
		.bus_type = V4L2_MBUS_CSI2_CPHY
	};
	struct fwnode_handle *ep;
	unsigned long rate;
	unsigned int i;
	int ret;

	if (!fwnode)
		return -ENXIO;

	imx581->reset_gpio = devm_gpiod_get_optional(imx581->dev, "reset",
						     GPIOD_OUT_HIGH);
	if (IS_ERR(imx581->reset_gpio)) {
		dev_err(imx581->dev, "failed to get reset gpio %pe\n",
			imx581->reset_gpio);
		return PTR_ERR(imx581->reset_gpio);
	}

	imx581->inclk = devm_v4l2_sensor_clk_get(imx581->dev, NULL);
	if (IS_ERR(imx581->inclk))
		return dev_err_probe(imx581->dev, PTR_ERR(imx581->inclk),
				     "could not get inclk\n");

	rate = clk_get_rate(imx581->inclk);
	if (rate != IMX581_INCLK_RATE) {
		dev_err(imx581->dev, "inclk frequency mismatch: expected %d, got %lu\n",
			IMX581_INCLK_RATE, rate);
		return -EINVAL;
	}

	for (i = 0; i < ARRAY_SIZE(imx581_supply_names); i++)
		imx581->supplies[i].supply = imx581_supply_names[i];

	ret = devm_regulator_bulk_get(imx581->dev,
				      ARRAY_SIZE(imx581_supply_names),
				      imx581->supplies);
	if (ret)
		return ret;

	ep = fwnode_graph_get_next_endpoint(fwnode, NULL);
	if (!ep)
		return -ENXIO;

	ret = v4l2_fwnode_endpoint_alloc_parse(ep, &bus_cfg);
	fwnode_handle_put(ep);
	if (ret)
		return ret;

	if (bus_cfg.bus.mipi_csi2.num_data_lanes != IMX581_NUM_DATA_LANES) {
		dev_err(imx581->dev,
			"number of CSI2 data lanes %d is not supported\n",
			bus_cfg.bus.mipi_csi2.num_data_lanes);
		ret = -EINVAL;
		goto done_endpoint_free;
	}

	if (!bus_cfg.nr_of_link_frequencies) {
		dev_err(imx581->dev, "no link frequencies defined\n");
		ret = -EINVAL;
		goto done_endpoint_free;
	}

	for (i = 0; i < bus_cfg.nr_of_link_frequencies; i++) {
		if (bus_cfg.link_frequencies[i] == IMX581_LINK_FREQ)
			goto done_endpoint_free;
	}

	ret = -EINVAL;

done_endpoint_free:
	v4l2_fwnode_endpoint_free(&bus_cfg);

	return ret;
}

static const struct v4l2_subdev_video_ops imx581_video_ops = {
	.s_stream = imx581_set_stream,
};

static const struct v4l2_subdev_pad_ops imx581_pad_ops = {
	.enum_mbus_code = imx581_enum_mbus_code,
	.enum_frame_size = imx581_enum_frame_size,
	.get_fmt = imx581_get_pad_format,
	.set_fmt = imx581_set_pad_format,
	.get_selection = imx581_get_selection,
};

static const struct v4l2_subdev_ops imx581_subdev_ops = {
	.video = &imx581_video_ops,
	.pad = &imx581_pad_ops,
};

static const struct v4l2_subdev_internal_ops imx581_internal_ops = {
	.init_state = imx581_init_state,
};

static void imx581_disable_supplies(struct imx581 *imx581)
{
	static const unsigned int order[] = { 3, 2, 4, 1, 0 };
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(order); i++) {
		dev_dbg(imx581->dev, "disabling %s supply\n",
			imx581_supply_names[order[i]]);
		regulator_disable(imx581->supplies[order[i]].consumer);
	}
}

static int imx581_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx581 *imx581 = to_imx581(sd);
	unsigned int i;
	int ret;

	gpiod_set_value_cansleep(imx581->reset_gpio, 1);

	/* VIO, auxiliary, analog, digital, then autofocus module supplies. */
	for (i = 0; i < ARRAY_SIZE(imx581_supply_names); i++) {
		dev_dbg(dev, "enabling %s supply\n", imx581_supply_names[i]);
		ret = regulator_enable(imx581->supplies[i].consumer);
		if (ret) {
			dev_err(dev, "failed to enable %s: %d\n",
				imx581_supply_names[i], ret);
			goto error_supplies;
		}
		dev_dbg(dev, "%s supply enabled\n", imx581_supply_names[i]);
		if (i >= 2)
			usleep_range(1000, 2000);
	}

	dev_dbg(dev, "enabling input clock\n");
	ret = clk_prepare_enable(imx581->inclk);
	if (ret)
		goto error_supplies;
	usleep_range(2000, 3000);

	dev_dbg(dev, "releasing sensor reset\n");
	gpiod_set_value_cansleep(imx581->reset_gpio, 0);
	usleep_range(12000, 15000);
	dev_dbg(dev, "sensor power sequence complete\n");

	return 0;

error_supplies:
	while (i--)
		regulator_disable(imx581->supplies[i].consumer);
	return ret;
}

static int imx581_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct imx581 *imx581 = to_imx581(sd);

	dev_dbg(dev, "asserting sensor reset\n");
	gpiod_set_value_cansleep(imx581->reset_gpio, 1);
	usleep_range(1000, 2000);
	dev_dbg(dev, "disabling input clock\n");
	clk_disable_unprepare(imx581->inclk);
	usleep_range(1000, 2000);
	imx581_disable_supplies(imx581);

	return 0;
}

static int imx581_init_controls(struct imx581 *imx581)
{
	struct v4l2_ctrl_handler *ctrl_hdlr = &imx581->ctrl_handler;
	const struct imx581_mode *mode = imx581->cur_mode;
	struct v4l2_fwnode_device_properties props;
	u32 lpfr;
	int ret;

	ret = v4l2_ctrl_handler_init(ctrl_hdlr, 7);
	if (ret)
		return ret;

	ctrl_hdlr->lock = &imx581->mutex;

	lpfr = mode->vblank + mode->height;
	imx581->exp_ctrl = v4l2_ctrl_new_std(ctrl_hdlr,
					     &imx581_ctrl_ops,
					     V4L2_CID_EXPOSURE,
					     IMX581_EXPOSURE_MIN,
					     lpfr - IMX581_EXPOSURE_OFFSET,
					     IMX581_EXPOSURE_STEP,
					     IMX581_EXPOSURE_DEFAULT);

	imx581->again_ctrl = v4l2_ctrl_new_std(ctrl_hdlr,
					       &imx581_ctrl_ops,
					       V4L2_CID_ANALOGUE_GAIN,
					       IMX581_AGAIN_MIN,
					       IMX581_AGAIN_MAX,
					       IMX581_AGAIN_STEP,
					       IMX581_AGAIN_DEFAULT);

	v4l2_ctrl_cluster(2, &imx581->exp_ctrl);

	v4l2_ctrl_new_std_menu_items(ctrl_hdlr, &imx581_ctrl_ops,
				     V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(imx581_test_pattern_menu) - 1,
				     0, 0, imx581_test_pattern_menu);

	imx581->vblank_ctrl = v4l2_ctrl_new_std(ctrl_hdlr,
						&imx581_ctrl_ops,
						V4L2_CID_VBLANK,
						mode->vblank_min,
						mode->vblank_max,
						1, mode->vblank);

	imx581->pclk_ctrl = v4l2_ctrl_new_std(ctrl_hdlr,
					      &imx581_ctrl_ops,
					      V4L2_CID_PIXEL_RATE,
					      mode->pclk, mode->pclk,
					      1, mode->pclk);

	imx581->hblank_ctrl = v4l2_ctrl_new_std(ctrl_hdlr,
						&imx581_ctrl_ops,
						V4L2_CID_HBLANK,
						mode->hblank, mode->hblank,
						1, mode->hblank);

	if (imx581->hblank_ctrl)
		imx581->hblank_ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	imx581->link_freq_ctrl =
		v4l2_ctrl_new_int_menu(ctrl_hdlr,
				       &imx581_ctrl_ops,
				       V4L2_CID_LINK_FREQ,
				       ARRAY_SIZE(link_freq) - 1,
				       mode->link_freq_idx,
				       link_freq);

	if (imx581->link_freq_ctrl)
		imx581->link_freq_ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	ret = ctrl_hdlr->error;
	if (ret)
		goto error;

	ret = v4l2_fwnode_device_parse(imx581->dev, &props);
	if (ret)
		goto error;

	ret = v4l2_ctrl_new_fwnode_properties(ctrl_hdlr, &imx581_ctrl_ops,
					      &props);
	if (ret)
		goto error;

	return 0;

error:
	v4l2_ctrl_handler_free(ctrl_hdlr);
	return ret;
}

static int imx581_probe(struct i2c_client *client)
{
	struct imx581 *imx581;
	const char *name = "imx581";
	int ret;

	imx581 = devm_kzalloc(&client->dev, sizeof(*imx581), GFP_KERNEL);
	if (!imx581)
		return -ENOMEM;

	imx581->dev = &client->dev;
	dev_dbg(imx581->dev, "begin sensor probe\n");
	imx581->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(imx581->regmap))
		return dev_err_probe(imx581->dev, PTR_ERR(imx581->regmap),
				     "failed to initialize CCI regmap\n");

	v4l2_i2c_subdev_init(&imx581->sd, client, &imx581_subdev_ops);
	imx581->sd.internal_ops = &imx581_internal_ops;

	dev_dbg(imx581->dev, "parsing sensor resources\n");
	ret = imx581_parse_hw_config(imx581);
	if (ret) {
		dev_err(imx581->dev, "HW configuration is not supported: %d\n", ret);
		return ret;
	}

	dev_dbg(imx581->dev, "sensor resources acquired\n");
	mutex_init(&imx581->mutex);

	ret = imx581_power_on(imx581->dev);
	if (ret) {
		dev_err(imx581->dev, "failed to power-on the sensor\n");
		goto error_mutex_destroy;
	}

	/* Check sensor module identity (0x0581) */
	ret = imx581_detect(imx581);
	if (ret) {
		dev_err(imx581->dev, "failed to find sensor: %d\n", ret);
		goto error_power_off;
	}

	/* Set default mode */
	imx581->cur_mode = &supported_modes[0];

	ret = imx581_init_controls(imx581);
	if (ret) {
		dev_err(imx581->dev, "failed to init controls: %d\n", ret);
		goto error_power_off;
	}

	imx581->sd.ctrl_handler = &imx581->ctrl_handler;
	imx581->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	imx581->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	v4l2_i2c_subdev_set_name(&imx581->sd, client, name, NULL);

	imx581->pad.flags = MEDIA_PAD_FL_SOURCE;
	ret = media_entity_pads_init(&imx581->sd.entity, 1, &imx581->pad);
	if (ret) {
		dev_err(imx581->dev, "failed to init entity pads: %d\n", ret);
		goto error_handler_free;
	}

	pm_runtime_set_active(imx581->dev);
	pm_runtime_get_noresume(imx581->dev);
	pm_runtime_enable(imx581->dev);

	dev_dbg(imx581->dev, "registering sensor subdevice\n");
	ret = v4l2_async_register_subdev_sensor(&imx581->sd);
	if (ret < 0) {
		dev_err(imx581->dev,
			"failed to register async subdev: %d\n", ret);
		goto error_media_entity;
	}

	dev_dbg(imx581->dev, "releasing sensor probe power reference\n");
	pm_runtime_put(imx581->dev);
	dev_dbg(imx581->dev, "sensor probe complete\n");

	return 0;

error_media_entity:
	pm_runtime_disable(imx581->dev);
	pm_runtime_put_noidle(imx581->dev);
	pm_runtime_set_suspended(imx581->dev);
	media_entity_cleanup(&imx581->sd.entity);
error_handler_free:
	v4l2_ctrl_handler_free(imx581->sd.ctrl_handler);
error_power_off:
	imx581_power_off(imx581->dev);
error_mutex_destroy:
	mutex_destroy(&imx581->mutex);

	return ret;
}

static void imx581_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct imx581 *imx581 = to_imx581(sd);

	v4l2_async_unregister_subdev(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(sd->ctrl_handler);

	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		imx581_power_off(&client->dev);
	pm_runtime_set_suspended(&client->dev);

	mutex_destroy(&imx581->mutex);
}

static const struct dev_pm_ops imx581_pm_ops = {
	SET_RUNTIME_PM_OPS(imx581_power_off, imx581_power_on, NULL)
};

static const struct of_device_id imx581_of_match[] = {
	{ .compatible = "sony,imx581" },
	{ }
};
MODULE_DEVICE_TABLE(of, imx581_of_match);

static struct i2c_driver imx581_driver = {
	.probe = imx581_probe,
	.remove = imx581_remove,
	.driver = {
		.name = "imx581",
		.pm = &imx581_pm_ops,
		.of_match_table = imx581_of_match,
	},
};

module_i2c_driver(imx581_driver);

MODULE_DESCRIPTION("Sony IMX581 sensor driver");
MODULE_LICENSE("GPL");
