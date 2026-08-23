// SPDX-License-Identifier: GPL-2.0-only
/*
 * Synaptics S3910 TouchComm (TCM) Touchscreen Driver
 *
 * Designed for mainline Linux kernel (Linux 6.x / 7.x)
 * Target device: OnePlus 12 (SM8650 / waffle) & Synaptics TCM2 series
 *
 * Copyright (C) 2017-2020 Synaptics Incorporated.
 * Copyright (C) 2026 Antigravity OS / Linux Porting Project.
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/unaligned.h>

#define DRIVER_NAME "synaptics_s3910"

/* Protocol constants */
#define TCM_HEADER_MARKER               0xA5
#define TCM_HEADER_SIZE                 4
#define TCM_BUFFER_SIZE                 1024

/* TCM Commands */
#define TCM_CMD_NONE                    0x00
#define TCM_CMD_CONTINUE_WRITE          0x01
#define TCM_CMD_IDENTIFY                0x02
#define TCM_CMD_RESET                   0x04
#define TCM_CMD_ENABLE_REPORT           0x05
#define TCM_CMD_DISABLE_REPORT          0x06
#define TCM_CMD_GET_APPLICATION_INFO    0x20
#define TCM_CMD_GET_DYNAMIC_CONFIG      0x23
#define TCM_CMD_SET_DYNAMIC_CONFIG      0x24
#define TCM_CMD_GET_TOUCH_REPORT_CFG    0x25
#define TCM_CMD_ENTER_DEEP_SLEEP        0x2C
#define TCM_CMD_EXIT_DEEP_SLEEP         0x2D

/* TCM Status Codes */
#define TCM_STATUS_IDLE                 0x00
#define TCM_STATUS_OK                   0x01
#define TCM_STATUS_CONTINUED_READ       0x03
#define TCM_STATUS_NO_REPORT            0x04
#define TCM_STATUS_ERROR                0x0F
#define TCM_STATUS_INVALID              0xFF

/* TCM Report Types */
#define TCM_REPORT_IDENTIFY             0x10
#define TCM_REPORT_TOUCH                0x11

/* Touch Report Config Codes */
#define TOUCH_REPORT_END                    0x00
#define TOUCH_REPORT_FOREACH_ACTIVE_OBJECT  0x01
#define TOUCH_REPORT_FOREACH_OBJECT         0x02
#define TOUCH_REPORT_FOREACH_END            0x03
#define TOUCH_REPORT_PAD_TO_NEXT_BYTE       0x04
#define TOUCH_REPORT_TIMESTAMP              0x05
#define TOUCH_REPORT_OBJECT_N_INDEX         0x06
#define TOUCH_REPORT_OBJECT_N_CLASSIF       0x07
#define TOUCH_REPORT_OBJECT_N_X_POS         0x08
#define TOUCH_REPORT_OBJECT_N_Y_POS         0x09
#define TOUCH_REPORT_OBJECT_N_Z             0x0A
#define TOUCH_REPORT_OBJECT_N_X_WIDTH       0x0B
#define TOUCH_REPORT_OBJECT_N_Y_WIDTH       0x0C
#define TOUCH_REPORT_NUM_ACTIVE_OBJECTS     0x18
#define TOUCH_REPORT_GESTURE_ID             0x10

/* Object Status */
#define OBJECT_STATUS_LIFT                  0x00
#define OBJECT_STATUS_FINGER                0x01
#define OBJECT_STATUS_GLOVED                0x02
#define OBJECT_STATUS_STYLUS                0x03
#define OBJECT_STATUS_PALM                  0x06

#define MAX_TOUCH_SLOTS                     10
#define MAX_RAW_COORD_X                     23040
#define MAX_RAW_COORD_Y                     50688
#define DEFAULT_DISPLAY_X                   1440
#define DEFAULT_DISPLAY_Y                   3168

struct syna_touch_object {
	u8 status;
	u32 x;
	u32 y;
	u32 z;
	u32 x_width;
	u32 y_width;
};

