// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2019-2020, The Linux Foundation. All rights reserved.
 * Copyright (c) 2022, Linaro Ltd
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 */
#include <linux/auxiliary_bus.h>
#include <linux/debugfs.h>
#include <linux/devm-helpers.h>
#include <linux/iio/consumer.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/nvmem-consumer.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/pm_wakeup.h>
#include <linux/power_supply.h>
#include <linux/property.h>
#include <linux/soc/qcom/pdr.h>
#include <linux/soc/qcom/pmic_glink.h>
#include <linux/math.h>
#include <linux/units.h>
#include <linux/usb/pd.h>

#define BATTMGR_CHEMISTRY_LEN	4
#define BATTMGR_STRING_LEN	128

enum qcom_battmgr_variant {
	QCOM_BATTMGR_SC8280XP,
	QCOM_BATTMGR_SM8350,
	QCOM_BATTMGR_SM8550,
	QCOM_BATTMGR_X1E80100,
};

#define BATTMGR_BAT_STATUS		0x1

#define BATTMGR_REQUEST_NOTIFICATION	0x4

#define BATTMGR_NOTIFICATION		0x7
#define NOTIF_BAT_PROPERTY		0x30
#define NOTIF_USB_PROPERTY		0x32
#define NOTIF_WLS_PROPERTY		0x34
#define NOTIF_OPLUS_OTG_ENABLE		0x50
#define NOTIF_OPLUS_OTG_DISABLE		0x51
#define NOTIF_CID_DETECT		0x53
#define NOTIF_TYPEC_STATE_CHANGE	0x55
#define NOTIF_PLUGIN_IRQ		0x57
#define NOTIF_APSD_DONE			0x58
#define NOTIF_CP_MOS_DISABLE		0x64
#define NOTIF_PD_SOURCE_CAPS		0x79
#define NOTIF_POWER_ROLE_STATUS		0x7d
#define NOTIF_PD_CONNECT_HARD_RESET	0x7f
#define NOTIF_BAT_STATUS		0x80
#define NOTIF_BAT_INFO			0x81
#define NOTIF_BAT_CHARGING_STATE	0x83

#define BATTMGR_BAT_INFO		0x9

#define BATTMGR_BAT_DISCHARGE_TIME	0xc

#define BATTMGR_BAT_CHARGE_TIME		0xd

#define BATTMGR_BAT_PROPERTY_GET	0x30
#define BATTMGR_BAT_PROPERTY_SET	0x31
#define BATT_STATUS			0
#define BATT_HEALTH			1
#define BATT_PRESENT			2
#define BATT_CHG_TYPE			3
#define BATT_CAPACITY			4
#define BATT_SOH			5
#define BATT_VOLT_OCV			6
#define BATT_VOLT_NOW			7
#define BATT_VOLT_MAX			8
#define BATT_CURR_NOW			9
#define BATT_CHG_CTRL_LIM		10
#define BATT_CHG_CTRL_LIM_MAX		11
#define BATT_TEMP			12
#define BATT_TECHNOLOGY			13
#define BATT_CHG_COUNTER		14
#define BATT_CYCLE_COUNT		15
#define BATT_CHG_FULL_DESIGN		16
#define BATT_CHG_FULL			17
#define BATT_MODEL_NAME			18
#define BATT_TTF_AVG			19
#define BATT_TTE_AVG			20
#define BATT_RESISTANCE			21
#define BATT_POWER_NOW			22
#define BATT_POWER_AVG			23
#define BATT_CHG_CTRL_EN		24
#define OPLUS_BATT_SET_PDO		25
#define BATT_CHG_CTRL_START_THR		25
#define BATT_CHG_CTRL_END_THR		26

#define BATTMGR_USB_PROPERTY_GET	0x32
#define BATTMGR_USB_PROPERTY_SET	0x33
#define USB_ONLINE			0
#define USB_VOLT_NOW			1
#define USB_VOLT_MAX			2
#define USB_CURR_NOW			3
#define USB_CURR_MAX			4
#define USB_INPUT_CURR_LIMIT		5
#define USB_TYPE			6
#define USB_ADAP_TYPE			7
#define USB_MOISTURE_DET_EN		8
#define USB_MOISTURE_DET_STS		9

/* OnePlus SM8650 uses the regular USB property namespace (not SOCCP). */
#define OPLUS_USB_OTG_AP_ENABLE		17
#define OPLUS_USB_OTG_VBUS_REGULATOR_ENABLE	24
#define OPLUS_USB_GET_PPS_STATUS		33
#define OPLUS_USB_SET_PPS_VOLT		34
#define OPLUS_USB_SET_PPS_CURR		35
#define OPLUS_USB_PPS_MOS_CTRL		39
#define OPLUS_USB_OTG_BOOST_CURRENT	64

#define OPLUS_OEM_READ_BUFFER		0x10000
#define OPLUS_PPS_READ_BUFFER		0x10004
#define OPLUS_BUFFER_WORDS		128
/* Ordinary buck-path limits are separate from supervised BYPASS limits. */
#define OPLUS_PPS_MAX_VOLT_MV		9000
#define OPLUS_PPS_MAX_CURR_MA		2000
#define OPLUS_DIRECT_MAX_VOLT_MV		11000
#define OPLUS_DIRECT_MAX_CURR_MA		3000
#define OPLUS_DIRECT_MAX_CELL_MV		4400
#define OPLUS_DIRECT_LEASE		(15 * HZ)

enum qcom_battmgr_oplus_direct_state {
	OPLUS_DIRECT_IDLE,
	OPLUS_DIRECT_PRECHARGE,
	OPLUS_DIRECT_RUNNING,
	OPLUS_DIRECT_STOPPING,
};

enum qcom_battmgr_oplus_thermistor {
	OPLUS_THERM_BATTERY_CONNECTOR,
	OPLUS_THERM_USB_CONNECTOR,
	OPLUS_THERM_SUBBOARD,
	OPLUS_THERM_COUNT,
};

static const char * const qcom_battmgr_oplus_thermistor_names[] = {
	"battery-connector", "usb-connector", "subboard",
};

struct qcom_battmgr_oplus_buffer_request {
	struct pmic_glink_hdr hdr;
	__le32 data_size;
};

struct qcom_battmgr_oplus_buffer_resp {
	struct pmic_glink_hdr hdr;
	__le32 data[OPLUS_BUFFER_WORDS];
	__le32 data_size;
};

#define BATTMGR_WLS_PROPERTY_GET	0x34
#define BATTMGR_WLS_PROPERTY_SET	0x35
#define WLS_ONLINE			0
#define WLS_VOLT_NOW			1
#define WLS_VOLT_MAX			2
#define WLS_CURR_NOW			3
#define WLS_CURR_MAX			4
#define WLS_TYPE			5
#define WLS_BOOST_EN			6

#define BATTMGR_CHG_CTRL_LIMIT_EN	0x48
#define CHARGE_CTRL_START_THR_MIN	50
#define CHARGE_CTRL_START_THR_MAX	95
#define CHARGE_CTRL_END_THR_MIN		55
#define CHARGE_CTRL_END_THR_MAX		100
#define CHARGE_CTRL_DELTA_SOC		5

struct qcom_battmgr_enable_request {
	struct pmic_glink_hdr hdr;
	__le32 battery_id;
	__le32 power_state;
	__le32 low_capacity;
	__le32 high_capacity;
};

struct qcom_battmgr_property_request {
	struct pmic_glink_hdr hdr;
	__le32 battery;
	__le32 property;
	__le32 value;
};

struct qcom_battmgr_update_request {
	struct pmic_glink_hdr hdr;
	__le32 battery_id;
};

struct qcom_battmgr_charge_time_request {
	struct pmic_glink_hdr hdr;
	__le32 battery_id;
	__le32 percent;
	__le32 reserved;
};

struct qcom_battmgr_discharge_time_request {
	struct pmic_glink_hdr hdr;
	__le32 battery_id;
	__le32 rate; /* 0 for current rate */
	__le32 reserved;
};

struct qcom_battmgr_charge_ctrl_request {
	struct pmic_glink_hdr hdr;
	__le32 enable;
	__le32 target_soc;
	__le32 delta_soc;
};

struct qcom_battmgr_message {
	struct pmic_glink_hdr hdr;
	union {
		struct {
			__le32 property;
			__le32 value;
			__le32 result;
		} intval;
		struct {
			__le32 property;
			char model[BATTMGR_STRING_LEN];
		} strval;
		struct {
			/*
			 * 0: mWh
			 * 1: mAh
			 */
			__le32 power_unit;
			__le32 design_capacity;
			__le32 last_full_capacity;
			/*
			 * 0 nonrechargable
			 * 1 rechargable
			 */
			__le32 battery_tech;
			__le32 design_voltage; /* mV */
			__le32 capacity_low;
			__le32 capacity_warning;
			__le32 cycle_count;
			/* thousandth of percent */
			__le32 accuracy;
			__le32 max_sample_time_ms;
			__le32 min_sample_time_ms;
			__le32 max_average_interval_ms;
			__le32 min_average_interval_ms;
			/* granularity between low and warning */
			__le32 capacity_granularity1;
			/* granularity between warning and full */
			__le32 capacity_granularity2;
			/*
			 * 0: no
			 * 1: cold
			 * 2: hot
			 */
			__le32 swappable;
			__le32 capabilities;
			char model_number[BATTMGR_STRING_LEN];
			char serial_number[BATTMGR_STRING_LEN];
			char battery_type[BATTMGR_STRING_LEN];
			char oem_info[BATTMGR_STRING_LEN];
			char battery_chemistry[BATTMGR_CHEMISTRY_LEN];
			char uid[BATTMGR_STRING_LEN];
			__le32 critical_bias;
			u8 day;
			u8 month;
			__le16 year;
			__le32 battery_id;
		} info;
		struct {
			/*
			 * BIT(0) discharging
			 * BIT(1) charging
			 * BIT(2) critical low
			 */
			__le32 battery_state;
			/* mWh or mAh, based on info->power_unit */
			__le32 capacity;
			__le32 rate;
			/* mv */
			__le32 battery_voltage;
			/*
			 * BIT(0) power online
			 * BIT(1) discharging
			 * BIT(2) charging
			 * BIT(3) battery critical
			 */
			__le32 power_state;
			/*
			 * 1: AC
			 * 2: USB
			 * 3: Wireless
			 */
			__le32 charging_source;
			__le32 temperature;
		} status;
		__le32 time;
		__le32 notification;
	};
};

#define BATTMGR_CHARGING_SOURCE_AC	1
#define BATTMGR_CHARGING_SOURCE_USB	2
#define BATTMGR_CHARGING_SOURCE_WIRELESS 3

enum qcom_battmgr_unit {
	QCOM_BATTMGR_UNIT_mWh = 0,
	QCOM_BATTMGR_UNIT_mAh = 1
};

struct qcom_battmgr_info {
	bool valid;

	bool present;
	unsigned int charge_type;
	unsigned int design_capacity;
	unsigned int last_full_capacity;
	unsigned int voltage_max_design;
	unsigned int voltage_max;
	unsigned int capacity_low;
	unsigned int capacity_warning;
	unsigned int cycle_count;
	unsigned int charge_count;
	unsigned int charge_ctrl_start;
	unsigned int charge_ctrl_end;
	char model_number[BATTMGR_STRING_LEN];
	char serial_number[BATTMGR_STRING_LEN];
	char oem_info[BATTMGR_STRING_LEN];
	unsigned char technology;
	unsigned char day;
	unsigned char month;
	unsigned short year;
};

struct qcom_battmgr_status {
	unsigned int status;
	unsigned int health;
	unsigned int capacity;
	unsigned int percent;
	int current_now;
	int power_now;
	unsigned int voltage_now;
	unsigned int voltage_ocv;
	unsigned int temperature;
	unsigned int resistance;
	unsigned int soh_percent;

	unsigned int discharge_time;
	unsigned int charge_time;
};

struct qcom_battmgr_ac {
	bool online;
};

struct qcom_battmgr_usb {
	bool online;
	unsigned int voltage_now;
	unsigned int voltage_max;
	unsigned int current_now;
	unsigned int current_max;
	unsigned int current_limit;
	unsigned int usb_type;
};

struct qcom_battmgr_wireless {
	bool online;
	unsigned int voltage_now;
	unsigned int voltage_max;
	unsigned int current_now;
	unsigned int current_max;
};

struct qcom_battmgr_oplus {
	bool otg_state;
	struct iio_channel *thermistors[OPLUS_THERM_COUNT];
	enum qcom_battmgr_oplus_direct_state direct_state;
	bool direct_abort;
	bool direct_shutdown;
	bool direct_mos_pending;
	bool direct_thermal_derated;
	bool direct_connector_derated;
	unsigned long direct_deadline;
	unsigned long direct_start_deadline;
	unsigned long direct_mos_deadline;
	int direct_error;
	int direct_temperatures[OPLUS_THERM_COUNT];
	u32 direct_target_mv;
	u32 direct_input_mv;
	u32 direct_input_ma;
	u32 direct_low_current_count;
	u32 direct_high_current_count;
	u32 direct_cell_current_ma;
	u32 mos_state;
	u32 oem_data[OPLUS_BUFFER_WORDS];
	u32 oem_words;
	u32 pps_pdos[PDO_MAX_OBJECTS];
	u32 pps_imax;
	u32 pps_vmax;
	u32 pps_status;
	u32 pps_voltage_mv;
	u32 pps_current_ma;
	u32 charge_current;
	u32 charge_current_max;
};

struct qcom_battmgr {
	struct device *dev;
	struct pmic_glink_client *client;

	enum qcom_battmgr_variant variant;

	struct power_supply *ac_psy;
	struct power_supply *bat_psy;
	struct power_supply *usb_psy;
	struct power_supply *wls_psy;

	enum qcom_battmgr_unit unit;

	int error;
	struct completion ack;
	u32 request_opcode;
	u32 request_property;
	bool request_has_property;
	bool request_pending;

	bool service_up;

	struct qcom_battmgr_info info;
	struct qcom_battmgr_status status;
	struct qcom_battmgr_ac ac;
	struct qcom_battmgr_usb usb;
	struct qcom_battmgr_wireless wireless;
	struct qcom_battmgr_oplus oplus;
	u32 usb_type_property;
	bool status_follows_power_online;
	bool oplus_otg_control;

	struct work_struct enable_work;
	struct work_struct oplus_otg_work;
	struct delayed_work oplus_direct_work;
	struct wakeup_source *oplus_direct_ws;

	/*
	 * @lock is used to prevent concurrent power supply requests to the
	 * firmware, as it then stops responding.
	 */
	struct mutex lock;
};

static int qcom_battmgr_request(struct qcom_battmgr *battmgr, void *data, size_t len)
{
	const struct pmic_glink_hdr *hdr = data;
	const struct qcom_battmgr_property_request *property_request = data;
	u32 opcode = le32_to_cpu(hdr->opcode);
	unsigned long left;
	int ret;

	reinit_completion(&battmgr->ack);

	battmgr->error = 0;
	battmgr->request_opcode = opcode;
	battmgr->request_has_property =
		len == sizeof(*property_request) &&
		(opcode == BATTMGR_BAT_PROPERTY_GET || opcode == BATTMGR_BAT_PROPERTY_SET ||
		 opcode == BATTMGR_USB_PROPERTY_GET || opcode == BATTMGR_USB_PROPERTY_SET ||
		 opcode == BATTMGR_WLS_PROPERTY_GET);
	if (battmgr->request_has_property)
		battmgr->request_property = le32_to_cpu(property_request->property);
	/* Publish the expected reply before the receive callback can inspect it. */
	smp_store_release(&battmgr->request_pending, true);

	ret = pmic_glink_send(battmgr->client, data, len);
	if (ret < 0) {
		WRITE_ONCE(battmgr->request_pending, false);
		return ret;
	}

	left = wait_for_completion_timeout(&battmgr->ack, HZ);
	WRITE_ONCE(battmgr->request_pending, false);
	if (!left)
		return -ETIMEDOUT;

	return battmgr->error;
}

