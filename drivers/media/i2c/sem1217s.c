// SPDX-License-Identifier: GPL-2.0-only
/* SEM1217S focus controller. OIS and nonvolatile firmware are left untouched. */
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>

#define SEM1217S_AF_CONTROL 0x0200
#define SEM1217S_AF_POSITION 0x0204
#define SEM1217S_FOCUS_MAX 1023

enum sem1217s_supply {
	SEM1217S_VIO,
	SEM1217S_VAF,
	SEM1217S_VDD,
};

struct sem1217s {
	struct v4l2_subdev sd;
	struct v4l2_ctrl_handler ctrls;
	struct v4l2_ctrl *focus;
	struct regulator_bulk_data supplies[3];
};

static struct sem1217s *to_sem1217s(struct v4l2_subdev *sd)
{
	return container_of(sd, struct sem1217s, sd);
}

static int sem1217s_write(struct sem1217s *vcm, u16 reg, u16 value, unsigned int size)
{
	struct i2c_client *client = v4l2_get_subdevdata(&vcm->sd);
	u8 buf[] = { reg >> 8, reg & 0xff, value & 0xff, value >> 8 };
	int ret;

	if (size == 1)
		buf[2] = value;
	ret = i2c_master_send(client, buf, size + 2);
	if (ret < 0)
		dev_err(&client->dev, "Register 0x%04x write failed: %d\n", reg, ret);
	return ret < 0 ? ret : ret == size + 2 ? 0 : -EIO;
}

static int sem1217s_set_position(struct sem1217s *vcm, unsigned int position)
{
	return sem1217s_write(vcm, SEM1217S_AF_POSITION, position << 4, 2);
}

static int sem1217s_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct sem1217s *vcm = container_of(ctrl->handler, struct sem1217s, ctrls);
	int ret;

	if (ctrl->id != V4L2_CID_FOCUS_ABSOLUTE)
		return -EINVAL;
	if (!pm_runtime_get_if_in_use(vcm->sd.dev))
		return 0;
	ret = sem1217s_set_position(vcm, ctrl->val);
	pm_runtime_put(vcm->sd.dev);
	return ret;
}

static const struct v4l2_ctrl_ops sem1217s_ctrl_ops = {
	.s_ctrl = sem1217s_set_ctrl,
};

static int sem1217s_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	return pm_runtime_resume_and_get(sd->dev);
}

static int sem1217s_close(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	pm_runtime_put(sd->dev);
	return 0;
}

static const struct v4l2_subdev_internal_ops sem1217s_internal_ops = {
	.open = sem1217s_open,
	.close = sem1217s_close,
};

static const struct v4l2_subdev_ops sem1217s_ops = {};

static int sem1217s_resume(struct device *dev)
{
	struct sem1217s *vcm = to_sem1217s(dev_get_drvdata(dev));
	int ret;

	ret = regulator_enable(vcm->supplies[SEM1217S_VIO].consumer);
	if (ret)
		return ret;
	usleep_range(1000, 1500);
	ret = regulator_enable(vcm->supplies[SEM1217S_VDD].consumer);
	if (ret)
		goto err_vio;
	usleep_range(8000, 8500);
	ret = regulator_enable(vcm->supplies[SEM1217S_VAF].consumer);
	if (ret)
		goto err_vdd;
	usleep_range(8000, 8500);

	/* The controller boots its application firmware after a power cycle. */
	msleep(100);
	ret = sem1217s_write(vcm, SEM1217S_AF_CONTROL, 1, 1);
	if (ret)
		goto err_power;
	/* Allow the closed-loop AF initialization to complete. */
	msleep(100);
	ret = sem1217s_set_position(vcm, vcm->focus->val);
	if (!ret)
		return 0;
	sem1217s_write(vcm, SEM1217S_AF_CONTROL, 0, 1);
err_power:
	regulator_disable(vcm->supplies[SEM1217S_VAF].consumer);
err_vdd:
	regulator_disable(vcm->supplies[SEM1217S_VDD].consumer);
err_vio:
	regulator_disable(vcm->supplies[SEM1217S_VIO].consumer);
	return ret;
}

static int sem1217s_suspend(struct device *dev)
{
	struct sem1217s *vcm = to_sem1217s(dev_get_drvdata(dev));
	int ret;

	ret = sem1217s_write(vcm, SEM1217S_AF_CONTROL, 0, 1);
	if (ret)
		return ret;
	usleep_range(10000, 11000);
	return regulator_bulk_disable(ARRAY_SIZE(vcm->supplies), vcm->supplies);
}

static int sem1217s_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct sem1217s *vcm;
	int ret;

	vcm = devm_kzalloc(dev, sizeof(*vcm), GFP_KERNEL);
	if (!vcm)
		return -ENOMEM;
	/* Bulk disable turns the controller supply off before the AF supply. */
	vcm->supplies[SEM1217S_VIO].supply = "vio";
	vcm->supplies[SEM1217S_VAF].supply = "vaf";
	vcm->supplies[SEM1217S_VDD].supply = "vdd";
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(vcm->supplies), vcm->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get supplies\n");

	v4l2_i2c_subdev_init(&vcm->sd, client, &sem1217s_ops);
	vcm->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	vcm->sd.internal_ops = &sem1217s_internal_ops;
	vcm->sd.entity.function = MEDIA_ENT_F_LENS;
	v4l2_ctrl_handler_init(&vcm->ctrls, 1);
	vcm->focus = v4l2_ctrl_new_std(&vcm->ctrls, &sem1217s_ctrl_ops,
				       V4L2_CID_FOCUS_ABSOLUTE, 0,
				       SEM1217S_FOCUS_MAX, 1, 512);
	ret = vcm->ctrls.error;
	if (ret)
		goto err_ctrls;
	vcm->sd.ctrl_handler = &vcm->ctrls;
	ret = media_entity_pads_init(&vcm->sd.entity, 0, NULL);
	if (ret)
		goto err_ctrls;

	pm_runtime_enable(dev);
	ret = v4l2_async_register_subdev(&vcm->sd);
	if (!ret)
		return 0;
	pm_runtime_disable(dev);
	media_entity_cleanup(&vcm->sd.entity);
err_ctrls:
	v4l2_ctrl_handler_free(&vcm->ctrls);
	return ret;
}

static void sem1217s_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct sem1217s *vcm = to_sem1217s(sd);

	v4l2_async_unregister_subdev(sd);
	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		sem1217s_suspend(&client->dev);
	pm_runtime_set_suspended(&client->dev);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&vcm->ctrls);
}

static DEFINE_RUNTIME_DEV_PM_OPS(sem1217s_pm_ops, sem1217s_suspend,
				sem1217s_resume, NULL);

static const struct of_device_id sem1217s_of_match[] = {
	{ .compatible = "onsemi,sem1217s" },
	{}
};
MODULE_DEVICE_TABLE(of, sem1217s_of_match);

static struct i2c_driver sem1217s_driver = {
	.driver = {
		.name = "sem1217s",
		.of_match_table = sem1217s_of_match,
		.pm = pm_ptr(&sem1217s_pm_ops),
	},
	.probe = sem1217s_probe,
	.remove = sem1217s_remove,
};
module_i2c_driver(sem1217s_driver);
MODULE_DESCRIPTION("SEM1217S lens focus controller");
MODULE_LICENSE("GPL");