struct syna_touch_data {
	u32 num_active_objects;
	struct syna_touch_object objects[MAX_TOUCH_SLOTS];
};

struct syna_tcm_id_info {
	u8 version;
	u8 mode;
	u8 part_number[16];
	u8 build_id[4];
	u8 max_write_size[2];
	u8 max_read_size[2];
	u8 reserved[8];
};

struct syna_tcm_app_info {
	u8 version[2];
	u8 status[2];
	u8 static_config_size[2];
	u8 dynamic_config_size[2];
	u8 app_config_start_write_block[2];
	u8 app_config_size[2];
	u8 max_touch_report_config_size[2];
	u8 max_touch_report_payload_size[2];
	u8 customer_config_id[16];
	u8 max_x[2];
	u8 max_y[2];
	u8 max_objects[2];
	u8 num_of_buttons[2];
	u8 num_of_image_rows[2];
	u8 num_of_image_cols[2];
	u8 has_hybrid_data[2];
	u8 num_of_force_elecs[2];
};

struct syna_tcm {
	struct spi_device *spi;
	struct input_dev *input;
	struct touchscreen_properties props;

	struct regulator *vdd;
	struct gpio_desc *avdd_gpio;
	struct gpio_desc *reset_gpio;

	struct mutex io_mutex;

	u8 *tx_buf;
	u8 *rx_buf;
	u32 buf_size;

	u8 *touch_config;
	u32 touch_config_len;

	struct syna_tcm_id_info id_info;
	struct syna_tcm_app_info app_info;

	u32 max_x;
	u32 max_y;
	u32 raw_max_x;
	u32 raw_max_y;

	bool prev_touch_state[MAX_TOUCH_SLOTS];
	bool suspended;
};

static int syna_tcm_extract_bits(const u8 *buf, u32 buf_len, u32 bit_offset,
				 u32 bit_count, u32 *out)
{
	u32 output = 0;
	u32 remaining = bit_count;
	u32 boff = bit_offset % 8;
	u32 byte_idx = bit_offset / 8;

	if (!buf || bit_count == 0 || bit_count > 32)
		return -EINVAL;

	if (bit_offset + bit_count > buf_len * 8) {
		*out = 0;
		return 0;
	}

	while (remaining) {
		u32 available = 8 - boff;
		u32 take = min(available, remaining);
		u8 mask = (0xFF >> (8 - take));
		u8 byte_val = (buf[byte_idx] >> boff) & mask;

		output |= ((u32)byte_val << (bit_count - remaining));

		boff = 0;
		byte_idx++;
		remaining -= take;
	}

	*out = output;
	return 0;
}

static int syna_tcm_spi_read_raw(struct syna_tcm *ts, u8 *rx, u32 len)
{
	struct spi_transfer xfer = {
		.tx_buf = ts->tx_buf,
		.rx_buf = ts->rx_buf,
		.len = len,
	};
	int ret;

	if (len > ts->buf_size)
		return -EINVAL;

	memset(ts->tx_buf, 0xFF, len);
	ret = spi_sync_transfer(ts->spi, &xfer, 1);
	if (!ret && rx)
		memcpy(rx, ts->rx_buf, len);

	return ret;
}

static int syna_tcm_spi_write_raw(struct syna_tcm *ts, const u8 *tx, u32 len)
{
	struct spi_transfer xfer = {
		.tx_buf = ts->tx_buf,
		.rx_buf = NULL,
		.len = len,
	};

	if (len > ts->buf_size)
		return -EINVAL;

	memcpy(ts->tx_buf, tx, len);
	return spi_sync_transfer(ts->spi, &xfer, 1);
}