static int qcom_battmgr_request_property(struct qcom_battmgr *battmgr, int opcode,
					 int property, u32 value)
{
	struct qcom_battmgr_property_request request = {
		.hdr.owner = cpu_to_le32(PMIC_GLINK_OWNER_BATTMGR),
		.hdr.type = cpu_to_le32(PMIC_GLINK_REQ_RESP),
		.hdr.opcode = cpu_to_le32(opcode),
		.battery = cpu_to_le32(0),
		.property = cpu_to_le32(property),
		.value = cpu_to_le32(value),
	};

	return qcom_battmgr_request(battmgr, &request, sizeof(request));
}

static int qcom_battmgr_update_status(struct qcom_battmgr *battmgr)
{
	struct qcom_battmgr_update_request request = {
		.hdr.owner = cpu_to_le32(PMIC_GLINK_OWNER_BATTMGR),
		.hdr.type = cpu_to_le32(PMIC_GLINK_REQ_RESP),
		.hdr.opcode = cpu_to_le32(BATTMGR_BAT_STATUS),
		.battery_id = cpu_to_le32(0),
	};

	return qcom_battmgr_request(battmgr, &request, sizeof(request));
}

static int qcom_battmgr_update_info(struct qcom_battmgr *battmgr)
{
	struct qcom_battmgr_update_request request = {
		.hdr.owner = cpu_to_le32(PMIC_GLINK_OWNER_BATTMGR),
		.hdr.type = cpu_to_le32(PMIC_GLINK_REQ_RESP),
		.hdr.opcode = cpu_to_le32(BATTMGR_BAT_INFO),
		.battery_id = cpu_to_le32(0),
	};

	return qcom_battmgr_request(battmgr, &request, sizeof(request));
}

static int qcom_battmgr_update_charge_time(struct qcom_battmgr *battmgr)
{
	struct qcom_battmgr_charge_time_request request = {
		.hdr.owner = cpu_to_le32(PMIC_GLINK_OWNER_BATTMGR),
		.hdr.type = cpu_to_le32(PMIC_GLINK_REQ_RESP),
		.hdr.opcode = cpu_to_le32(BATTMGR_BAT_CHARGE_TIME),
		.battery_id = cpu_to_le32(0),
		.percent = cpu_to_le32(100),
	};

	return qcom_battmgr_request(battmgr, &request, sizeof(request));
}

static int qcom_battmgr_update_discharge_time(struct qcom_battmgr *battmgr)
{
	struct qcom_battmgr_discharge_time_request request = {
		.hdr.owner = cpu_to_le32(PMIC_GLINK_OWNER_BATTMGR),
		.hdr.type = cpu_to_le32(PMIC_GLINK_REQ_RESP),
		.hdr.opcode = cpu_to_le32(BATTMGR_BAT_DISCHARGE_TIME),
		.battery_id = cpu_to_le32(0),
		.rate = cpu_to_le32(0),
	};

	return qcom_battmgr_request(battmgr, &request, sizeof(request));
}

static const u8 sm8350_bat_prop_map[] = {
	[POWER_SUPPLY_PROP_STATUS] = BATT_STATUS,
	[POWER_SUPPLY_PROP_HEALTH] = BATT_HEALTH,
	[POWER_SUPPLY_PROP_PRESENT] = BATT_PRESENT,
	[POWER_SUPPLY_PROP_CHARGE_TYPE] = BATT_CHG_TYPE,
	[POWER_SUPPLY_PROP_CAPACITY] = BATT_CAPACITY,
	[POWER_SUPPLY_PROP_VOLTAGE_OCV] = BATT_VOLT_OCV,
	[POWER_SUPPLY_PROP_VOLTAGE_NOW] = BATT_VOLT_NOW,
	[POWER_SUPPLY_PROP_VOLTAGE_MAX] = BATT_VOLT_MAX,
	[POWER_SUPPLY_PROP_CURRENT_NOW] = BATT_CURR_NOW,
	[POWER_SUPPLY_PROP_TEMP] = BATT_TEMP,
	[POWER_SUPPLY_PROP_TECHNOLOGY] = BATT_TECHNOLOGY,
	[POWER_SUPPLY_PROP_CHARGE_COUNTER] =  BATT_CHG_COUNTER,
	[POWER_SUPPLY_PROP_CYCLE_COUNT] = BATT_CYCLE_COUNT,
	[POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN] =  BATT_CHG_FULL_DESIGN,
	[POWER_SUPPLY_PROP_CHARGE_FULL] = BATT_CHG_FULL,
	[POWER_SUPPLY_PROP_MODEL_NAME] = BATT_MODEL_NAME,
	[POWER_SUPPLY_PROP_TIME_TO_FULL_AVG] = BATT_TTF_AVG,
	[POWER_SUPPLY_PROP_TIME_TO_EMPTY_AVG] = BATT_TTE_AVG,
	[POWER_SUPPLY_PROP_INTERNAL_RESISTANCE] = BATT_RESISTANCE,
	[POWER_SUPPLY_PROP_STATE_OF_HEALTH] = BATT_SOH,
	[POWER_SUPPLY_PROP_POWER_NOW] = BATT_POWER_NOW,
	[POWER_SUPPLY_PROP_CHARGE_CONTROL_START_THRESHOLD] = BATT_CHG_CTRL_START_THR,
	[POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD] = BATT_CHG_CTRL_END_THR,
};

static int qcom_battmgr_bat_sm8350_update(struct qcom_battmgr *battmgr,
					  enum power_supply_property psp)
{
	unsigned int prop;
	int ret;

	if (psp >= ARRAY_SIZE(sm8350_bat_prop_map))
		return -EINVAL;

	prop = sm8350_bat_prop_map[psp];

	mutex_lock(&battmgr->lock);
	ret = qcom_battmgr_request_property(battmgr, BATTMGR_BAT_PROPERTY_GET, prop, 0);
	mutex_unlock(&battmgr->lock);

	return ret;
}

/*
 * The Waffle firmware leaves BATT_STATUS set to Charging after external
 * power is removed.  Its USB and wireless ONLINE properties do update.
 */
static int qcom_battmgr_bat_waffle_update_status(struct qcom_battmgr *battmgr)
{
	int ret;

	mutex_lock(&battmgr->lock);

	ret = qcom_battmgr_request_property(battmgr, BATTMGR_BAT_PROPERTY_GET,
					    BATT_STATUS, 0);
	if (ret)
		goto out_unlock;

	ret = qcom_battmgr_request_property(battmgr, BATTMGR_USB_PROPERTY_GET,
					    USB_ONLINE, 0);
	if (ret)
		goto out_unlock;

	ret = qcom_battmgr_request_property(battmgr, BATTMGR_WLS_PROPERTY_GET,
					    WLS_ONLINE, 0);
	if (!ret && battmgr->status.status != POWER_SUPPLY_STATUS_FULL) {
		if (battmgr->oplus_otg_control &&
		    READ_ONCE(battmgr->oplus.otg_state))
			battmgr->status.status = POWER_SUPPLY_STATUS_DISCHARGING;
		else if (battmgr->usb.online || battmgr->wireless.online)
			battmgr->status.status = POWER_SUPPLY_STATUS_CHARGING;
		else
			battmgr->status.status = POWER_SUPPLY_STATUS_DISCHARGING;
	}

out_unlock:
	mutex_unlock(&battmgr->lock);
	return ret;
}

static int qcom_battmgr_bat_sc8280xp_update(struct qcom_battmgr *battmgr,
					    enum power_supply_property psp)
{
	int ret;

	mutex_lock(&battmgr->lock);

	if (!battmgr->info.valid) {
		ret = qcom_battmgr_update_info(battmgr);
		if (ret < 0)
			goto out_unlock;
		battmgr->info.valid = true;
	}

	ret = qcom_battmgr_update_status(battmgr);
	if (ret < 0)
		goto out_unlock;

	if (psp == POWER_SUPPLY_PROP_TIME_TO_FULL_AVG) {
		ret = qcom_battmgr_update_charge_time(battmgr);
		if (ret < 0) {
			ret = -ENODATA;
			goto out_unlock;
		}
	}

	if (psp == POWER_SUPPLY_PROP_TIME_TO_EMPTY_AVG) {
		ret = qcom_battmgr_update_discharge_time(battmgr);
		if (ret < 0) {
			ret = -ENODATA;
			goto out_unlock;
		}
	}

out_unlock:
	mutex_unlock(&battmgr->lock);
	return ret;
}

static int qcom_battmgr_bat_get_property(struct power_supply *psy,
					 enum power_supply_property psp,
					 union power_supply_propval *val)
{
	struct qcom_battmgr *battmgr = power_supply_get_drvdata(psy);
	enum qcom_battmgr_unit unit = battmgr->unit;
	int ret;

	if (!battmgr->service_up)
		return -EAGAIN;

	if (battmgr->status_follows_power_online &&
	    psp == POWER_SUPPLY_PROP_STATUS)
		ret = qcom_battmgr_bat_waffle_update_status(battmgr);
	else if (battmgr->variant == QCOM_BATTMGR_SC8280XP ||
	    battmgr->variant == QCOM_BATTMGR_X1E80100)
		ret = qcom_battmgr_bat_sc8280xp_update(battmgr, psp);
	else
		ret = qcom_battmgr_bat_sm8350_update(battmgr, psp);
	if (ret < 0)
		return ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = battmgr->status.status;
		break;
	case POWER_SUPPLY_PROP_CHARGE_TYPE:
		val->intval = battmgr->info.charge_type;
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		val->intval = battmgr->status.health;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		val->intval = battmgr->info.present;
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = battmgr->info.technology;
		break;
	case POWER_SUPPLY_PROP_CYCLE_COUNT:
		val->intval = battmgr->info.cycle_count;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN:
		val->intval = battmgr->info.voltage_max_design;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		val->intval = battmgr->info.voltage_max;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = battmgr->status.voltage_now;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_OCV:
		val->intval = battmgr->status.voltage_ocv;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		val->intval = battmgr->status.current_now;
		break;
	case POWER_SUPPLY_PROP_POWER_NOW:
		val->intval = battmgr->status.power_now;
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		if (unit != QCOM_BATTMGR_UNIT_mAh)
			return -ENODATA;
		val->intval = battmgr->info.design_capacity;
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL:
		if (unit != QCOM_BATTMGR_UNIT_mAh)
			return -ENODATA;
		val->intval = battmgr->info.last_full_capacity;
		break;
	case POWER_SUPPLY_PROP_CHARGE_EMPTY:
		if (unit != QCOM_BATTMGR_UNIT_mAh)
			return -ENODATA;
		val->intval = battmgr->info.capacity_low;
		break;
	case POWER_SUPPLY_PROP_CHARGE_NOW:
		if (unit != QCOM_BATTMGR_UNIT_mAh)
			return -ENODATA;
		val->intval = battmgr->status.capacity;
		break;
	case POWER_SUPPLY_PROP_CHARGE_COUNTER:
		val->intval = battmgr->info.charge_count;
		break;
	case POWER_SUPPLY_PROP_ENERGY_FULL_DESIGN:
		if (unit != QCOM_BATTMGR_UNIT_mWh)
			return -ENODATA;
		val->intval = battmgr->info.design_capacity;
		break;
	case POWER_SUPPLY_PROP_ENERGY_FULL:
		if (unit != QCOM_BATTMGR_UNIT_mWh)
			return -ENODATA;
		val->intval = battmgr->info.last_full_capacity;
		break;
	case POWER_SUPPLY_PROP_ENERGY_EMPTY:
		if (unit != QCOM_BATTMGR_UNIT_mWh)
			return -ENODATA;
		val->intval = battmgr->info.capacity_low;
		break;
	case POWER_SUPPLY_PROP_ENERGY_NOW:
		if (unit != QCOM_BATTMGR_UNIT_mWh)
			return -ENODATA;
		val->intval = battmgr->status.capacity;
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		if (battmgr->status.percent == (unsigned int)-1)
			return -ENODATA;
		val->intval = battmgr->status.percent;
		break;
	case POWER_SUPPLY_PROP_TEMP:
		val->intval = battmgr->status.temperature;
		break;
	case POWER_SUPPLY_PROP_INTERNAL_RESISTANCE:
		val->intval = battmgr->status.resistance;
		break;
	case POWER_SUPPLY_PROP_STATE_OF_HEALTH:
		val->intval = battmgr->status.soh_percent;
		break;
	case POWER_SUPPLY_PROP_TIME_TO_EMPTY_AVG:
		val->intval = battmgr->status.discharge_time;
		break;
	case POWER_SUPPLY_PROP_TIME_TO_FULL_AVG:
		val->intval = battmgr->status.charge_time;
		break;
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_START_THRESHOLD:
		val->intval = battmgr->info.charge_ctrl_start;
		break;
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD:
		val->intval = battmgr->info.charge_ctrl_end;
		break;
	case POWER_SUPPLY_PROP_MANUFACTURE_YEAR:
		val->intval = battmgr->info.year;
		break;
	case POWER_SUPPLY_PROP_MANUFACTURE_MONTH:
		val->intval = battmgr->info.month;
		break;
	case POWER_SUPPLY_PROP_MANUFACTURE_DAY:
		val->intval = battmgr->info.day;
		break;
	case POWER_SUPPLY_PROP_MODEL_NAME:
		val->strval = battmgr->info.model_number;
		break;
	case POWER_SUPPLY_PROP_MANUFACTURER:
		val->strval = battmgr->info.oem_info;
		break;
	case POWER_SUPPLY_PROP_SERIAL_NUMBER:
		val->strval = battmgr->info.serial_number;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int qcom_battmgr_set_charge_control(struct qcom_battmgr *battmgr,
					   u32 target_soc, u32 delta_soc)
{
	struct qcom_battmgr_charge_ctrl_request request = {
		.hdr.owner = cpu_to_le32(PMIC_GLINK_OWNER_BATTMGR),
		.hdr.type = cpu_to_le32(PMIC_GLINK_REQ_RESP),
		.hdr.opcode = cpu_to_le32(BATTMGR_CHG_CTRL_LIMIT_EN),
		.enable = cpu_to_le32(1),
		.target_soc = cpu_to_le32(target_soc),
		.delta_soc = cpu_to_le32(delta_soc),
	};

	return qcom_battmgr_request(battmgr, &request, sizeof(request));
}

static int qcom_battmgr_set_charge_start_threshold(struct qcom_battmgr *battmgr, int start_soc)
{
	u32 target_soc, delta_soc;
	int ret;

	start_soc = clamp(start_soc, CHARGE_CTRL_START_THR_MIN, CHARGE_CTRL_START_THR_MAX);

	/*
	 * If the new start threshold is larger than the old end threshold,
	 * move the end threshold one step (DELTA_SOC) after the new start
	 * threshold.
	 */
	if (start_soc > battmgr->info.charge_ctrl_end) {
		target_soc = start_soc + CHARGE_CTRL_DELTA_SOC;
		target_soc = min_t(u32, target_soc, CHARGE_CTRL_END_THR_MAX);
		delta_soc = target_soc - start_soc;
		delta_soc = min_t(u32, delta_soc, CHARGE_CTRL_DELTA_SOC);
	} else {
		target_soc =  battmgr->info.charge_ctrl_end;
		delta_soc = battmgr->info.charge_ctrl_end - start_soc;
	}

	mutex_lock(&battmgr->lock);
	ret = qcom_battmgr_set_charge_control(battmgr, target_soc, delta_soc);
	mutex_unlock(&battmgr->lock);
	if (!ret) {
		battmgr->info.charge_ctrl_start = start_soc;
		battmgr->info.charge_ctrl_end = target_soc;
	}

	return 0;
}

static int qcom_battmgr_set_charge_end_threshold(struct qcom_battmgr *battmgr, int end_soc)
{
	u32 delta_soc = CHARGE_CTRL_DELTA_SOC;
	int ret;

	end_soc = clamp(end_soc, CHARGE_CTRL_END_THR_MIN, CHARGE_CTRL_END_THR_MAX);

	if (battmgr->info.charge_ctrl_start && end_soc > battmgr->info.charge_ctrl_start)
		delta_soc = end_soc - battmgr->info.charge_ctrl_start;

	mutex_lock(&battmgr->lock);
	ret = qcom_battmgr_set_charge_control(battmgr, end_soc, delta_soc);
	mutex_unlock(&battmgr->lock);
	if (!ret) {
		battmgr->info.charge_ctrl_start = end_soc - delta_soc;
		battmgr->info.charge_ctrl_end = end_soc;
	}

	return 0;
}

static int qcom_battmgr_charge_control_thresholds_init(struct qcom_battmgr *battmgr)
{
	int ret;
	u8 en, end_soc, start_soc, delta_soc;

	ret = nvmem_cell_read_u8(battmgr->dev->parent, "charge_limit_en", &en);
	if (!ret && en != 0) {
		ret = nvmem_cell_read_u8(battmgr->dev->parent, "charge_limit_end", &end_soc);
		if (ret < 0)
			return ret;

		ret = nvmem_cell_read_u8(battmgr->dev->parent, "charge_limit_delta", &delta_soc);
		if (ret < 0)
			return ret;

		if (delta_soc >= end_soc)
			return -EINVAL;

		start_soc = end_soc - delta_soc;
		end_soc = clamp(end_soc, CHARGE_CTRL_END_THR_MIN, CHARGE_CTRL_END_THR_MAX);
		start_soc = clamp(start_soc, CHARGE_CTRL_START_THR_MIN, CHARGE_CTRL_START_THR_MAX);

		battmgr->info.charge_ctrl_start = start_soc;
		battmgr->info.charge_ctrl_end = end_soc;
	}

	return 0;
}

static int qcom_battmgr_bat_is_writeable(struct power_supply *psy,
					 enum power_supply_property psp)
{
	switch (psp) {
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_START_THRESHOLD:
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD:
		return 1;
	default:
		return 0;
	}

	return 0;
}

static int qcom_battmgr_bat_set_property(struct power_supply *psy,
					 enum power_supply_property psp,
					 const union power_supply_propval *pval)
{
	struct qcom_battmgr *battmgr = power_supply_get_drvdata(psy);

	if (!battmgr->service_up)
		return -EAGAIN;

	switch (psp) {
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_START_THRESHOLD:
		return qcom_battmgr_set_charge_start_threshold(battmgr, pval->intval);
	case POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD:
		return qcom_battmgr_set_charge_end_threshold(battmgr, pval->intval);
	default:
		return -EINVAL;
	}

	return 0;
}

static const enum power_supply_property sc8280xp_bat_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CYCLE_COUNT,
	POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_POWER_NOW,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_EMPTY,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_ENERGY_FULL_DESIGN,
	POWER_SUPPLY_PROP_ENERGY_FULL,
	POWER_SUPPLY_PROP_ENERGY_EMPTY,
	POWER_SUPPLY_PROP_ENERGY_NOW,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_MANUFACTURE_YEAR,
	POWER_SUPPLY_PROP_MANUFACTURE_MONTH,
	POWER_SUPPLY_PROP_MANUFACTURE_DAY,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_MANUFACTURER,
	POWER_SUPPLY_PROP_SERIAL_NUMBER,
};

static const struct power_supply_desc sc8280xp_bat_psy_desc = {
	.name = "qcom-battmgr-bat",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = sc8280xp_bat_props,
	.num_properties = ARRAY_SIZE(sc8280xp_bat_props),
	.get_property = qcom_battmgr_bat_get_property,
};

static const enum power_supply_property x1e80100_bat_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CYCLE_COUNT,
	POWER_SUPPLY_PROP_VOLTAGE_MAX_DESIGN,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_POWER_NOW,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_CHARGE_EMPTY,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_ENERGY_FULL_DESIGN,
	POWER_SUPPLY_PROP_ENERGY_FULL,
	POWER_SUPPLY_PROP_ENERGY_EMPTY,
	POWER_SUPPLY_PROP_ENERGY_NOW,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_MANUFACTURE_YEAR,
	POWER_SUPPLY_PROP_MANUFACTURE_MONTH,
	POWER_SUPPLY_PROP_MANUFACTURE_DAY,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_MANUFACTURER,
	POWER_SUPPLY_PROP_SERIAL_NUMBER,
	POWER_SUPPLY_PROP_CHARGE_CONTROL_START_THRESHOLD,
	POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD,
};

static const struct power_supply_desc x1e80100_bat_psy_desc = {
	.name = "qcom-battmgr-bat",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = x1e80100_bat_props,
	.num_properties = ARRAY_SIZE(x1e80100_bat_props),
	.get_property = qcom_battmgr_bat_get_property,
	.set_property = qcom_battmgr_bat_set_property,
	.property_is_writeable = qcom_battmgr_bat_is_writeable,
};

static const enum power_supply_property sm8350_bat_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_CHARGE_TYPE,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_VOLTAGE_OCV,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CHARGE_COUNTER,
	POWER_SUPPLY_PROP_CYCLE_COUNT,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_TIME_TO_FULL_AVG,
	POWER_SUPPLY_PROP_TIME_TO_EMPTY_AVG,
	POWER_SUPPLY_PROP_INTERNAL_RESISTANCE,
	POWER_SUPPLY_PROP_STATE_OF_HEALTH,
	POWER_SUPPLY_PROP_POWER_NOW,
};

static const struct power_supply_desc sm8350_bat_psy_desc = {
	.name = "qcom-battmgr-bat",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = sm8350_bat_props,
	.num_properties = ARRAY_SIZE(sm8350_bat_props),
	.get_property = qcom_battmgr_bat_get_property,
};

static const enum power_supply_property sm8550_bat_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_CHARGE_TYPE,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_VOLTAGE_OCV,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CHARGE_COUNTER,
	POWER_SUPPLY_PROP_CYCLE_COUNT,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CHARGE_FULL,
	POWER_SUPPLY_PROP_MODEL_NAME,
	POWER_SUPPLY_PROP_TIME_TO_FULL_AVG,
	POWER_SUPPLY_PROP_TIME_TO_EMPTY_AVG,
	POWER_SUPPLY_PROP_INTERNAL_RESISTANCE,
	POWER_SUPPLY_PROP_STATE_OF_HEALTH,
	POWER_SUPPLY_PROP_POWER_NOW,
	POWER_SUPPLY_PROP_CHARGE_CONTROL_START_THRESHOLD,
	POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD,
};

static const struct power_supply_desc sm8550_bat_psy_desc = {
	.name = "qcom-battmgr-bat",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = sm8550_bat_props,
	.num_properties = ARRAY_SIZE(sm8550_bat_props),
	.get_property = qcom_battmgr_bat_get_property,
	.set_property = qcom_battmgr_bat_set_property,
	.property_is_writeable = qcom_battmgr_bat_is_writeable,
};

static int qcom_battmgr_ac_get_property(struct power_supply *psy,
					enum power_supply_property psp,
					union power_supply_propval *val)
{
	struct qcom_battmgr *battmgr = power_supply_get_drvdata(psy);
	int ret;

	if (!battmgr->service_up)
		return -EAGAIN;

	ret = qcom_battmgr_bat_sc8280xp_update(battmgr, psp);
	if (ret)
		return ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = battmgr->ac.online;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static const enum power_supply_property sc8280xp_ac_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
};

static const struct power_supply_desc sc8280xp_ac_psy_desc = {
	.name = "qcom-battmgr-ac",
	.type = POWER_SUPPLY_TYPE_MAINS,
	.properties = sc8280xp_ac_props,
	.num_properties = ARRAY_SIZE(sc8280xp_ac_props),
	.get_property = qcom_battmgr_ac_get_property,
};

static const u8 sm8350_usb_prop_map[] = {
	[POWER_SUPPLY_PROP_ONLINE] = USB_ONLINE,
	[POWER_SUPPLY_PROP_VOLTAGE_NOW] = USB_VOLT_NOW,
	[POWER_SUPPLY_PROP_VOLTAGE_MAX] = USB_VOLT_MAX,
	[POWER_SUPPLY_PROP_CURRENT_NOW] = USB_CURR_NOW,
	[POWER_SUPPLY_PROP_CURRENT_MAX] = USB_CURR_MAX,
	[POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT] = USB_INPUT_CURR_LIMIT,
	[POWER_SUPPLY_PROP_USB_TYPE] = USB_TYPE,
};

static int qcom_battmgr_usb_sm8350_update(struct qcom_battmgr *battmgr,
					  enum power_supply_property psp)
{
	unsigned int prop;
	int ret;

	if (psp >= ARRAY_SIZE(sm8350_usb_prop_map))
		return -EINVAL;

	prop = sm8350_usb_prop_map[psp];
	if (psp == POWER_SUPPLY_PROP_USB_TYPE)
		prop = battmgr->usb_type_property;

	mutex_lock(&battmgr->lock);
	ret = qcom_battmgr_request_property(battmgr, BATTMGR_USB_PROPERTY_GET, prop, 0);
	mutex_unlock(&battmgr->lock);

	return ret;
}

/* Caller holds battmgr->lock, including across both PPS property writes. */
static int qcom_battmgr_oplus_pps_caps(struct qcom_battmgr *battmgr)
{
	struct qcom_battmgr_oplus_buffer_request request = {
		.hdr.owner = cpu_to_le32(PMIC_GLINK_OWNER_BATTMGR),
		.hdr.type = cpu_to_le32(PMIC_GLINK_REQ_RESP),
		.hdr.opcode = cpu_to_le32(OPLUS_PPS_READ_BUFFER),
		.data_size = cpu_to_le32(OPLUS_BUFFER_WORDS * sizeof(__le32)),
	};
	int ret;

	ret = qcom_battmgr_request_property(battmgr, BATTMGR_USB_PROPERTY_GET,
					    USB_ONLINE, 0);
	if (ret)
		return ret;
	/* Buck input suspend reports offline even while a PPS source is attached. */
	if (!battmgr->usb.online && battmgr->oplus.direct_state == OPLUS_DIRECT_IDLE)
		return -ENODATA;

	ret = qcom_battmgr_request_property(battmgr, BATTMGR_USB_PROPERTY_GET,
					    USB_ADAP_TYPE, 0);
	if (ret)
		return ret;
	if (battmgr->usb.usb_type != POWER_SUPPLY_USB_TYPE_PD &&
	    battmgr->usb.usb_type != POWER_SUPPLY_USB_TYPE_PD_DRP &&
	    battmgr->usb.usb_type != POWER_SUPPLY_USB_TYPE_PD_PPS)
		return -EOPNOTSUPP;

	memset(battmgr->oplus.pps_pdos, 0, sizeof(battmgr->oplus.pps_pdos));
	return qcom_battmgr_request(battmgr, &request, sizeof(request));
}

static bool qcom_battmgr_oplus_pps_supported(struct qcom_battmgr *battmgr,
					     u32 voltage_mv, u32 current_ma)
{
	u32 pdo;
	int i;

	for (i = 0; i < ARRAY_SIZE(battmgr->oplus.pps_pdos); i++) {
		pdo = battmgr->oplus.pps_pdos[i];
		if (pdo_type(pdo) != PDO_TYPE_APDO || pdo_apdo_type(pdo) != APDO_TYPE_PPS)
			continue;
		if (voltage_mv >= pdo_pps_apdo_min_voltage(pdo) &&
		    voltage_mv <= pdo_pps_apdo_max_voltage(pdo) &&
		    current_ma <= pdo_pps_apdo_max_current(pdo))
			return true;
	}

	return false;
}

static int qcom_battmgr_usb_set_property(struct power_supply *psy,
					 enum power_supply_property psp,
					 const union power_supply_propval *val)
{
	struct qcom_battmgr *battmgr = power_supply_get_drvdata(psy);
	u32 voltage_mv, current_ma;
	int ret;

	if (!battmgr->oplus_otg_control)
		return -EOPNOTSUPP;
	if (!READ_ONCE(battmgr->service_up))
		return -EAGAIN;
	if (val->intval <= 0 || val->intval % 1000)
		return -EINVAL;
	if (psp != POWER_SUPPLY_PROP_VOLTAGE_NOW && psp != POWER_SUPPLY_PROP_CURRENT_NOW)
		return -EINVAL;

	mutex_lock(&battmgr->lock);
	if (battmgr->oplus.direct_state != OPLUS_DIRECT_IDLE) {
		ret = -EBUSY;
		goto out_unlock;
	}
	ret = qcom_battmgr_oplus_pps_caps(battmgr);
	if (ret)
		goto out_unlock;
	if (battmgr->usb.usb_type != POWER_SUPPLY_USB_TYPE_PD_PPS) {
		ret = -EOPNOTSUPP;
		goto out_unlock;
	}

	voltage_mv = battmgr->oplus.pps_voltage_mv;
	current_ma = battmgr->oplus.pps_current_ma;
	if (psp == POWER_SUPPLY_PROP_VOLTAGE_NOW)
		voltage_mv = val->intval / 1000;
	else
		current_ma = val->intval / 1000;

	if (voltage_mv < 5000 || voltage_mv > OPLUS_PPS_MAX_VOLT_MV ||
	    current_ma > OPLUS_PPS_MAX_CURR_MA || voltage_mv % 20 || current_ma % 50 ||
	    !qcom_battmgr_oplus_pps_supported(battmgr, voltage_mv, current_ma)) {
		ret = -EINVAL;
		goto out_unlock;
	}

	ret = qcom_battmgr_request_property(battmgr, BATTMGR_USB_PROPERTY_GET,
					    USB_INPUT_CURR_LIMIT, 0);
	if (ret)
		goto out_unlock;
	if (current_ma * 1000 > battmgr->usb.current_limit) {
		ret = -EINVAL;
		goto out_unlock;
	}

	ret = qcom_battmgr_request_property(battmgr, BATTMGR_USB_PROPERTY_SET,
					    OPLUS_USB_SET_PPS_VOLT, voltage_mv);
	if (ret)
		goto out_unlock;
	ret = qcom_battmgr_request_property(battmgr, BATTMGR_USB_PROPERTY_SET,
					    OPLUS_USB_SET_PPS_CURR, current_ma);
	if (ret) {
		/* Restore the last accepted request if the pair was interrupted. */
		qcom_battmgr_request_property(battmgr, BATTMGR_USB_PROPERTY_SET,
					      OPLUS_USB_SET_PPS_VOLT,
					      battmgr->oplus.pps_voltage_mv);
		qcom_battmgr_request_property(battmgr, BATTMGR_USB_PROPERTY_SET,
					      OPLUS_USB_SET_PPS_CURR,
					      battmgr->oplus.pps_current_ma);
		goto out_unlock;
	}

	battmgr->oplus.pps_voltage_mv = voltage_mv;
	battmgr->oplus.pps_current_ma = current_ma;

out_unlock:
	mutex_unlock(&battmgr->lock);
	if (!ret)
		power_supply_changed(psy);
	return ret > 0 ? -EREMOTEIO : ret;
}