static int syna_tcm_read_message(struct syna_tcm *ts, u8 *status_report_code,
				 u8 *payload, u32 max_payload_len, u32 *actual_payload_len)
{
	u8 header[TCM_HEADER_SIZE];
	u32 payload_len;
	u32 remaining;
	u32 offset = 0;
	int ret;

	ret = syna_tcm_spi_read_raw(ts, header, TCM_HEADER_SIZE);
	if (ret < 0) {
		dev_err(&ts->spi->dev, "Failed to read header: %d\n", ret);
		return ret;
	}

	if (header[0] != TCM_HEADER_MARKER) {
		return -ENODEV;
	}

	*status_report_code = header[1];
	payload_len = get_unaligned_le16(&header[2]);

	if (actual_payload_len)
		*actual_payload_len = payload_len;

	if (payload_len == 0)
		return 0;

	if (payload_len > max_payload_len) {
		dev_warn(&ts->spi->dev, "Payload len %u > max %u, truncating\n",
			 payload_len, max_payload_len);
		payload_len = max_payload_len;
	}

	remaining = payload_len;
	while (remaining > 0) {
		u32 chunk_len = min_t(u32, remaining, ts->buf_size - 2);
		u8 temp[TCM_BUFFER_SIZE];

		ret = syna_tcm_spi_read_raw(ts, temp, chunk_len + 2);
		if (ret < 0) {
			dev_err(&ts->spi->dev, "Continued read failed: %d\n", ret);
			return ret;
		}

		if (temp[1] != TCM_STATUS_CONTINUED_READ && temp[1] != *status_report_code) {
			dev_warn(&ts->spi->dev, "Unexpected chunk code: 0x%02x\n", temp[1]);
		}

		memcpy(payload + offset, &temp[2], chunk_len);
		offset += chunk_len;
		remaining -= chunk_len;
	}

	return 0;
}

static int syna_tcm_write_cmd(struct syna_tcm *ts, u8 command,
			      const u8 *payload, u32 payload_len,
			      u8 *resp_code, u8 *resp_data, u32 max_resp_len, u32 *resp_len)
{
	u8 cmd_buf[TCM_BUFFER_SIZE];
	u32 tx_len = 3 + payload_len;
	int ret, retries;

	if (tx_len > sizeof(cmd_buf))
		return -EINVAL;

	/* TouchComm v1 host-to-device command format:
	 * Byte 0: Command code
	 * Byte 1-2: Payload length (16-bit Little Endian)
	 * Byte 3..: Payload data
	 */
	cmd_buf[0] = command;
	put_unaligned_le16(payload_len, &cmd_buf[1]);

	if (payload && payload_len > 0)
		memcpy(&cmd_buf[3], payload, payload_len);

	ret = syna_tcm_spi_write_raw(ts, cmd_buf, tx_len);
	if (ret < 0) {
		dev_err(&ts->spi->dev, "Command write failed: %d\n", ret);
		return ret;
	}

	/* Poll for command completion response */
	for (retries = 0; retries < 25; retries++) {
		u8 code = TCM_STATUS_INVALID;
		u32 len = 0;

		usleep_range(5000, 8000);

		ret = syna_tcm_read_message(ts, &code, resp_data, max_resp_len, &len);
		if (ret < 0)
			continue;

		if (resp_code)
			*resp_code = code;

		if (resp_len)
			*resp_len = len;

		if (code == TCM_STATUS_OK)
			return 0;

		if (code != TCM_STATUS_IDLE && code != TCM_STATUS_NO_REPORT) {
			dev_dbg(&ts->spi->dev, "Command 0x%02x response code 0x%02x\n",
				command, code);
			return (code == TCM_STATUS_OK) ? 0 : -EIO;
		}
	}

	return -ETIMEDOUT;
}

static int syna_tcm_parse_touch_report(struct syna_tcm *ts, const u8 *report,
				       u32 report_len, struct syna_touch_data *touch)
{
	u32 idx = 0;
	u32 offset = 0;
	u32 obj = 0;
	u32 objects = 0;
	u32 next = 0;
	u32 active_objects = 0;
	bool active_only = false;
	bool num_active_set = false;
	u32 end_of_foreach = 0;

	memset(touch, 0, sizeof(*touch));

	if (!ts->touch_config || ts->touch_config_len == 0) {
		/* Fallback standard S3910 report parser if config absent */
		touch->num_active_objects = report[0] & 0x0F;
		for (obj = 0; obj < min_t(u32, MAX_TOUCH_SLOTS, touch->num_active_objects); obj++) {
			u32 base = 1 + obj * 8;
			if (base + 8 > report_len)
				break;
			touch->objects[obj].status = OBJECT_STATUS_FINGER;
			touch->objects[obj].x = get_unaligned_le16(&report[base]);
			touch->objects[obj].y = get_unaligned_le16(&report[base + 2]);
			touch->objects[obj].z = report[base + 4];
			touch->objects[obj].x_width = report[base + 5];
			touch->objects[obj].y_width = report[base + 6];
		}
		return 0;
	}

	while (idx < ts->touch_config_len) {
		u8 code = ts->touch_config[idx++];
		u32 bits, val = 0;
		int ret;

		switch (code) {
		case TOUCH_REPORT_END:
			return 0;

		case TOUCH_REPORT_FOREACH_ACTIVE_OBJECT:
			obj = 0;
			objects = 0;
			next = idx;
			active_only = true;
			break;

		case TOUCH_REPORT_FOREACH_OBJECT:
			obj = 0;
			objects = 0;
			next = idx;
			active_only = false;
			break;

		case TOUCH_REPORT_FOREACH_END:
			end_of_foreach = idx;
			if (active_only) {
				if (num_active_set) {
					objects++;
					obj++;
					if (objects < active_objects && obj < MAX_TOUCH_SLOTS)
						idx = next;
				} else if (offset < report_len * 8) {
					obj++;
					if (obj < MAX_TOUCH_SLOTS)
						idx = next;
				}
			} else {
				obj++;
				if (obj < MAX_TOUCH_SLOTS)
					idx = next;
			}
			break;

		case TOUCH_REPORT_PAD_TO_NEXT_BYTE:
			offset = roundup(offset, 8);
			break;

		case TOUCH_REPORT_NUM_ACTIVE_OBJECTS:
			bits = ts->touch_config[idx++];
			ret = syna_tcm_extract_bits(report, report_len, offset, bits, &val);
			if (ret < 0)
				return ret;
			active_objects = val;
			num_active_set = true;
			touch->num_active_objects = val;
			offset += bits;
			if (active_objects == 0 && end_of_foreach)
				idx = end_of_foreach;
			break;

		case TOUCH_REPORT_OBJECT_N_INDEX:
			bits = ts->touch_config[idx++];
			ret = syna_tcm_extract_bits(report, report_len, offset, bits, &val);
			if (ret < 0)
				return ret;
			if (val < MAX_TOUCH_SLOTS)
				obj = val;
			offset += bits;
			break;

		case TOUCH_REPORT_OBJECT_N_CLASSIF:
			bits = ts->touch_config[idx++];
			ret = syna_tcm_extract_bits(report, report_len, offset, bits, &val);
			if (ret < 0)
				return ret;
			if (obj < MAX_TOUCH_SLOTS)
				touch->objects[obj].status = (u8)val;
			offset += bits;
			break;

		case TOUCH_REPORT_OBJECT_N_X_POS:
			bits = ts->touch_config[idx++];
			ret = syna_tcm_extract_bits(report, report_len, offset, bits, &val);
			if (ret < 0)
				return ret;
			if (obj < MAX_TOUCH_SLOTS)
				touch->objects[obj].x = val;
			offset += bits;
			break;

		case TOUCH_REPORT_OBJECT_N_Y_POS:
			bits = ts->touch_config[idx++];
			ret = syna_tcm_extract_bits(report, report_len, offset, bits, &val);
			if (ret < 0)
				return ret;
			if (obj < MAX_TOUCH_SLOTS)
				touch->objects[obj].y = val;
			offset += bits;
			break;

		case TOUCH_REPORT_OBJECT_N_Z:
			bits = ts->touch_config[idx++];
			ret = syna_tcm_extract_bits(report, report_len, offset, bits, &val);
			if (ret < 0)
				return ret;
			if (obj < MAX_TOUCH_SLOTS)
				touch->objects[obj].z = val;
			offset += bits;
			break;

		case TOUCH_REPORT_OBJECT_N_X_WIDTH:
			bits = ts->touch_config[idx++];
			ret = syna_tcm_extract_bits(report, report_len, offset, bits, &val);
			if (ret < 0)
				return ret;
			if (obj < MAX_TOUCH_SLOTS)
				touch->objects[obj].x_width = val;
			offset += bits;
			break;

		case TOUCH_REPORT_OBJECT_N_Y_WIDTH:
			bits = ts->touch_config[idx++];
			ret = syna_tcm_extract_bits(report, report_len, offset, bits, &val);
			if (ret < 0)
				return ret;
			if (obj < MAX_TOUCH_SLOTS)
				touch->objects[obj].y_width = val;
			offset += bits;
			break;

		default:
			/* Skip unknown element bits */
			bits = ts->touch_config[idx++];
			offset += bits;
			break;
		}
	}

	return 0;
}