static int qcom_battmgr_usb_property_is_writeable(struct power_supply *psy,
						  enum power_supply_property psp)
{
	struct qcom_battmgr *battmgr = power_supply_get_drvdata(psy);

	return battmgr->oplus_otg_control &&
	       (psp == POWER_SUPPLY_PROP_VOLTAGE_NOW || psp == POWER_SUPPLY_PROP_CURRENT_NOW);
}

static int qcom_battmgr_oplus_pps_show(struct seq_file *s, void *unused)
{
	struct qcom_battmgr *battmgr = s->private;
	u32 pdo, status;
	int ret, i;

	if (!READ_ONCE(battmgr->service_up))
		return -EAGAIN;

	mutex_lock(&battmgr->lock);
	ret = qcom_battmgr_oplus_pps_caps(battmgr);
	if (ret)
		goto out_unlock;

	seq_printf(s, "firmware_imax=%u firmware_vmax=%u\n",
		   battmgr->oplus.pps_imax, battmgr->oplus.pps_vmax);
	for (i = 0; i < ARRAY_SIZE(battmgr->oplus.pps_pdos); i++) {
		pdo = battmgr->oplus.pps_pdos[i];
		seq_printf(s, "pdo[%d]=%#010x", i, pdo);
		if (pdo_type(pdo) == PDO_TYPE_APDO && pdo_apdo_type(pdo) == APDO_TYPE_PPS)
			seq_printf(s, " PPS %u-%u mV %u mA",
				   pdo_pps_apdo_min_voltage(pdo),
				   pdo_pps_apdo_max_voltage(pdo), pdo_pps_apdo_max_current(pdo));
		seq_putc(s, '\n');
	}

	ret = qcom_battmgr_request_property(battmgr, BATTMGR_USB_PROPERTY_GET,
					    OPLUS_USB_GET_PPS_STATUS, 0);
	if (ret) {
		seq_printf(s, "status_error=%d\n", ret);
		ret = 0;
	} else {
		status = battmgr->oplus.pps_status;
		if (!status)
			seq_puts(s, "status=unavailable (firmware returned zero)\n");
		else
			seq_printf(s, "status=%#010x voltage=%u mV current=%u mA\n",
				   status, (status & 0xffff) * 20, ((status >> 16) & 0xff) * 50);
	}
	seq_printf(s, "last_request=%u mV %u mA\n", battmgr->oplus.pps_voltage_mv,
		   battmgr->oplus.pps_current_ma);

	ret = qcom_battmgr_request_property(battmgr, BATTMGR_BAT_PROPERTY_GET,
					    BATT_CHG_CTRL_LIM, 0);
	if (ret) {
		seq_printf(s, "charge_current_error=%d\n", ret);
		ret = 0;
	} else {
		seq_printf(s, "charge_current=%u uA\n", battmgr->oplus.charge_current);
	}
	ret = qcom_battmgr_request_property(battmgr, BATTMGR_BAT_PROPERTY_GET,
					    BATT_CHG_CTRL_LIM_MAX, 0);
	if (ret) {
		seq_printf(s, "charge_current_max_error=%d\n", ret);
		ret = 0;
	} else {
		seq_printf(s, "charge_current_max=%u uA\n", battmgr->oplus.charge_current_max);
	}

out_unlock:
	mutex_unlock(&battmgr->lock);
	return ret > 0 ? -EREMOTEIO : ret;
}
DEFINE_SHOW_ATTRIBUTE(qcom_battmgr_oplus_pps);

static int qcom_battmgr_oplus_oem_read(struct qcom_battmgr *battmgr)
{
	struct qcom_battmgr_oplus_buffer_request request = {
		.hdr.owner = cpu_to_le32(PMIC_GLINK_OWNER_BATTMGR),
		.hdr.type = cpu_to_le32(PMIC_GLINK_REQ_RESP),
		.hdr.opcode = cpu_to_le32(OPLUS_OEM_READ_BUFFER),
		.data_size = cpu_to_le32(OPLUS_BUFFER_WORDS * sizeof(__le32)),
	};

	battmgr->oplus.oem_words = 0;
	memset(battmgr->oplus.oem_data, 0, sizeof(battmgr->oplus.oem_data));
	return qcom_battmgr_request(battmgr, &request, sizeof(request));
}

static int qcom_battmgr_oplus_charger_show(struct seq_file *s, void *unused)
{
	struct qcom_battmgr *battmgr = s->private;
	u32 *data = battmgr->oplus.oem_data;
	int ret, i;

	if (!READ_ONCE(battmgr->service_up))
		return -EAGAIN;

	mutex_lock(&battmgr->lock);
	ret = qcom_battmgr_oplus_oem_read(battmgr);
	if (ret)
		goto out_unlock;

	seq_printf(s, "temperature=%d deciC current=%d uA voltage=%u uV soc=%u centipercent\n",
		   (s32)data[0], (s32)data[1], data[2], data[3]);
	seq_printf(s, "remaining=%u full=%u cycle_count=%u soh=%u suspend=%u\n",
		   data[4], data[6], data[7], data[8], data[9]);
	for (i = 0; i < battmgr->oplus.oem_words; i++)
		seq_printf(s, "oem[%d]=%#010x\n", i, data[i]);

out_unlock:
	mutex_unlock(&battmgr->lock);
	return ret > 0 ? -EREMOTEIO : ret;
}
DEFINE_SHOW_ATTRIBUTE(qcom_battmgr_oplus_charger);

static int qcom_battmgr_oplus_read_temperatures(struct qcom_battmgr *battmgr,
						int *temperatures)
{
	int ret, i;

	if (!IS_REACHABLE(CONFIG_IIO))
		return -EOPNOTSUPP;
	for (i = 0; i < OPLUS_THERM_COUNT; i++) {
		if (!battmgr->oplus.thermistors[i])
			return -ENODEV;
		ret = iio_read_channel_processed(battmgr->oplus.thermistors[i],
						 &temperatures[i]);
		if (ret < 0)
			return ret;
	}
	return 0;
}

static int qcom_battmgr_oplus_thermal_show(struct seq_file *s, void *unused)
{
	struct qcom_battmgr *battmgr = s->private;
	int temperatures[OPLUS_THERM_COUNT];
	int ret, i;

	ret = qcom_battmgr_oplus_read_temperatures(battmgr, temperatures);
	if (ret)
		return ret;
	for (i = 0; i < OPLUS_THERM_COUNT; i++)
		seq_printf(s, "%s=%d mC\n", qcom_battmgr_oplus_thermistor_names[i],
			   temperatures[i]);

	return 0;
}
DEFINE_SHOW_ATTRIBUTE(qcom_battmgr_oplus_thermal);

static int qcom_battmgr_oplus_mos_show(struct seq_file *s, void *unused)
{
	struct qcom_battmgr *battmgr = s->private;
	int ret;

	if (!READ_ONCE(battmgr->service_up))
		return -EAGAIN;
	mutex_lock(&battmgr->lock);
	battmgr->oplus.mos_state = U32_MAX;
	ret = qcom_battmgr_request_property(battmgr, BATTMGR_USB_PROPERTY_GET,
					    OPLUS_USB_PPS_MOS_CTRL, 0);
	if (!ret)
		seq_printf(s, "%u\n", battmgr->oplus.mos_state);
	mutex_unlock(&battmgr->lock);
	return ret > 0 ? -EREMOTEIO : ret;
}
DEFINE_SHOW_ATTRIBUTE(qcom_battmgr_oplus_mos);

/* Explicit bring-up controls, limited to the OEM ordinary-PD operating point. */
static int qcom_battmgr_oplus_charger_test_set(struct qcom_battmgr *battmgr,
					       u32 opcode, u32 property, u32 value)
{
	u32 *data = battmgr->oplus.oem_data;
	bool supported = false;
	u32 pdo;
	int ret, i;

	if (!READ_ONCE(battmgr->service_up))
		return -EAGAIN;

	mutex_lock(&battmgr->lock);
	/* Keep the conservative-current rollback available even with invalid telemetry. */
	if (battmgr->oplus.direct_state != OPLUS_DIRECT_IDLE) {
		ret = -EBUSY;
		goto out_unlock;
	}
	if ((property == BATT_CHG_CTRL_LIM || property == USB_INPUT_CURR_LIMIT) &&
	    value <= 500000)
		goto write_property;
	ret = qcom_battmgr_oplus_pps_caps(battmgr);
	if (ret)
		goto out_unlock;
	if (property != OPLUS_BATT_SET_PDO || value != 5000) {
		ret = qcom_battmgr_oplus_oem_read(battmgr);
		if (ret)
			goto out_unlock;
		/* Do not raise limits using the generic, potentially stale gauge data. */
		if ((s32)data[0] < 100 || (s32)data[0] > 400 ||
		    data[2] < 3000000 || data[2] > 4200000) {
			ret = -EPERM;
			goto out_unlock;
		}
	}

	if (property == OPLUS_BATT_SET_PDO) {
		for (i = 0; i < ARRAY_SIZE(battmgr->oplus.pps_pdos); i++) {
			pdo = battmgr->oplus.pps_pdos[i];
			if (pdo_type(pdo) == PDO_TYPE_FIXED &&
			    pdo_fixed_voltage(pdo) == value && pdo_max_current(pdo) >= 500)
				supported = true;
		}
		if (!supported) {
			ret = -EINVAL;
			goto out_unlock;
		}
	}
	if (opcode == BATTMGR_USB_PROPERTY_SET && property == USB_INPUT_CURR_LIMIT) {
		ret = qcom_battmgr_request_property(battmgr, BATTMGR_USB_PROPERTY_GET,
						    USB_VOLT_MAX, 0);
		if (ret)
			goto out_unlock;
		for (i = 0; i < ARRAY_SIZE(battmgr->oplus.pps_pdos); i++) {
			pdo = battmgr->oplus.pps_pdos[i];
			if (pdo_type(pdo) == PDO_TYPE_FIXED &&
			    pdo_fixed_voltage(pdo) * 1000 == battmgr->usb.voltage_max &&
			    pdo_max_current(pdo) * 1000 >= value)
				supported = true;
		}
		if (battmgr->usb.usb_type == POWER_SUPPLY_USB_TYPE_PD_PPS &&
		    qcom_battmgr_oplus_pps_supported(battmgr,
						     battmgr->usb.voltage_max / 1000, value / 1000))
			supported = true;
		if (!supported) {
			ret = -EINVAL;
			goto out_unlock;
		}
	}
	if (property == BATT_CHG_CTRL_LIM) {
		ret = qcom_battmgr_request_property(battmgr, BATTMGR_BAT_PROPERTY_GET,
						    BATT_CHG_CTRL_LIM_MAX, 0);
		if (ret)
			goto out_unlock;
		if (value > battmgr->oplus.charge_current_max) {
			ret = -EINVAL;
			goto out_unlock;
		}
	}
write_property:
	ret = qcom_battmgr_request_property(battmgr, opcode, property, value);

out_unlock:
	mutex_unlock(&battmgr->lock);
	if (!ret) {
		power_supply_changed(battmgr->bat_psy);
		power_supply_changed(battmgr->usb_psy);
	}
	return ret > 0 ? -EREMOTEIO : ret;
}

static int qcom_battmgr_oplus_fcc_set(void *data, u64 value)
{
	if (value < 100000 || value > 2000000 || value % 50000)
		return -EINVAL;
	return qcom_battmgr_oplus_charger_test_set(data, BATTMGR_BAT_PROPERTY_SET,
						 BATT_CHG_CTRL_LIM, value);
}
DEFINE_DEBUGFS_ATTRIBUTE(qcom_battmgr_oplus_fcc_fops, NULL, qcom_battmgr_oplus_fcc_set,
			 "%llu\n");

static int qcom_battmgr_oplus_icl_set(void *data, u64 value)
{
	if (value < 100000 || value > 2000000 || value % 50000)
		return -EINVAL;
	return qcom_battmgr_oplus_charger_test_set(data, BATTMGR_USB_PROPERTY_SET,
						 USB_INPUT_CURR_LIMIT, value);
}
DEFINE_DEBUGFS_ATTRIBUTE(qcom_battmgr_oplus_icl_fops, NULL, qcom_battmgr_oplus_icl_set,
			 "%llu\n");

static int qcom_battmgr_oplus_fixed_voltage_set(void *data, u64 value)
{
	if (value != 5000 && value != 9000)
		return -EINVAL;
	return qcom_battmgr_oplus_charger_test_set(data, BATTMGR_BAT_PROPERTY_SET,
						 OPLUS_BATT_SET_PDO, value);
}
DEFINE_DEBUGFS_ATTRIBUTE(qcom_battmgr_oplus_fixed_voltage_fops, NULL,
			 qcom_battmgr_oplus_fixed_voltage_set, "%llu\n");

/* Supervised BYPASS bring-up: never arm automatically or change float voltage. */
static int qcom_battmgr_oplus_direct_mos(struct qcom_battmgr *battmgr, bool enable)
{
	int ret;

	ret = qcom_battmgr_request_property(battmgr, BATTMGR_USB_PROPERTY_SET,
					    OPLUS_USB_PPS_MOS_CTRL, enable);
	if (ret)
		return ret > 0 ? -EREMOTEIO : ret;
	battmgr->oplus.mos_state = U32_MAX;
	ret = qcom_battmgr_request_property(battmgr, BATTMGR_USB_PROPERTY_GET,
					    OPLUS_USB_PPS_MOS_CTRL, 0);
	if (ret)
		return ret > 0 ? -EREMOTEIO : ret;
	/* Firmware returns 2 when enabled; downstream treats this as a bool. */
	return !!battmgr->oplus.mos_state == enable ? 0 : -EIO;
}

static int qcom_battmgr_oplus_direct_pps(struct qcom_battmgr *battmgr,
					 u32 voltage_mv, u32 current_ma)
{
	int ret;

	if (voltage_mv < 5000 || voltage_mv > OPLUS_DIRECT_MAX_VOLT_MV || voltage_mv % 20 ||
	    current_ma < 500 || current_ma > OPLUS_DIRECT_MAX_CURR_MA ||
	    current_ma % 50 ||
	    !qcom_battmgr_oplus_pps_supported(battmgr, voltage_mv, current_ma))
		return -EINVAL;

	ret = qcom_battmgr_request_property(battmgr, BATTMGR_USB_PROPERTY_SET,
					    OPLUS_USB_SET_PPS_VOLT, voltage_mv);
	if (ret)
		return ret > 0 ? -EREMOTEIO : ret;
	ret = qcom_battmgr_request_property(battmgr, BATTMGR_USB_PROPERTY_SET,
					    OPLUS_USB_SET_PPS_CURR, current_ma);
	if (ret)
		return ret > 0 ? -EREMOTEIO : ret;
	battmgr->oplus.pps_voltage_mv = voltage_mv;
	battmgr->oplus.pps_current_ma = current_ma;
	return 0;
}