static void syna_tcm_report_events(struct syna_tcm *ts, struct syna_touch_data *touch)
{
	int i;
	int active_count = 0;

	for (i = 0; i < MAX_TOUCH_SLOTS; i++) {
		struct syna_touch_object *obj = &touch->objects[i];
		bool down = (obj->status == OBJECT_STATUS_FINGER ||
			     obj->status == OBJECT_STATUS_GLOVED ||
			     obj->status == OBJECT_STATUS_STYLUS);

		if (!down && !ts->prev_touch_state[i])
			continue;

		input_mt_slot(ts->input, i);
		input_mt_report_slot_state(ts->input, MT_TOOL_FINGER, down);

		if (down) {
			u32 x = obj->x;
			u32 y = obj->y;
			u32 z = obj->z ? obj->z : 100;
			u32 major = max(obj->x_width, obj->y_width);
			u32 minor = min(obj->x_width, obj->y_width);

			/* Scale raw hardware coordinates to display resolution */
			if (ts->raw_max_x && ts->max_x)
				x = mult_frac(x, ts->max_x, ts->raw_max_x);
			if (ts->raw_max_y && ts->max_y)
				y = mult_frac(y, ts->max_y, ts->raw_max_y);

			touchscreen_report_pos(ts->input, &ts->props, x, y, true);
			input_report_abs(ts->input, ABS_MT_PRESSURE, min_t(u32, z, 255));
			input_report_abs(ts->input, ABS_MT_TOUCH_MAJOR, major);
			input_report_abs(ts->input, ABS_MT_TOUCH_MINOR, minor);

			active_count++;
		}

		ts->prev_touch_state[i] = down;
	}

	input_mt_sync_frame(ts->input);
	input_report_key(ts->input, BTN_TOUCH, active_count > 0);
	input_sync(ts->input);
}

static irqreturn_t syna_tcm_threaded_irq(int irq, void *data)
{
	struct syna_tcm *ts = data;
	u8 code;
	u8 payload[TCM_BUFFER_SIZE];
	u32 payload_len;
	int ret, packet_count = 0;

	mutex_lock(&ts->io_mutex);

	if (ts->suspended) {
		mutex_unlock(&ts->io_mutex);
		return IRQ_HANDLED;
	}

	while (packet_count < 32) {
		code = TCM_STATUS_INVALID;
		payload_len = 0;

		ret = syna_tcm_read_message(ts, &code, payload, sizeof(payload), &payload_len);
		if (ret < 0)
			break;

		if (code == TCM_STATUS_IDLE || code == TCM_STATUS_INVALID)
			break;

		if (code == TCM_REPORT_TOUCH) {
			struct syna_touch_data touch;

			ret = syna_tcm_parse_touch_report(ts, payload, payload_len, &touch);
			if (!ret)
				syna_tcm_report_events(ts, &touch);
		} else if (code == TCM_REPORT_IDENTIFY) {
			dev_info(&ts->spi->dev, "Device reset detected via Identify report\n");
			syna_tcm_write_cmd(ts, TCM_CMD_ENABLE_REPORT, (u8[]){ TCM_REPORT_TOUCH }, 1,
					   NULL, NULL, 0, NULL);
		}

		packet_count++;
	}

	mutex_unlock(&ts->io_mutex);
	return IRQ_HANDLED;
}

static int syna_tcm_power_on(struct syna_tcm *ts)
{
	int ret;

	if (ts->vdd) {
		ret = regulator_enable(ts->vdd);
		if (ret) {
			dev_err(&ts->spi->dev, "Failed to enable vdd: %d\n", ret);
			return ret;
		}
	}

	if (ts->avdd_gpio)
		gpiod_set_value_cansleep(ts->avdd_gpio, 1);

	msleep(200);

	if (ts->reset_gpio) {
		gpiod_set_value_cansleep(ts->reset_gpio, 1);
		msleep(10);
		gpiod_set_value_cansleep(ts->reset_gpio, 0);
		msleep(80);
	}

	return 0;
}

static void syna_tcm_power_off(struct syna_tcm *ts)
{
	if (ts->reset_gpio)
		gpiod_set_value_cansleep(ts->reset_gpio, 1);

	if (ts->avdd_gpio)
		gpiod_set_value_cansleep(ts->avdd_gpio, 0);

	if (ts->vdd)
		regulator_disable(ts->vdd);
}

static int syna_tcm_init_device(struct syna_tcm *ts)
{
	u8 payload[TCM_BUFFER_SIZE];
	u32 payload_len = 0;
	u8 resp_code = 0;
	int ret;

	/* 1. Identify device */
	ret = syna_tcm_read_message(ts, &resp_code, (u8 *)&ts->id_info,
				    sizeof(ts->id_info), &payload_len);
	if (ret < 0 || resp_code != TCM_REPORT_IDENTIFY) {
		dev_info(&ts->spi->dev, "Reading initial identify failed, sending IDENTIFY cmd\n");
		ret = syna_tcm_write_cmd(ts, TCM_CMD_IDENTIFY, NULL, 0,
					 &resp_code, (u8 *)&ts->id_info,
					 sizeof(ts->id_info), &payload_len);
		if (ret < 0) {
			dev_err(&ts->spi->dev, "Identify command failed: %d\n", ret);
			return ret;
		}
	}

	dev_info(&ts->spi->dev, "Synaptics TCM: mode=0x%02x, build_id=%u, part=%.16s\n",
		 ts->id_info.mode, get_unaligned_le32(ts->id_info.build_id),
		 ts->id_info.part_number);

	/* 2. Retrieve Application info */
	ret = syna_tcm_write_cmd(ts, TCM_CMD_GET_APPLICATION_INFO, NULL, 0,
				 &resp_code, (u8 *)&ts->app_info,
				 sizeof(ts->app_info), &payload_len);
	if (!ret && resp_code == TCM_STATUS_OK) {
		u32 fw_max_x = get_unaligned_le16(ts->app_info.max_x);
		u32 fw_max_y = get_unaligned_le16(ts->app_info.max_y);
		u32 max_objs = get_unaligned_le16(ts->app_info.max_objects);

		if (fw_max_x > 0)
			ts->raw_max_x = fw_max_x;
		if (fw_max_y > 0)
			ts->raw_max_y = fw_max_y;

		dev_info(&ts->spi->dev, "App info: max_objects=%u, fw_res=%ux%u\n",
			 max_objs, ts->raw_max_x, ts->raw_max_y);
	}

	/* 3. Retrieve Touch Report Config */
	ret = syna_tcm_write_cmd(ts, TCM_CMD_GET_TOUCH_REPORT_CFG, NULL, 0,
				 &resp_code, payload, sizeof(payload), &payload_len);
	if (!ret && resp_code == TCM_STATUS_OK && payload_len > 0) {
		kfree(ts->touch_config);
		ts->touch_config = kmemdup(payload, payload_len, GFP_KERNEL);
		if (ts->touch_config)
			ts->touch_config_len = payload_len;
		dev_info(&ts->spi->dev, "Touch report config loaded (%u bytes)\n", payload_len);
	}

	/* 4. Enable Touch streaming report */
	ret = syna_tcm_write_cmd(ts, TCM_CMD_ENABLE_REPORT, (u8[]){ TCM_REPORT_TOUCH }, 1,
				 &resp_code, NULL, 0, NULL);
	if (ret < 0) {
		dev_err(&ts->spi->dev, "Failed to enable touch report: %d\n", ret);
		return ret;
	}

	return 0;
}