/* All telemetry is refreshed under the same lock as the firmware requests. */
static int qcom_battmgr_oplus_direct_sample(struct qcom_battmgr *battmgr,
					    u32 *target_current)
{
	struct qcom_battmgr_oplus *oplus = &battmgr->oplus;
	u32 *data = oplus->oem_data;
	int temp, battery_current, min_cell_mv, ret, i, hottest = INT_MIN;
	u32 cell_mv;

	if (!READ_ONCE(battmgr->service_up) || READ_ONCE(oplus->otg_state))
		return -ENOTCONN;
	ret = qcom_battmgr_oplus_pps_caps(battmgr);
	if (ret)
		return ret > 0 ? -EREMOTEIO : ret;
	if (battmgr->usb.usb_type != POWER_SUPPLY_USB_TYPE_PD_PPS)
		return -EOPNOTSUPP;
	ret = qcom_battmgr_oplus_oem_read(battmgr);
	if (ret)
		return ret > 0 ? -EREMOTEIO : ret;
	ret = qcom_battmgr_oplus_read_temperatures(battmgr, oplus->direct_temperatures);
	if (ret)
		return ret;

	temp = (s32)data[0];
	battery_current = (s32)data[1];
	cell_mv = data[2] / 1000;
	min_cell_mv = data[13];
	if (temp < 120 || temp >= 400 || cell_mv < 3000 ||
	    cell_mv >= OPLUS_DIRECT_MAX_CELL_MV || min_cell_mv < 3000 ||
	    min_cell_mv >= OPLUS_DIRECT_MAX_CELL_MV ||
	    abs((int)cell_mv - min_cell_mv) > 100 || data[3] >= 7500 ||
	    battery_current < -4000000 || battery_current > 1000000)
		return -ERANGE;
	for (i = 0; i < OPLUS_THERM_COUNT; i++) {
		/* Missing or implausible sensors must not become a synthetic 25 C. */
		if (oplus->direct_temperatures[i] < -10000 ||
		    oplus->direct_temperatures[i] >= 60000)
			return -ERANGE;
		hottest = max(hottest, oplus->direct_temperatures[i]);
	}

	/* Stay below Waffle's downstream 4.42 V software-full threshold. */
	if (cell_mv >= 4350)
		oplus->direct_cell_current_ma = min(oplus->direct_cell_current_ma, 1000U);
	else if (cell_mv >= 4250)
		oplus->direct_cell_current_ma = min(oplus->direct_cell_current_ma, 2000U);
	/* Do not undo voltage taper merely because reducing current lowers IR drop. */
	*target_current = oplus->direct_cell_current_ma;
	if (temp >= 380)
		oplus->direct_thermal_derated = true;
	else if (temp < 360)
		oplus->direct_thermal_derated = false;
	if (hottest >= 50000)
		oplus->direct_connector_derated = true;
	else if (hottest < 45000)
		oplus->direct_connector_derated = false;
	if (temp < 160 || temp >= 360)
		*target_current = min(*target_current, 2000U);
	if (oplus->direct_thermal_derated || oplus->direct_connector_derated)
		*target_current = min(*target_current, 1000U);

	if (oplus->direct_state == OPLUS_DIRECT_RUNNING) {
		/* Downstream's ADSP fallback estimates a 0.35 ohm charging path. */
		oplus->direct_target_mv = min(OPLUS_DIRECT_MAX_VOLT_MV,
					      2 * cell_mv + *target_current * 35 / 100);
	} else {
		/* The 300 mV margin is only for approaching voltage with MOS open. */
		oplus->direct_target_mv = (2 * cell_mv / 100) * 100 + 300;
	}
	if (!qcom_battmgr_oplus_pps_supported(battmgr, 5000, 1000) ||
	    !qcom_battmgr_oplus_pps_supported(battmgr, oplus->direct_target_mv,
					    *target_current))
		return -EOPNOTSUPP;

	ret = qcom_battmgr_request_property(battmgr, BATTMGR_USB_PROPERTY_GET,
					    USB_VOLT_NOW, 0);
	if (ret)
		return ret > 0 ? -EREMOTEIO : ret;
	oplus->direct_input_mv = battmgr->usb.voltage_now / 1000;
	ret = qcom_battmgr_request_property(battmgr, BATTMGR_USB_PROPERTY_GET,
					    USB_CURR_NOW, 0);
	if (ret)
		return ret > 0 ? -EREMOTEIO : ret;
	oplus->direct_input_ma = battmgr->usb.current_now / 1000;
	if (oplus->direct_input_mv < 3500 ||
	    oplus->direct_input_mv > OPLUS_DIRECT_MAX_VOLT_MV ||
	    oplus->direct_input_ma > 3300)
		return -ERANGE;

	oplus->mos_state = U32_MAX;
	ret = qcom_battmgr_request_property(battmgr, BATTMGR_USB_PROPERTY_GET,
					    OPLUS_USB_PPS_MOS_CTRL, 0);
	if (ret)
		return ret > 0 ? -EREMOTEIO : ret;
	return 0;
}

static int qcom_battmgr_oplus_direct_exit(struct qcom_battmgr *battmgr)
{
	struct qcom_battmgr_oplus *oplus = &battmgr->oplus;
	int ret;

	if (!READ_ONCE(battmgr->service_up))
		return -ENOTCONN;
	/* Never resume the buck path until MOS-off has been read back. */
	ret = qcom_battmgr_oplus_direct_mos(battmgr, false);
	if (ret) {
		/* Reduce the PPS limit even if MOS-off cannot yet be confirmed. */
		qcom_battmgr_request_property(battmgr, BATTMGR_USB_PROPERTY_SET,
					      OPLUS_USB_SET_PPS_CURR, 500);
		return ret;
	}
	ret = qcom_battmgr_request_property(battmgr, BATTMGR_BAT_PROPERTY_SET,
					    BATT_CHG_CTRL_LIM, 500000);
	if (ret)
		return ret > 0 ? -EREMOTEIO : ret;
	/* Firmware uses a sentinel to clear input suspend before setting an ICL. */
	ret = qcom_battmgr_request_property(battmgr, BATTMGR_USB_PROPERTY_SET,
					    USB_INPUT_CURR_LIMIT, U32_MAX);
	if (ret)
		return ret > 0 ? -EREMOTEIO : ret;
	ret = qcom_battmgr_request_property(battmgr, BATTMGR_USB_PROPERTY_SET,
					    USB_INPUT_CURR_LIMIT, 500000);
	if (ret)
		return ret > 0 ? -EREMOTEIO : ret;
	ret = qcom_battmgr_oplus_pps_caps(battmgr);
	if (!ret && battmgr->usb.usb_type == POWER_SUPPLY_USB_TYPE_PD_PPS) {
		ret = qcom_battmgr_oplus_direct_pps(battmgr, 5000, 500);
		if (ret)
			return ret;
	} else if (!ret) {
		ret = qcom_battmgr_request_property(battmgr, BATTMGR_BAT_PROPERTY_SET,
						    OPLUS_BATT_SET_PDO, 5000);
		if (ret)
			return ret > 0 ? -EREMOTEIO : ret;
	} else if (ret != -ENODATA && ret != -EOPNOTSUPP) {
		return ret;
	}
	ret = qcom_battmgr_oplus_oem_read(battmgr);
	if (ret)
		return ret > 0 ? -EREMOTEIO : ret;
	ret = qcom_battmgr_request_property(battmgr, BATTMGR_USB_PROPERTY_GET,
					    USB_VOLT_NOW, 0);
	if (ret)
		return ret > 0 ? -EREMOTEIO : ret;
	if (battmgr->usb.voltage_now >= 3500000 && oplus->oem_data[9])
		return -EAGAIN;
	oplus->direct_state = OPLUS_DIRECT_IDLE;
	__pm_relax(battmgr->oplus_direct_ws);
	if (READ_ONCE(oplus->otg_state))
		schedule_work(&battmgr->oplus_otg_work);
	return 0;
}

static void qcom_battmgr_oplus_direct_worker(struct work_struct *work)
{
	struct qcom_battmgr *battmgr = container_of(to_delayed_work(work),
					struct qcom_battmgr, oplus_direct_work);
	struct qcom_battmgr_oplus *oplus = &battmgr->oplus;
	u32 target_current, voltage, request_current;
	u32 pack_mv;
	int ret, charge_ma;

	mutex_lock(&battmgr->lock);
	if (oplus->direct_state == OPLUS_DIRECT_IDLE)
		goto out_unlock;
	if (oplus->direct_state == OPLUS_DIRECT_STOPPING)
		goto stop;
	if (READ_ONCE(oplus->direct_abort) ||
	    time_after_eq(jiffies, oplus->direct_deadline)) {
		oplus->direct_error = -ECANCELED;
		oplus->direct_state = OPLUS_DIRECT_STOPPING;
	}
	if (oplus->direct_state == OPLUS_DIRECT_STOPPING)
		goto stop;

	ret = qcom_battmgr_oplus_direct_sample(battmgr, &target_current);
	if (ret)
		goto fault;
	if (READ_ONCE(oplus->direct_abort) ||
	    time_after_eq(jiffies, oplus->direct_deadline)) {
		ret = -ECANCELED;
		goto fault;
	}
	voltage = oplus->direct_target_mv;
	if (oplus->direct_state == OPLUS_DIRECT_PRECHARGE) {
		/* Downstream checks MOS status after 500 ms, not in the set reply. */
		if (oplus->direct_mos_pending) {
			if (oplus->mos_state) {
				oplus->direct_mos_pending = false;
				oplus->direct_state = OPLUS_DIRECT_RUNNING;
				dev_info(battmgr->dev,
					 "PPS BYPASS enabled at %u mV / 1000 mA (MOS status %u)\n",
					 oplus->direct_input_mv, oplus->mos_state);
				goto reschedule;
			}
			if (time_after_eq(jiffies, oplus->direct_mos_deadline)) {
				ret = -ETIMEDOUT;
				goto fault;
			}
			goto reschedule;
		}
		if (oplus->mos_state != 0 ||
		    time_after_eq(jiffies, oplus->direct_start_deadline)) {
			ret = -ETIMEDOUT;
			goto fault;
		}
		/* Approach pack voltage with MOS open before enabling BYPASS. */
		if (oplus->direct_input_mv < voltage) {
			voltage = min(voltage + 200, oplus->pps_voltage_mv + 200);
			if (voltage == oplus->pps_voltage_mv) {
				ret = -ERANGE;
				goto fault;
			}
		} else if (oplus->direct_input_mv > voltage + 200) {
			voltage = max(5000U, oplus->pps_voltage_mv - 100);
		} else {
			if (oplus->oem_data[9] != 1 || (s32)oplus->oem_data[1] < -100000) {
				ret = -EIO;
				goto fault;
			}
			ret = qcom_battmgr_request_property(battmgr, BATTMGR_USB_PROPERTY_SET,
							    OPLUS_USB_PPS_MOS_CTRL, 1);
			if (ret) {
				ret = ret > 0 ? -EREMOTEIO : ret;
				goto fault;
			}
			oplus->direct_mos_pending = true;
			oplus->direct_mos_deadline = jiffies + 3 * HZ;
			dev_info(battmgr->dev, "PPS BYPASS MOS-on requested at %u mV\n",
				 oplus->direct_input_mv);
			goto reschedule;
		}
		ret = qcom_battmgr_oplus_direct_pps(battmgr, voltage, 1000);
		if (ret)
			goto fault;
		goto reschedule;
	}

	pack_mv = oplus->oem_data[2] / 1000 + oplus->oem_data[13];
	/* Loaded VIN follows pack voltage; it no longer needs the startup margin. */
	if (!oplus->mos_state || oplus->direct_input_mv + 300 < pack_mv ||
	    oplus->direct_input_mv > pack_mv + 500) {
		ret = -EIO;
		goto fault;
	}
	/* ADSP has no CP IIN getter: downstream uses fresh gauge current instead. */
	charge_ma = -(s32)oplus->oem_data[1] / 1000;
	if (charge_ma < 100) {
		if (++oplus->direct_low_current_count >= 4) {
			ret = -EIO;
			goto fault;
		}
	} else {
		oplus->direct_low_current_count = 0;
	}
	request_current = min(target_current, oplus->pps_current_ma + 250);
	if (charge_ma > (int)target_current + 500) {
		if (++oplus->direct_high_current_count >= 4) {
			ret = -ERANGE;
			goto fault;
		}
		voltage = max(5000U, oplus->pps_voltage_mv - 100);
	} else {
		oplus->direct_high_current_count = 0;
		if (charge_ma > (int)target_current + 200)
			voltage = max(5000U, oplus->pps_voltage_mv - 40);
		else if (request_current != target_current ||
			 charge_ma >= (int)target_current - 200)
			voltage = oplus->pps_voltage_mv;
		else if (oplus->pps_voltage_mv + 80 < voltage)
			voltage = min(OPLUS_DIRECT_MAX_VOLT_MV, oplus->pps_voltage_mv + 40);
		else if (oplus->pps_voltage_mv > voltage + 80)
			voltage = max(5000U, oplus->pps_voltage_mv - 40);
		else
			voltage = oplus->pps_voltage_mv;
	}
	/* Mirror the ADSP downstream watchdog-reset operation while MOS is on. */
	ret = qcom_battmgr_oplus_direct_mos(battmgr, true);
	if (ret)
		goto fault;
	ret = qcom_battmgr_oplus_direct_pps(battmgr, voltage, request_current);
	if (ret)
		goto fault;
	goto reschedule;

fault:
	oplus->direct_error = ret;
	oplus->direct_state = OPLUS_DIRECT_STOPPING;
	dev_warn(battmgr->dev, "PPS BYPASS stopping: %d\n", ret);
stop:
	ret = qcom_battmgr_oplus_direct_exit(battmgr);
	if (ret)
		dev_warn_ratelimited(battmgr->dev, "PPS BYPASS exit pending: %d\n", ret);
reschedule:
	if (oplus->direct_state != OPLUS_DIRECT_IDLE && !READ_ONCE(oplus->direct_shutdown))
		schedule_delayed_work(&battmgr->oplus_direct_work, HZ);
out_unlock:
	mutex_unlock(&battmgr->lock);
}