static int syna_tcm_probe(struct spi_device *spi)
{
	struct device *dev = &spi->dev;
	struct syna_tcm *ts;
	int ret;

	ts = devm_kzalloc(dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;

	ts->spi = spi;
	ts->buf_size = TCM_BUFFER_SIZE;
	mutex_init(&ts->io_mutex);

	ts->tx_buf = devm_kzalloc(dev, ts->buf_size, GFP_KERNEL);
	ts->rx_buf = devm_kzalloc(dev, ts->buf_size, GFP_KERNEL);
	if (!ts->tx_buf || !ts->rx_buf)
		return -ENOMEM;

	/* Parse device tree supplies and GPIOs */
	ts->vdd = devm_regulator_get_optional(dev, "vdd");
	if (IS_ERR(ts->vdd)) {
		if (PTR_ERR(ts->vdd) != -ENODEV)
			return PTR_ERR(ts->vdd);
		ts->vdd = NULL;
	}

	ts->avdd_gpio = devm_gpiod_get_optional(dev, "avdd", GPIOD_OUT_HIGH);
	if (IS_ERR(ts->avdd_gpio))
		return dev_err_probe(dev, PTR_ERR(ts->avdd_gpio), "Failed to get avdd gpio\n");

	ts->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ts->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ts->reset_gpio), "Failed to get reset gpio\n");

	/* Default coordinates for OnePlus 12 */
	ts->raw_max_x = MAX_RAW_COORD_X;
	ts->raw_max_y = MAX_RAW_COORD_Y;
	ts->max_x = DEFAULT_DISPLAY_X;
	ts->max_y = DEFAULT_DISPLAY_Y;

	/* Setup SPI controller */
	spi->mode = SPI_MODE_0;
	spi->bits_per_word = 8;
	ret = spi_setup(spi);
	if (ret)
		return dev_err_probe(dev, ret, "SPI setup failed\n");

	spi_set_drvdata(spi, ts);

	/* Power on and initialize hardware */
	ret = syna_tcm_power_on(ts);
	if (ret)
		return ret;

	ret = syna_tcm_init_device(ts);
	if (ret) {
		syna_tcm_power_off(ts);
		return dev_err_probe(dev, ret, "Device initialization failed\n");
	}

	/* Register Input Subsystem */
	ts->input = devm_input_allocate_device(dev);
	if (!ts->input) {
		syna_tcm_power_off(ts);
		return -ENOMEM;
	}

	ts->input->name = "Synaptics S3910 Touchscreen";
	ts->input->id.bustype = BUS_SPI;
	ts->input->dev.parent = dev;

	input_set_abs_params(ts->input, ABS_MT_POSITION_X, 0, ts->max_x - 1, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_POSITION_Y, 0, ts->max_y - 1, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_PRESSURE, 0, 255, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_TOUCH_MAJOR, 0, 255, 0, 0);
	input_set_abs_params(ts->input, ABS_MT_TOUCH_MINOR, 0, 255, 0, 0);

	touchscreen_parse_properties(ts->input, true, &ts->props);

	ret = input_mt_init_slots(ts->input, MAX_TOUCH_SLOTS,
				  INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
	if (ret) {
		syna_tcm_power_off(ts);
		return dev_err_probe(dev, ret, "Failed to init MT slots\n");
	}

	ret = input_register_device(ts->input);
	if (ret) {
		syna_tcm_power_off(ts);
		return dev_err_probe(dev, ret, "Failed to register input device\n");
	}

	/* Register Threaded IRQ */
	ret = devm_request_threaded_irq(dev, spi->irq, NULL,
					syna_tcm_threaded_irq,
					IRQF_ONESHOT,
					DRIVER_NAME, ts);
	if (ret) {
		syna_tcm_power_off(ts);
		return dev_err_probe(dev, ret, "Failed to request threaded IRQ %d\n", spi->irq);
	}

	dev_info(dev, "Synaptics S3910 Touchscreen registered successfully (irq=%d)\n", spi->irq);
	return 0;
}

static void syna_tcm_remove(struct spi_device *spi)
{
	struct syna_tcm *ts = spi_get_drvdata(spi);

	mutex_lock(&ts->io_mutex);
	ts->suspended = true;
	syna_tcm_write_cmd(ts, TCM_CMD_DISABLE_REPORT, (u8[]){ TCM_REPORT_TOUCH }, 1,
			   NULL, NULL, 0, NULL);
	syna_tcm_power_off(ts);
	kfree(ts->touch_config);
	mutex_unlock(&ts->io_mutex);
}

static int syna_tcm_suspend(struct device *dev)
{
	struct spi_device *spi = to_spi_device(dev);
	struct syna_tcm *ts = spi_get_drvdata(spi);

	mutex_lock(&ts->io_mutex);
	ts->suspended = true;
	disable_irq(spi->irq);
	syna_tcm_write_cmd(ts, TCM_CMD_ENTER_DEEP_SLEEP, NULL, 0, NULL, NULL, 0, NULL);
	mutex_unlock(&ts->io_mutex);

	return 0;
}

static int syna_tcm_resume(struct device *dev)
{
	struct spi_device *spi = to_spi_device(dev);
	struct syna_tcm *ts = spi_get_drvdata(spi);

	mutex_lock(&ts->io_mutex);
	syna_tcm_power_on(ts);
	syna_tcm_write_cmd(ts, TCM_CMD_ENABLE_REPORT, (u8[]){ TCM_REPORT_TOUCH }, 1,
			   NULL, NULL, 0, NULL);
	ts->suspended = false;
	enable_irq(spi->irq);
	mutex_unlock(&ts->io_mutex);

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(syna_tcm_pm_ops, syna_tcm_suspend, syna_tcm_resume);

static const struct of_device_id syna_tcm_of_match[] = {
	{ .compatible = "synaptics,s3910" },
	{ .compatible = "synaptics,tcm-spi-hbp" },
	{ }
};
MODULE_DEVICE_TABLE(of, syna_tcm_of_match);

static const struct spi_device_id syna_tcm_spi_id[] = {
	{ "s3910", 0 },
	{ "tcm-spi-hbp", 0 },
	{ DRIVER_NAME, 0 },
	{ }
};
MODULE_DEVICE_TABLE(spi, syna_tcm_spi_id);

static struct spi_driver syna_tcm_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = syna_tcm_of_match,
		.pm = pm_sleep_ptr(&syna_tcm_pm_ops),
	},
	.probe = syna_tcm_probe,
	.remove = syna_tcm_remove,
	.id_table = syna_tcm_spi_id,
};

module_spi_driver(syna_tcm_driver);

MODULE_AUTHOR("Antigravity OS Team");
MODULE_DESCRIPTION("Synaptics S3910 Touchscreen SPI Driver");
MODULE_LICENSE("GPL");