static int qcom_battmgr_oplus_direct_set(void *data, u64 command)
{
	struct qcom_battmgr *battmgr = data;
	struct qcom_battmgr_oplus *oplus = &battmgr->oplus;
	u32 target_current;
	int ret = 0;

	if (command > 2)
		return -EINVAL;
	mutex_lock(&battmgr->lock);
	if (READ_ONCE(oplus->direct_shutdown)) {
		ret = -ESHUTDOWN;
		goto out_unlock;
	}
	if (!command) {
		if (oplus->direct_state == OPLUS_DIRECT_IDLE)
			goto out_unlock;
		oplus->direct_state = OPLUS_DIRECT_STOPPING;
		ret = qcom_battmgr_oplus_direct_exit(battmgr);
		if (ret)
			mod_delayed_work(system_percpu_wq, &battmgr->oplus_direct_work, HZ);
		goto out_unlock;
	}
	if (command == 2) {
		if (oplus->direct_state != OPLUS_DIRECT_PRECHARGE &&
		    oplus->direct_state != OPLUS_DIRECT_RUNNING) {
			ret = -EINVAL;
			goto out_unlock;
		}
		/* An expired session cannot be resurrected by a late heartbeat. */
		if (READ_ONCE(oplus->direct_abort) ||
		    time_after_eq(jiffies, oplus->direct_deadline)) {
			ret = -ECANCELED;
			mod_delayed_work(system_percpu_wq, &battmgr->oplus_direct_work, 0);
			goto out_unlock;
		}
		oplus->direct_deadline = jiffies + OPLUS_DIRECT_LEASE;
		goto out_unlock;
	}
	if (oplus->direct_state != OPLUS_DIRECT_IDLE) {
		ret = -EBUSY;
		goto out_unlock;
	}
	oplus->direct_cell_current_ma = OPLUS_DIRECT_MAX_CURR_MA;
	oplus->direct_thermal_derated = false;
	oplus->direct_connector_derated = false;
	ret = qcom_battmgr_oplus_direct_sample(battmgr, &target_current);
	if (ret)
		goto out_unlock;
	if (oplus->mos_state != 0) {
		ret = -EBUSY;
		goto out_unlock;
	}
	WRITE_ONCE(oplus->direct_abort, false);
	oplus->direct_mos_pending = false;
	oplus->direct_low_current_count = 0;
	oplus->direct_high_current_count = 0;
	oplus->direct_error = 0;
	oplus->direct_state = OPLUS_DIRECT_PRECHARGE;
	oplus->direct_deadline = jiffies + OPLUS_DIRECT_LEASE;
	oplus->direct_start_deadline = jiffies + 30 * HZ;
	__pm_stay_awake(battmgr->oplus_direct_ws);
	ret = qcom_battmgr_request_property(battmgr, BATTMGR_BAT_PROPERTY_SET,
					    BATT_CHG_CTRL_LIM, 500000);
	if (!ret)
		ret = qcom_battmgr_request_property(battmgr, BATTMGR_USB_PROPERTY_SET,
						    USB_INPUT_CURR_LIMIT, 0);
	if (!ret)
		ret = qcom_battmgr_oplus_direct_pps(battmgr, 5000, 1000);
	if (ret) {
		oplus->direct_error = ret > 0 ? -EREMOTEIO : ret;
		oplus->direct_state = OPLUS_DIRECT_STOPPING;
		qcom_battmgr_oplus_direct_exit(battmgr);
	}
	mod_delayed_work(system_percpu_wq, &battmgr->oplus_direct_work, HZ);
out_unlock:
	mutex_unlock(&battmgr->lock);
	return ret > 0 ? -EREMOTEIO : ret;
}
DEFINE_DEBUGFS_ATTRIBUTE(qcom_battmgr_oplus_direct_control_fops, NULL,
			 qcom_battmgr_oplus_direct_set, "%llu\n");

static int qcom_battmgr_oplus_direct_show(struct seq_file *s, void *unused)
{
	struct qcom_battmgr *battmgr = s->private;
	struct qcom_battmgr_oplus *oplus = &battmgr->oplus;

	mutex_lock(&battmgr->lock);
	seq_printf(s, "state=%u error=%d target_voltage=%u mV request=%u mV/%u mA\n",
		   oplus->direct_state, oplus->direct_error, oplus->direct_target_mv,
		   oplus->pps_voltage_mv, oplus->pps_current_ma);
	seq_printf(s, "input=%u mV/%u mA mos=%u temperature=%d/%d/%d mC\n",
		   oplus->direct_input_mv, oplus->direct_input_ma, oplus->mos_state,
		   oplus->direct_temperatures[0], oplus->direct_temperatures[1],
		   oplus->direct_temperatures[2]);
	mutex_unlock(&battmgr->lock);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(qcom_battmgr_oplus_direct);

static void qcom_battmgr_oplus_direct_shutdown(struct auxiliary_device *adev)
{
	struct qcom_battmgr *battmgr = dev_get_drvdata(&adev->dev);
	int ret = 0, i;

	WRITE_ONCE(battmgr->oplus.direct_shutdown, true);
	WRITE_ONCE(battmgr->oplus.direct_abort, true);
	cancel_delayed_work_sync(&battmgr->oplus_direct_work);
	mutex_lock(&battmgr->lock);
	if (battmgr->oplus.direct_state != OPLUS_DIRECT_IDLE) {
		battmgr->oplus.direct_state = OPLUS_DIRECT_STOPPING;
		for (i = 0; i < 3; i++) {
			ret = qcom_battmgr_oplus_direct_exit(battmgr);
			if (!ret)
				break;
		}
		if (ret)
			dev_err(battmgr->dev, "failed to stop PPS BYPASS at shutdown: %d\n", ret);
	}
	mutex_unlock(&battmgr->lock);
}

static void qcom_battmgr_oplus_direct_cleanup(void *data)
{
	struct qcom_battmgr *battmgr = data;

	qcom_battmgr_oplus_direct_shutdown(to_auxiliary_dev(battmgr->dev));
	wakeup_source_unregister(battmgr->oplus_direct_ws);
}

static int qcom_battmgr_oplus_suspend(struct auxiliary_device *adev, pm_message_t state)
{
	struct qcom_battmgr *battmgr = dev_get_drvdata(&adev->dev);
	int ret;

	mutex_lock(&battmgr->lock);
	ret = battmgr->oplus.direct_state == OPLUS_DIRECT_IDLE ? 0 : -EBUSY;
	mutex_unlock(&battmgr->lock);
	return ret;
}

static void qcom_battmgr_debugfs_remove(void *data)
{
	debugfs_remove_recursive(data);
}

static int qcom_battmgr_usb_get_property(struct power_supply *psy,
					 enum power_supply_property psp,
					 union power_supply_propval *val)
{
	struct qcom_battmgr *battmgr = power_supply_get_drvdata(psy);
	int ret;

	if (!battmgr->service_up)
		return -EAGAIN;

	if (battmgr->variant == QCOM_BATTMGR_SC8280XP ||
	    battmgr->variant == QCOM_BATTMGR_X1E80100)
		ret = qcom_battmgr_bat_sc8280xp_update(battmgr, psp);
	else
		ret = qcom_battmgr_usb_sm8350_update(battmgr, psp);
	if (ret)
		return ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = battmgr->usb.online;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = battmgr->usb.voltage_now;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		val->intval = battmgr->usb.voltage_max;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		val->intval = battmgr->usb.current_now;
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		val->intval = battmgr->usb.current_max;
		break;
	case POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT:
		val->intval = battmgr->usb.current_limit;
		break;
	case POWER_SUPPLY_PROP_USB_TYPE:
		val->intval = battmgr->usb.usb_type;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static const enum power_supply_property sc8280xp_usb_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
};

static const struct power_supply_desc sc8280xp_usb_psy_desc = {
	.name = "qcom-battmgr-usb",
	.type = POWER_SUPPLY_TYPE_USB,
	.properties = sc8280xp_usb_props,
	.num_properties = ARRAY_SIZE(sc8280xp_usb_props),
	.get_property = qcom_battmgr_usb_get_property,
	.usb_types = BIT(POWER_SUPPLY_USB_TYPE_UNKNOWN) |
		     BIT(POWER_SUPPLY_USB_TYPE_SDP)     |
		     BIT(POWER_SUPPLY_USB_TYPE_DCP)     |
		     BIT(POWER_SUPPLY_USB_TYPE_CDP)     |
		     BIT(POWER_SUPPLY_USB_TYPE_ACA)     |
		     BIT(POWER_SUPPLY_USB_TYPE_C)       |
		     BIT(POWER_SUPPLY_USB_TYPE_PD)      |
		     BIT(POWER_SUPPLY_USB_TYPE_PD_DRP)  |
		     BIT(POWER_SUPPLY_USB_TYPE_PD_PPS)  |
		     BIT(POWER_SUPPLY_USB_TYPE_APPLE_BRICK_ID),
};

static const enum power_supply_property sm8350_usb_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CURRENT_MAX,
	POWER_SUPPLY_PROP_INPUT_CURRENT_LIMIT,
	POWER_SUPPLY_PROP_USB_TYPE,
};

static const struct power_supply_desc sm8350_usb_psy_desc = {
	.name = "qcom-battmgr-usb",
	.type = POWER_SUPPLY_TYPE_USB,
	.properties = sm8350_usb_props,
	.num_properties = ARRAY_SIZE(sm8350_usb_props),
	.get_property = qcom_battmgr_usb_get_property,
	.set_property = qcom_battmgr_usb_set_property,
	.property_is_writeable = qcom_battmgr_usb_property_is_writeable,
	.usb_types = BIT(POWER_SUPPLY_USB_TYPE_UNKNOWN) |
		     BIT(POWER_SUPPLY_USB_TYPE_SDP)     |
		     BIT(POWER_SUPPLY_USB_TYPE_DCP)     |
		     BIT(POWER_SUPPLY_USB_TYPE_CDP)     |
		     BIT(POWER_SUPPLY_USB_TYPE_ACA)     |
		     BIT(POWER_SUPPLY_USB_TYPE_C)       |
		     BIT(POWER_SUPPLY_USB_TYPE_PD)      |
		     BIT(POWER_SUPPLY_USB_TYPE_PD_DRP)  |
		     BIT(POWER_SUPPLY_USB_TYPE_PD_PPS)  |
		     BIT(POWER_SUPPLY_USB_TYPE_APPLE_BRICK_ID),
};

static const u8 sm8350_wls_prop_map[] = {
	[POWER_SUPPLY_PROP_ONLINE] = WLS_ONLINE,
	[POWER_SUPPLY_PROP_VOLTAGE_NOW] = WLS_VOLT_NOW,
	[POWER_SUPPLY_PROP_VOLTAGE_MAX] = WLS_VOLT_MAX,
	[POWER_SUPPLY_PROP_CURRENT_NOW] = WLS_CURR_NOW,
	[POWER_SUPPLY_PROP_CURRENT_MAX] = WLS_CURR_MAX,
};

static int qcom_battmgr_wls_sm8350_update(struct qcom_battmgr *battmgr,
					  enum power_supply_property psp)
{
	unsigned int prop;
	int ret;

	if (psp >= ARRAY_SIZE(sm8350_wls_prop_map))
		return -EINVAL;

	prop = sm8350_wls_prop_map[psp];

	mutex_lock(&battmgr->lock);
	ret = qcom_battmgr_request_property(battmgr, BATTMGR_WLS_PROPERTY_GET, prop, 0);
	mutex_unlock(&battmgr->lock);

	return ret;
}

static int qcom_battmgr_wls_get_property(struct power_supply *psy,
					 enum power_supply_property psp,
					 union power_supply_propval *val)
{
	struct qcom_battmgr *battmgr = power_supply_get_drvdata(psy);
	int ret;

	if (!battmgr->service_up)
		return -EAGAIN;

	if (battmgr->variant == QCOM_BATTMGR_SC8280XP ||
	    battmgr->variant == QCOM_BATTMGR_X1E80100)
		ret = qcom_battmgr_bat_sc8280xp_update(battmgr, psp);
	else
		ret = qcom_battmgr_wls_sm8350_update(battmgr, psp);
	if (ret < 0)
		return ret;

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = battmgr->wireless.online;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = battmgr->wireless.voltage_now;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		val->intval = battmgr->wireless.voltage_max;
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		val->intval = battmgr->wireless.current_now;
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		val->intval = battmgr->wireless.current_max;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static const enum power_supply_property sc8280xp_wls_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
};

static const struct power_supply_desc sc8280xp_wls_psy_desc = {
	.name = "qcom-battmgr-wls",
	.type = POWER_SUPPLY_TYPE_WIRELESS,
	.properties = sc8280xp_wls_props,
	.num_properties = ARRAY_SIZE(sc8280xp_wls_props),
	.get_property = qcom_battmgr_wls_get_property,
};

static const enum power_supply_property sm8350_wls_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CURRENT_MAX,
};

static const struct power_supply_desc sm8350_wls_psy_desc = {
	.name = "qcom-battmgr-wls",
	.type = POWER_SUPPLY_TYPE_WIRELESS,
	.properties = sm8350_wls_props,
	.num_properties = ARRAY_SIZE(sm8350_wls_props),
	.get_property = qcom_battmgr_wls_get_property,
};

static void qcom_battmgr_notification(struct qcom_battmgr *battmgr,
				      const struct qcom_battmgr_message *msg,
				      int len)
{
	size_t payload_len = len - sizeof(struct pmic_glink_hdr);
	unsigned int notification;

	if (payload_len != sizeof(msg->notification)) {
		dev_warn(battmgr->dev, "ignoring notification with invalid length\n");
		return;
	}

	notification = le32_to_cpu(msg->notification);
	notification &= 0xff;
	if (battmgr->oplus_otg_control) {
		if (notification == NOTIF_CP_MOS_DISABLE ||
		    notification == NOTIF_PD_CONNECT_HARD_RESET ||
		    notification == NOTIF_OPLUS_OTG_ENABLE) {
			WRITE_ONCE(battmgr->oplus.direct_abort, true);
			if (READ_ONCE(battmgr->oplus.direct_state) != OPLUS_DIRECT_IDLE &&
			    !READ_ONCE(battmgr->oplus.direct_shutdown))
				mod_delayed_work(system_percpu_wq, &battmgr->oplus_direct_work, 0);
		}
		switch (notification) {
		case NOTIF_OPLUS_OTG_ENABLE:
			WRITE_ONCE(battmgr->oplus.otg_state, true);
			schedule_work(&battmgr->oplus_otg_work);
			break;
		case NOTIF_OPLUS_OTG_DISABLE:
			WRITE_ONCE(battmgr->oplus.otg_state, false);
			schedule_work(&battmgr->oplus_otg_work);
			break;
		}
	}

	switch (notification) {
	case NOTIF_BAT_INFO:
		battmgr->info.valid = false;
		fallthrough;
	case NOTIF_BAT_STATUS:
	case NOTIF_BAT_PROPERTY:
	case NOTIF_BAT_CHARGING_STATE:
		power_supply_changed(battmgr->bat_psy);
		break;
	case NOTIF_USB_PROPERTY:
		power_supply_changed(battmgr->usb_psy);
		break;
	case NOTIF_TYPEC_STATE_CHANGE:
	case NOTIF_CID_DETECT:
	case NOTIF_PLUGIN_IRQ:
	case NOTIF_APSD_DONE:
	case NOTIF_CP_MOS_DISABLE:
	case NOTIF_POWER_ROLE_STATUS:
	case NOTIF_PD_SOURCE_CAPS:
	case NOTIF_PD_CONNECT_HARD_RESET:
	case NOTIF_OPLUS_OTG_ENABLE:
	case NOTIF_OPLUS_OTG_DISABLE:
		power_supply_changed(battmgr->bat_psy);
		power_supply_changed(battmgr->usb_psy);
		break;
	case NOTIF_WLS_PROPERTY:
		power_supply_changed(battmgr->wls_psy);
		break;
	default:
		dev_err(battmgr->dev, "unknown notification: %#x\n", notification);
		break;
	}
}

static void qcom_battmgr_sc8280xp_strcpy(char *dest, const char *src)
{
	size_t len = src[0];

	/* Some firmware versions return Pascal-style strings */
	if (len < BATTMGR_STRING_LEN && len == strnlen(src + 1, BATTMGR_STRING_LEN - 1)) {
		memcpy(dest, src + 1, len);
		dest[len] = '\0';
	} else {
		strscpy(dest, src, BATTMGR_STRING_LEN);
	}
}

static unsigned int qcom_battmgr_sc8280xp_parse_technology(const char *chemistry)
{
	if ((!strncmp(chemistry, "LIO", 3)) ||
	    (!strncmp(chemistry, "OOI", 3)))
		return POWER_SUPPLY_TECHNOLOGY_LION;
	if (!strncmp(chemistry, "LIP", 3) ||
	    !strncmp(chemistry, "LiP", 3))
		return POWER_SUPPLY_TECHNOLOGY_LIPO;

	pr_err("Unknown battery technology '%s'\n", chemistry);
	return POWER_SUPPLY_TECHNOLOGY_UNKNOWN;
}

static unsigned int qcom_battmgr_sc8280xp_convert_temp(unsigned int temperature)
{
	return DIV_ROUND_CLOSEST(temperature, 10);
}

static void qcom_battmgr_sc8280xp_callback(struct qcom_battmgr *battmgr,
					   const struct qcom_battmgr_message *resp,
					   size_t len)
{
	unsigned int opcode = le32_to_cpu(resp->hdr.opcode);
	unsigned int source;
	unsigned int state;
	size_t payload_len = len - sizeof(struct pmic_glink_hdr);

	if (payload_len < sizeof(__le32)) {
		dev_warn(battmgr->dev, "invalid payload length for %#x: %zd\n",
			 opcode, len);
		return;
	}

	switch (opcode) {
	case BATTMGR_REQUEST_NOTIFICATION:
		battmgr->error = 0;
		break;
	case BATTMGR_BAT_INFO:
		/* some firmware versions report an extra __le32 at the end of the payload */
		if (payload_len != sizeof(resp->info) &&
		    payload_len != (sizeof(resp->info) + sizeof(__le32))) {
			dev_warn(battmgr->dev,
				 "invalid payload length for battery information request: %zd\n",
				 payload_len);
			battmgr->error = -ENODATA;
			return;
		}

		battmgr->unit = le32_to_cpu(resp->info.power_unit);

		battmgr->info.present = true;
		battmgr->info.design_capacity = le32_to_cpu(resp->info.design_capacity) * 1000;
		battmgr->info.last_full_capacity = le32_to_cpu(resp->info.last_full_capacity) * 1000;
		battmgr->info.voltage_max_design = le32_to_cpu(resp->info.design_voltage) * 1000;
		battmgr->info.capacity_low = le32_to_cpu(resp->info.capacity_low) * 1000;
		battmgr->info.cycle_count = le32_to_cpu(resp->info.cycle_count);
		qcom_battmgr_sc8280xp_strcpy(battmgr->info.model_number, resp->info.model_number);
		qcom_battmgr_sc8280xp_strcpy(battmgr->info.serial_number, resp->info.serial_number);
		battmgr->info.technology = qcom_battmgr_sc8280xp_parse_technology(resp->info.battery_chemistry);
		qcom_battmgr_sc8280xp_strcpy(battmgr->info.oem_info, resp->info.oem_info);
		battmgr->info.day = resp->info.day;
		battmgr->info.month = resp->info.month;
		battmgr->info.year = le16_to_cpu(resp->info.year);
		break;
	case BATTMGR_BAT_STATUS:
		if (payload_len != sizeof(resp->status)) {
			dev_warn(battmgr->dev,
				 "invalid payload length for battery status request: %zd\n",
				 payload_len);
			battmgr->error = -ENODATA;
			return;
		}

		state = le32_to_cpu(resp->status.battery_state);
		if (state & BIT(0))
			battmgr->status.status = POWER_SUPPLY_STATUS_DISCHARGING;
		else if (state & BIT(1))
			battmgr->status.status = POWER_SUPPLY_STATUS_CHARGING;
		else
			battmgr->status.status = POWER_SUPPLY_STATUS_NOT_CHARGING;

		battmgr->status.capacity = le32_to_cpu(resp->status.capacity) * 1000;
		battmgr->status.power_now = le32_to_cpu(resp->status.rate) * 1000;
		battmgr->status.voltage_now = le32_to_cpu(resp->status.battery_voltage) * 1000;
		battmgr->status.temperature = qcom_battmgr_sc8280xp_convert_temp(le32_to_cpu(resp->status.temperature));

		source = le32_to_cpu(resp->status.charging_source);
		battmgr->ac.online = source == BATTMGR_CHARGING_SOURCE_AC;
		battmgr->usb.online = source == BATTMGR_CHARGING_SOURCE_USB;
		battmgr->wireless.online = source == BATTMGR_CHARGING_SOURCE_WIRELESS;
		if (battmgr->info.last_full_capacity != 0) {
			/*
			 * 100 * battmgr->status.capacity can overflow a 32bit
			 * unsigned integer. FW readings are in m{W/A}h, which
			 * are multiplied by 1000 converting them to u{W/A}h,
			 * the format the power_supply API expects.
			 * To avoid overflow use the original value for dividend
			 * and convert the divider back to m{W/A}h, which can be
			 * done without any loss of precision.
			 */
			battmgr->status.percent =
				(100 * le32_to_cpu(resp->status.capacity)) /
				(battmgr->info.last_full_capacity / 1000);
		} else {
			/*
			 * Let the sysfs handler know no data is available at
			 * this time.
			 */
			battmgr->status.percent = (unsigned int)-1;
		}
		break;
	case BATTMGR_BAT_DISCHARGE_TIME:
		battmgr->status.discharge_time = le32_to_cpu(resp->time);
		break;
	case BATTMGR_BAT_CHARGE_TIME:
		battmgr->status.charge_time = le32_to_cpu(resp->time);
		break;
	case BATTMGR_CHG_CTRL_LIMIT_EN:
		battmgr->error = 0;
		break;
	default:
		dev_warn(battmgr->dev, "unknown message %#x\n", opcode);
		break;
	}

	complete(&battmgr->ack);
}

static void qcom_battmgr_sm8350_callback(struct qcom_battmgr *battmgr,
					 const struct qcom_battmgr_message *resp,
					 size_t len)
{
	unsigned int property;
	unsigned int opcode = le32_to_cpu(resp->hdr.opcode);
	size_t payload_len = len - sizeof(struct pmic_glink_hdr);
	unsigned int val;

	if (payload_len < sizeof(__le32)) {
		dev_warn(battmgr->dev, "invalid payload length for %#x: %zd\n",
			 opcode, len);
		return;
	}

	switch (opcode) {
	case BATTMGR_BAT_PROPERTY_GET:
		property = le32_to_cpu(resp->intval.property);
		if (property == BATT_MODEL_NAME) {
			if (payload_len != sizeof(resp->strval)) {
				dev_warn(battmgr->dev,
					 "invalid payload length for BATT_MODEL_NAME request: %zd\n",
					 payload_len);
				battmgr->error = -ENODATA;
				return;
			}
		} else {
			if (payload_len != sizeof(resp->intval)) {
				dev_warn(battmgr->dev,
					 "invalid payload length for %#x request: %zd\n",
					 property, payload_len);
				battmgr->error = -ENODATA;
				return;
			}

			battmgr->error = le32_to_cpu(resp->intval.result);
			if (battmgr->error)
				goto out_complete;
		}

		switch (property) {
		case BATT_STATUS:
			battmgr->status.status = le32_to_cpu(resp->intval.value);
			break;
		case BATT_HEALTH:
			battmgr->status.health = le32_to_cpu(resp->intval.value);
			break;
		case BATT_PRESENT:
			battmgr->info.present = le32_to_cpu(resp->intval.value);
			break;
		case BATT_CHG_TYPE:
			battmgr->info.charge_type = le32_to_cpu(resp->intval.value);
			break;
		case BATT_CAPACITY:
			battmgr->status.percent = le32_to_cpu(resp->intval.value) / 100;
			break;
		case BATT_SOH:
			battmgr->status.soh_percent = le32_to_cpu(resp->intval.value);
			break;
		case BATT_VOLT_OCV:
			battmgr->status.voltage_ocv = le32_to_cpu(resp->intval.value);
			break;
		case BATT_VOLT_NOW:
			battmgr->status.voltage_now = le32_to_cpu(resp->intval.value);
			break;
		case BATT_VOLT_MAX:
			battmgr->info.voltage_max = le32_to_cpu(resp->intval.value);
			break;
		case BATT_CURR_NOW:
			battmgr->status.current_now = le32_to_cpu(resp->intval.value);
			break;
		case BATT_CHG_CTRL_LIM:
			battmgr->oplus.charge_current = le32_to_cpu(resp->intval.value);
			break;
		case BATT_CHG_CTRL_LIM_MAX:
			battmgr->oplus.charge_current_max = le32_to_cpu(resp->intval.value);
			break;
		case BATT_TEMP:
			val = le32_to_cpu(resp->intval.value);
			battmgr->status.temperature = DIV_ROUND_CLOSEST(val, 10);
			break;
		case BATT_TECHNOLOGY:
			battmgr->info.technology = le32_to_cpu(resp->intval.value);
			break;
		case BATT_CHG_COUNTER:
			battmgr->info.charge_count = le32_to_cpu(resp->intval.value);
			break;
		case BATT_CYCLE_COUNT:
			battmgr->info.cycle_count = le32_to_cpu(resp->intval.value);
			break;
		case BATT_CHG_FULL_DESIGN:
			battmgr->info.design_capacity = le32_to_cpu(resp->intval.value);
			break;
		case BATT_CHG_FULL:
			battmgr->info.last_full_capacity = le32_to_cpu(resp->intval.value);
			break;
		case BATT_MODEL_NAME:
			strscpy(battmgr->info.model_number, resp->strval.model, BATTMGR_STRING_LEN);
			break;
		case BATT_TTF_AVG:
			battmgr->status.charge_time = le32_to_cpu(resp->intval.value);
			break;
		case BATT_TTE_AVG:
			battmgr->status.discharge_time = le32_to_cpu(resp->intval.value);
			break;
		case BATT_RESISTANCE:
			battmgr->status.resistance = le32_to_cpu(resp->intval.value);
			break;
		case BATT_POWER_NOW:
			battmgr->status.power_now = le32_to_cpu(resp->intval.value);
			break;
		case BATT_CHG_CTRL_START_THR:
			battmgr->info.charge_ctrl_start = le32_to_cpu(resp->intval.value);
			break;
		case BATT_CHG_CTRL_END_THR:
			battmgr->info.charge_ctrl_end = le32_to_cpu(resp->intval.value);
			break;
		default:
			dev_warn(battmgr->dev, "unknown property %#x\n", property);
			break;
		}
		break;
	case BATTMGR_USB_PROPERTY_GET:
		property = le32_to_cpu(resp->intval.property);
		if (payload_len != sizeof(resp->intval)) {
			dev_warn(battmgr->dev,
				 "invalid payload length for %#x request: %zd\n",
				 property, payload_len);
			battmgr->error = -ENODATA;
			return;
		}

		battmgr->error = le32_to_cpu(resp->intval.result);
		if (battmgr->error)
			goto out_complete;

		switch (property) {
		case USB_ONLINE:
			battmgr->usb.online = le32_to_cpu(resp->intval.value);
			if (!battmgr->usb.online &&
			    battmgr->oplus.direct_state == OPLUS_DIRECT_IDLE) {
				battmgr->oplus.pps_voltage_mv = 5000;
				battmgr->oplus.pps_current_ma = 1000;
			}
			break;
		case USB_VOLT_NOW:
			battmgr->usb.voltage_now = le32_to_cpu(resp->intval.value);
			break;
		case USB_VOLT_MAX:
			battmgr->usb.voltage_max = le32_to_cpu(resp->intval.value);
			break;
		case USB_CURR_NOW:
			battmgr->usb.current_now = le32_to_cpu(resp->intval.value);
			break;
		case USB_CURR_MAX:
			battmgr->usb.current_max = le32_to_cpu(resp->intval.value);
			break;
		case USB_INPUT_CURR_LIMIT:
			battmgr->usb.current_limit = le32_to_cpu(resp->intval.value);
			break;
		case USB_TYPE:
		case USB_ADAP_TYPE:
			battmgr->usb.usb_type = le32_to_cpu(resp->intval.value);
			break;
		case OPLUS_USB_GET_PPS_STATUS:
			battmgr->oplus.pps_status = le32_to_cpu(resp->intval.value);
			break;
		case OPLUS_USB_PPS_MOS_CTRL:
			battmgr->oplus.mos_state = le32_to_cpu(resp->intval.value);
			break;
		default:
			dev_warn(battmgr->dev, "unknown property %#x\n", property);
			break;
		}
		break;
	case BATTMGR_BAT_PROPERTY_SET:
	case BATTMGR_USB_PROPERTY_SET:
		if (payload_len != sizeof(resp->intval)) {
			dev_warn(battmgr->dev,
				 "invalid payload length for property set: %zd\n",
				 payload_len);
			battmgr->error = -ENODATA;
			return;
		}

		battmgr->error = le32_to_cpu(resp->intval.result);
		break;
	case BATTMGR_WLS_PROPERTY_GET:
		property = le32_to_cpu(resp->intval.property);
		if (payload_len != sizeof(resp->intval)) {
			dev_warn(battmgr->dev,
				 "invalid payload length for %#x request: %zd\n",
				 property, payload_len);
			battmgr->error = -ENODATA;
			return;
		}

		battmgr->error = le32_to_cpu(resp->intval.result);
		if (battmgr->error)
			goto out_complete;

		switch (property) {
		case WLS_ONLINE:
			battmgr->wireless.online = le32_to_cpu(resp->intval.value);
			break;
		case WLS_VOLT_NOW:
			battmgr->wireless.voltage_now = le32_to_cpu(resp->intval.value);
			break;
		case WLS_VOLT_MAX:
			battmgr->wireless.voltage_max = le32_to_cpu(resp->intval.value);
			break;
		case WLS_CURR_NOW:
			battmgr->wireless.current_now = le32_to_cpu(resp->intval.value);
			break;
		case WLS_CURR_MAX:
			battmgr->wireless.current_max = le32_to_cpu(resp->intval.value);
			break;
		default:
			dev_warn(battmgr->dev, "unknown property %#x\n", property);
			break;
		}
		break;
	case BATTMGR_REQUEST_NOTIFICATION:
	case BATTMGR_CHG_CTRL_LIMIT_EN:
		battmgr->error = 0;
		break;
	default:
		dev_warn(battmgr->dev, "unknown message %#x\n", opcode);
		break;
	}

out_complete:
	complete(&battmgr->ack);
}

static void qcom_battmgr_oplus_buffer_callback(struct qcom_battmgr *battmgr,
					       const struct qcom_battmgr_oplus_buffer_resp *resp,
					       size_t len)
{
	u32 data_size;
	int i, count;

	if (len != sizeof(*resp)) {
		battmgr->error = -EMSGSIZE;
		goto out_complete;
	}
	data_size = le32_to_cpu(resp->data_size);
	if (data_size < 3 * sizeof(__le32) || data_size > sizeof(resp->data) ||
	    data_size % sizeof(__le32)) {
		battmgr->error = -ENODATA;
		goto out_complete;
	}
	if (le32_to_cpu(resp->hdr.opcode) == OPLUS_OEM_READ_BUFFER) {
		if (data_size < 15 * sizeof(__le32)) {
			battmgr->error = -ENODATA;
			goto out_complete;
		}
		battmgr->oplus.oem_words = data_size / sizeof(__le32);
		for (i = 0; i < battmgr->oplus.oem_words; i++)
			battmgr->oplus.oem_data[i] = le32_to_cpu(resp->data[i]);
		goto out_complete;
	}

	battmgr->oplus.pps_imax = le32_to_cpu(resp->data[0]);
	battmgr->oplus.pps_vmax = le32_to_cpu(resp->data[1]);
	count = min_t(unsigned int, data_size / sizeof(__le32) - 2, PDO_MAX_OBJECTS);
	for (i = 0; i < count; i++)
		battmgr->oplus.pps_pdos[i] = le32_to_cpu(resp->data[i + 2]);

out_complete:
	complete(&battmgr->ack);
}

static void qcom_battmgr_callback(const void *data, size_t len, void *priv)
{
	const struct pmic_glink_hdr *hdr = data;
	struct qcom_battmgr *battmgr = priv;
	unsigned int opcode;

	if (len < sizeof(*hdr))
		return;
	opcode = le32_to_cpu(hdr->opcode);

	if (opcode == BATTMGR_NOTIFICATION) {
		qcom_battmgr_notification(battmgr, data, len);
		return;
	}
	/* A late, unrelated reply must not acknowledge a safety-critical write. */
	if (battmgr->oplus_otg_control) {
		/* Pair with the publication in qcom_battmgr_request(). */
		if (!smp_load_acquire(&battmgr->request_pending) ||
		    opcode != battmgr->request_opcode)
			return;
		if (battmgr->request_has_property) {
			const struct qcom_battmgr_message *resp = data;

			if (len < sizeof(*hdr) + sizeof(__le32))
				return;
			if (le32_to_cpu(resp->intval.property) != battmgr->request_property) {
				dev_warn_ratelimited(battmgr->dev,
						     "reply %#x property %u, expected %u\n", opcode,
						     le32_to_cpu(resp->intval.property),
						     battmgr->request_property);
				return;
			}
		}
	}
	if ((opcode == OPLUS_PPS_READ_BUFFER || opcode == OPLUS_OEM_READ_BUFFER) &&
	    battmgr->oplus_otg_control)
		qcom_battmgr_oplus_buffer_callback(battmgr, data, len);
	else if (battmgr->variant == QCOM_BATTMGR_SC8280XP ||
		 battmgr->variant == QCOM_BATTMGR_X1E80100)
		qcom_battmgr_sc8280xp_callback(battmgr, data, len);
	else
		qcom_battmgr_sm8350_callback(battmgr, data, len);
}

static void qcom_battmgr_enable_worker(struct work_struct *work)
{
	struct qcom_battmgr *battmgr = container_of(work, struct qcom_battmgr, enable_work);
	struct qcom_battmgr_enable_request req = {
		.hdr.owner = cpu_to_le32(PMIC_GLINK_OWNER_BATTMGR),
		.hdr.type = cpu_to_le32(PMIC_GLINK_NOTIFY),
		.hdr.opcode = cpu_to_le32(BATTMGR_REQUEST_NOTIFICATION),
	};
	int ret;

	mutex_lock(&battmgr->lock);
	ret = qcom_battmgr_request(battmgr, &req, sizeof(req));
	mutex_unlock(&battmgr->lock);
	if (ret)
		dev_err(battmgr->dev, "failed to request power notifications\n");

	if (battmgr->oplus_otg_control) {
		mutex_lock(&battmgr->lock);
		ret = qcom_battmgr_request_property(battmgr,
						    BATTMGR_USB_PROPERTY_SET,
						    OPLUS_USB_OTG_AP_ENABLE, 1);
		mutex_unlock(&battmgr->lock);
		if (ret)
			dev_err(battmgr->dev, "failed to enable OnePlus AP OTG control: %d\n", ret);
	}
}

static void qcom_battmgr_oplus_otg_worker(struct work_struct *work)
{
	struct qcom_battmgr *battmgr =
		container_of(work, struct qcom_battmgr, oplus_otg_work);
	bool enable = READ_ONCE(battmgr->oplus.otg_state);
	int ret = 0;

	if (!READ_ONCE(battmgr->service_up))
		return;

	mutex_lock(&battmgr->lock);
	if (enable && battmgr->oplus.direct_state != OPLUS_DIRECT_IDLE) {
		ret = -EBUSY;
		goto out_unlock;
	}
	if (enable) {
		ret = qcom_battmgr_request_property(battmgr,
						    BATTMGR_USB_PROPERTY_SET,
						    OPLUS_USB_OTG_BOOST_CURRENT, 2000);
		if (ret)
			goto out_unlock;
	}

	ret = qcom_battmgr_request_property(battmgr,
						    BATTMGR_USB_PROPERTY_SET,
						    OPLUS_USB_OTG_VBUS_REGULATOR_ENABLE,
						    enable);

out_unlock:
	mutex_unlock(&battmgr->lock);

	if (ret)
		dev_err(battmgr->dev, "failed to %s OnePlus OTG VBUS: %d\n",
			enable ? "enable" : "disable", ret);
	power_supply_changed(battmgr->bat_psy);
	power_supply_changed(battmgr->usb_psy);
}

static void qcom_battmgr_pdr_notify(void *priv, int state)
{
	struct qcom_battmgr *battmgr = priv;

	if (state == SERVREG_SERVICE_STATE_UP) {
		battmgr->service_up = true;
		schedule_work(&battmgr->enable_work);
	} else {
		battmgr->service_up = false;
		WRITE_ONCE(battmgr->oplus.otg_state, false);
		WRITE_ONCE(battmgr->oplus.direct_abort, true);
		if (battmgr->oplus_otg_control &&
		    READ_ONCE(battmgr->oplus.direct_state) != OPLUS_DIRECT_IDLE &&
		    !READ_ONCE(battmgr->oplus.direct_shutdown))
			mod_delayed_work(system_percpu_wq, &battmgr->oplus_direct_work, 0);
	}
}

static const struct of_device_id qcom_battmgr_of_variants[] = {
	{ .compatible = "qcom,glymur-pmic-glink", .data = (void *)QCOM_BATTMGR_X1E80100 },
	{ .compatible = "qcom,kaanapali-pmic-glink", .data = (void *)QCOM_BATTMGR_SM8550 },
	{ .compatible = "qcom,sc8180x-pmic-glink", .data = (void *)QCOM_BATTMGR_SC8280XP },
	{ .compatible = "qcom,sc8280xp-pmic-glink", .data = (void *)QCOM_BATTMGR_SC8280XP },
	{ .compatible = "qcom,sm8550-pmic-glink", .data = (void *)QCOM_BATTMGR_SM8550 },
	{ .compatible = "qcom,x1e80100-pmic-glink", .data = (void *)QCOM_BATTMGR_X1E80100 },
	/* Unmatched devices falls back to QCOM_BATTMGR_SM8350 */
	{}
};

static char *qcom_battmgr_battery[] = { "battery" };

static int qcom_battmgr_probe(struct auxiliary_device *adev,
			      const struct auxiliary_device_id *id)
{
	const struct power_supply_desc *psy_desc;
	struct power_supply_config psy_cfg_supply = {};
	struct power_supply_config psy_cfg = {};
	const struct of_device_id *match;
	struct qcom_battmgr *battmgr;
	struct dentry *debugfs_dir;
	struct device *dev = &adev->dev;
	int ret, i;

	battmgr = devm_kzalloc(dev, sizeof(*battmgr), GFP_KERNEL);
	if (!battmgr)
		return -ENOMEM;

	battmgr->dev = dev;
	dev_set_drvdata(dev, battmgr);
	INIT_DELAYED_WORK(&battmgr->oplus_direct_work, qcom_battmgr_oplus_direct_worker);
	battmgr->status_follows_power_online =
		of_device_is_compatible(dev->of_node, "oneplus,waffle-pmic-glink");
	battmgr->oplus_otg_control = battmgr->status_follows_power_online;
	/* OnePlus reports the negotiated adapter type separately from USB_TYPE. */
	battmgr->usb_type_property = battmgr->oplus_otg_control ? USB_ADAP_TYPE : USB_TYPE;
	battmgr->oplus.pps_voltage_mv = 5000;
	battmgr->oplus.pps_current_ma = 1000;

	psy_cfg.drv_data = battmgr;
	psy_cfg.fwnode = dev_fwnode(&adev->dev);

	psy_cfg_supply.drv_data = battmgr;
	psy_cfg_supply.fwnode = dev_fwnode(&adev->dev);
	psy_cfg_supply.supplied_to = qcom_battmgr_battery;
	psy_cfg_supply.num_supplicants = 1;

	mutex_init(&battmgr->lock);
	init_completion(&battmgr->ack);
	if (battmgr->oplus_otg_control && device_property_present(dev, "io-channels")) {
		if (!IS_REACHABLE(CONFIG_IIO))
			return dev_err_probe(dev, -EOPNOTSUPP,
					     "charging thermistors require IIO\n");
		for (i = 0; i < OPLUS_THERM_COUNT; i++) {
			battmgr->oplus.thermistors[i] =
				devm_iio_channel_get(dev, qcom_battmgr_oplus_thermistor_names[i]);
			if (IS_ERR(battmgr->oplus.thermistors[i]))
				return dev_err_probe(dev, PTR_ERR(battmgr->oplus.thermistors[i]),
						     "failed to get %s thermistor\n",
						     qcom_battmgr_oplus_thermistor_names[i]);
		}
	}

	match = of_match_device(qcom_battmgr_of_variants, dev->parent);
	if (match)
		battmgr->variant = (unsigned long)match->data;
	else
		battmgr->variant = QCOM_BATTMGR_SM8350;

	ret = qcom_battmgr_charge_control_thresholds_init(battmgr);
	if (ret < 0)
		return dev_err_probe(dev, ret,
				     "failed to init battery charge control thresholds\n");

	if (battmgr->variant == QCOM_BATTMGR_SC8280XP ||
	    battmgr->variant == QCOM_BATTMGR_X1E80100) {
		if (battmgr->variant == QCOM_BATTMGR_X1E80100)
			psy_desc = &x1e80100_bat_psy_desc;
		else
			psy_desc = &sc8280xp_bat_psy_desc;

		battmgr->bat_psy = devm_power_supply_register(dev, psy_desc, &psy_cfg);
		if (IS_ERR(battmgr->bat_psy))
			return dev_err_probe(dev, PTR_ERR(battmgr->bat_psy),
					     "failed to register battery power supply\n");

		battmgr->ac_psy = devm_power_supply_register(dev, &sc8280xp_ac_psy_desc, &psy_cfg_supply);
		if (IS_ERR(battmgr->ac_psy))
			return dev_err_probe(dev, PTR_ERR(battmgr->ac_psy),
					     "failed to register AC power supply\n");

		battmgr->usb_psy = devm_power_supply_register(dev, &sc8280xp_usb_psy_desc, &psy_cfg_supply);
		if (IS_ERR(battmgr->usb_psy))
			return dev_err_probe(dev, PTR_ERR(battmgr->usb_psy),
					     "failed to register USB power supply\n");

		battmgr->wls_psy = devm_power_supply_register(dev, &sc8280xp_wls_psy_desc, &psy_cfg_supply);
		if (IS_ERR(battmgr->wls_psy))
			return dev_err_probe(dev, PTR_ERR(battmgr->wls_psy),
					     "failed to register wireless charing power supply\n");
	} else {
		/* The OnePlus firmware uses properties 25/26 for PD/QC requests. */
		if (battmgr->variant == QCOM_BATTMGR_SM8550 && !battmgr->oplus_otg_control)
			psy_desc = &sm8550_bat_psy_desc;
		else
			psy_desc = &sm8350_bat_psy_desc;

		battmgr->bat_psy = devm_power_supply_register(dev, psy_desc, &psy_cfg);
		if (IS_ERR(battmgr->bat_psy))
			return dev_err_probe(dev, PTR_ERR(battmgr->bat_psy),
					     "failed to register battery power supply\n");

		battmgr->usb_psy = devm_power_supply_register(dev, &sm8350_usb_psy_desc, &psy_cfg_supply);
		if (IS_ERR(battmgr->usb_psy))
			return dev_err_probe(dev, PTR_ERR(battmgr->usb_psy),
					     "failed to register USB power supply\n");

		battmgr->wls_psy = devm_power_supply_register(dev, &sm8350_wls_psy_desc, &psy_cfg_supply);
		if (IS_ERR(battmgr->wls_psy))
			return dev_err_probe(dev, PTR_ERR(battmgr->wls_psy),
					     "failed to register wireless charing power supply\n");
	}

	ret = devm_work_autocancel(dev, &battmgr->enable_work,
				   qcom_battmgr_enable_worker);
	if (ret)
		return ret;

	ret = devm_work_autocancel(dev, &battmgr->oplus_otg_work,
				   qcom_battmgr_oplus_otg_worker);
	if (ret)
		return ret;

	battmgr->client = devm_pmic_glink_client_alloc(dev, PMIC_GLINK_OWNER_BATTMGR,
						       qcom_battmgr_callback,
						       qcom_battmgr_pdr_notify,
						       battmgr);
	if (IS_ERR(battmgr->client))
		return PTR_ERR(battmgr->client);

	pmic_glink_client_register(battmgr->client);

	if (battmgr->oplus_otg_control) {
		battmgr->oplus_direct_ws = wakeup_source_register(dev, "waffle-pps-bypass");
		if (!battmgr->oplus_direct_ws)
			return -ENOMEM;
		ret = devm_add_action_or_reset(dev, qcom_battmgr_oplus_direct_cleanup, battmgr);
		if (ret)
			return ret;
		debugfs_dir = debugfs_create_dir("qcom-battmgr", NULL);
		debugfs_create_file("pps", 0400, debugfs_dir, battmgr,
				    &qcom_battmgr_oplus_pps_fops);
		debugfs_create_file("charger", 0400, debugfs_dir, battmgr,
				    &qcom_battmgr_oplus_charger_fops);
		debugfs_create_file("thermal", 0400, debugfs_dir, battmgr,
				    &qcom_battmgr_oplus_thermal_fops);
		debugfs_create_file("mos_state", 0400, debugfs_dir, battmgr,
				    &qcom_battmgr_oplus_mos_fops);
		debugfs_create_file("direct_charge", 0200, debugfs_dir, battmgr,
				    &qcom_battmgr_oplus_direct_control_fops);
		debugfs_create_file("direct_state", 0400, debugfs_dir, battmgr,
				    &qcom_battmgr_oplus_direct_fops);
		debugfs_create_file("charge_current_uA", 0200, debugfs_dir, battmgr,
				    &qcom_battmgr_oplus_fcc_fops);
		debugfs_create_file("input_current_limit_uA", 0200, debugfs_dir, battmgr,
				    &qcom_battmgr_oplus_icl_fops);
		debugfs_create_file("fixed_voltage_mV", 0200, debugfs_dir, battmgr,
				    &qcom_battmgr_oplus_fixed_voltage_fops);
		ret = devm_add_action_or_reset(dev, qcom_battmgr_debugfs_remove, debugfs_dir);
		if (ret)
			return ret;
	}

	return 0;
}

static const struct auxiliary_device_id qcom_battmgr_id_table[] = {
	{ .name = "pmic_glink.power-supply", },
	{},
};
MODULE_DEVICE_TABLE(auxiliary, qcom_battmgr_id_table);

static struct auxiliary_driver qcom_battmgr_driver = {
	.name = "pmic_glink_power_supply",
	.probe = qcom_battmgr_probe,
	.shutdown = qcom_battmgr_oplus_direct_shutdown,
	.suspend = qcom_battmgr_oplus_suspend,
	.id_table = qcom_battmgr_id_table,
};

module_auxiliary_driver(qcom_battmgr_driver);

MODULE_DESCRIPTION("Qualcomm PMIC GLINK battery manager driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("IIO_CONSUMER");
