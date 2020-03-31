/* Bosch BMI160 inertial measurement unit driver, trigger implementation
 *
 * Copyright (c) 2016 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <kernel.h>
#include <drivers/sensor.h>
#include <drivers/gpio.h>

#include "bm160.h"

#include <logging/log.h>
LOG_MODULE_DECLARE(BM160, CONFIG_SENSOR_LOG_LEVEL);

static uint16_t tap_duration[] = {50, 100, 150, 200, 250, 375, 500, 700};

static void bmi160_handle_anymotion(const struct device *dev)
{
	struct bmi160_device_data *bmi160 = dev->data;
	struct sensor_trigger anym_trigger = {
		.type = SENSOR_TRIG_DELTA,
		.chan = SENSOR_CHAN_ACCEL_XYZ,
	};

	if (bmi160->handler_anymotion) {
		bmi160->handler_anymotion(dev, &anym_trigger);
	}
}

static void bmi160_handle_tap(const struct device *dev, bool is_single)
{
	struct bmi160_device_data *bmi160 = dev->data;
	struct sensor_trigger trigger;

	trigger.type = is_single ? SENSOR_TRIG_TAP : SENSOR_TRIG_DOUBLE_TAP;

	if (is_single && bmi160->handler_tap) {
		bmi160->handler_tap(dev, &trigger);
	}

	if (!is_single && bmi160->handler_dtap) {
		bmi160->handler_dtap(dev, &trigger);
	}
}

static void bmi160_handle_drdy(const struct device *dev, uint8_t status)
{
	struct bmi160_device_data *bmi160 = dev->data;
	struct sensor_trigger drdy_trigger = {
		.type = SENSOR_TRIG_DATA_READY,
	};

#if !defined(CONFIG_BMI160_ACCEL_PMU_SUSPEND)
	if (bmi160->handler_drdy_acc && (status & BMI160_STATUS_ACC_DRDY)) {
		drdy_trigger.chan = SENSOR_CHAN_ACCEL_XYZ;
		bmi160->handler_drdy_acc(dev, &drdy_trigger);
	}
#endif

#if !defined(CONFIG_BMI160_GYRO_PMU_SUSPEND)
	if (bmi160->handler_drdy_gyr && (status & BMI160_STATUS_GYR_DRDY)) {
		drdy_trigger.chan = SENSOR_CHAN_GYRO_XYZ;
		bmi160->handler_drdy_gyr(dev, &drdy_trigger);
	}
#endif

#if defined(CONFIG_BMX160_MAG)
	if (bmi160->handler_drdy_mag && (status & BMI160_STATUS_MAG_DRDY)) {
		drdy_trigger.chan = SENSOR_CHAN_MAGN_XYZ;
		bmi160->handler_drdy_mag(dev, &drdy_trigger);
	}
#endif
}

static void bmi160_handle_interrupts(const struct device *dev)
{
	union {
		uint8_t raw[6];
		struct {
/* dummy byte in beginning!! not working for I2C, kept to not break exising */
#if !defined(CONFIG_BMX160)
			uint8_t dummy; /* spi related dummy byte */
#endif
			uint8_t status;
			uint8_t int_status[4];
		};
	} buf;

	if (bmi160_read(dev, BMI160_REG_STATUS, buf.raw, sizeof(buf)) < 0) {
		return;
	}

	if ((buf.int_status[0] & BMI160_INT_STATUS0_ANYM) &&
	    (buf.int_status[2] & (BMI160_INT_STATUS2_ANYM_FIRST_X |
				  BMI160_INT_STATUS2_ANYM_FIRST_Y |
				  BMI160_INT_STATUS2_ANYM_FIRST_Z))) {
		bmi160_handle_anymotion(dev);
	}

	if ((buf.int_status[0] & BMI160_INT_STATUS0_S_TAP) &&
	    (buf.int_status[2] & (BMI160_INT_STATUS2_TAP_FIRST_X |
				  BMI160_INT_STATUS2_TAP_FIRST_Y |
				  BMI160_INT_STATUS2_TAP_FIRST_Z))) {
		bmi160_handle_tap(dev, true);
	}

	if ((buf.int_status[0] & BMI160_INT_STATUS0_D_TAP) &&
	    (buf.int_status[2] & (BMI160_INT_STATUS2_TAP_FIRST_X |
				  BMI160_INT_STATUS2_TAP_FIRST_Y |
				  BMI160_INT_STATUS2_TAP_FIRST_Z))) {
		bmi160_handle_tap(dev, false);
	}

	if (buf.int_status[1] & BMI160_INT_STATUS1_DRDY) {
		bmi160_handle_drdy(dev, buf.status);
	}
}

#ifdef CONFIG_BMI160_TRIGGER_OWN_THREAD
static K_KERNEL_STACK_DEFINE(bmi160_thread_stack, CONFIG_BMI160_THREAD_STACK_SIZE);
static struct k_thread bmi160_thread;

static void bmi160_thread_main(struct bmi160_device_data *bmi160)
{
	while (1) {
		k_sem_take(&bmi160->sem, K_FOREVER);
		bmi160_handle_interrupts(bmi160->dev);
	}
}
#endif

#ifdef CONFIG_BMI160_TRIGGER_GLOBAL_THREAD
static void bmi160_work_handler(struct k_work *work)
{
	struct bmi160_device_data *bmi160 =
		CONTAINER_OF(work, struct bmi160_device_data, work);

	bmi160_handle_interrupts(bmi160->dev);
}
#endif

extern struct bmi160_device_data bmi160_data;

static void bmi160_gpio_callback(const struct device *port,
				 struct gpio_callback *cb, uint32_t pin)
{
	struct bmi160_device_data *bmi160 =
		CONTAINER_OF(cb, struct bmi160_device_data, gpio_cb);

	ARG_UNUSED(port);
	ARG_UNUSED(pin);

#if defined(CONFIG_BMI160_TRIGGER_OWN_THREAD)
	k_sem_give(&bmi160->sem);
#elif defined(CONFIG_BMI160_TRIGGER_GLOBAL_THREAD)
	k_work_submit(&bmi160->work);
#endif
}

static int bmi160_trigger_drdy_set(const struct device *dev,
				   enum sensor_channel chan,
				   sensor_trigger_handler_t handler)
{
	struct bmi160_device_data *bmi160 = dev->data;
	uint8_t drdy_en = 0U;

#if !defined(CONFIG_BMI160_ACCEL_PMU_SUSPEND)
	if (chan == SENSOR_CHAN_ACCEL_XYZ) {
		bmi160->handler_drdy_acc = handler;
	}

	if (bmi160->handler_drdy_acc) {
		drdy_en = BMI160_INT_DRDY_EN;
	}
#endif

#if !defined(CONFIG_BMI160_GYRO_PMU_SUSPEND)
	if (chan == SENSOR_CHAN_GYRO_XYZ) {
		bmi160->handler_drdy_gyr = handler;
	}

	if (bmi160->handler_drdy_gyr) {
		drdy_en = BMI160_INT_DRDY_EN;
	}
#endif

#if defined(CONFIG_BMX160_MAG)
	if (chan == SENSOR_CHAN_MAGN_XYZ) {
		bmi160->handler_drdy_mag = handler;
	}

	if (bmi160->handler_drdy_mag) {
		drdy_en = BMI160_INT_DRDY_EN;
	}
#endif

	if (bmi160_reg_update(dev, BMI160_REG_INT_EN1,
			      BMI160_INT_DRDY_EN, drdy_en) < 0) {
		return -EIO;
	}

	return 0;
}

#if !defined(CONFIG_BMI160_ACCEL_PMU_SUSPEND)
static int bmi160_trigger_anym_set(const struct device *dev,
				   sensor_trigger_handler_t handler)
{
	struct bmi160_device_data *bmi160 = dev->data;
	int ret;
	uint8_t anym_en = 0U;

	bmi160->handler_anymotion = handler;

	if (handler) {
		anym_en = BMI160_INT_ANYM_X_EN |
			  BMI160_INT_ANYM_Y_EN |
			  BMI160_INT_ANYM_Z_EN;
	}

	ret = bmi160_reg_update(dev, BMI160_REG_INT_EN0,
				BMI160_INT_ANYM_MASK, anym_en);
	if (ret < 0) {
		return ret;
	}

	return 0;
}

static int bmi160_trigger_tap_set(const struct device *dev,
				  sensor_trigger_handler_t handler,
				  bool is_single)
{
	struct bmi160_device_data *bmi160 = dev->data;
	int ret;
	uint8_t rval = 0U;
	uint8_t mask;

	if (is_single) {
		bmi160->handler_tap = handler;
		mask = BMI160_INT_S_TAP_EN;
	} else {
		bmi160->handler_dtap = handler;
		mask = BMI160_INT_D_TAP_EN;
	}

	if (handler) {
		rval = is_single ? BMI160_INT_S_TAP_EN : BMI160_INT_D_TAP_EN;
	}

	ret = bmi160_reg_update(dev, BMI160_REG_INT_EN0, mask, rval);
	if (ret < 0) {
		return ret;
	}

	return 0;
}

static int bmi160_trigger_set_acc(const struct device *dev,
				  const struct sensor_trigger *trig,
				  sensor_trigger_handler_t handler)
{
	switch (trig->type) {
	case SENSOR_TRIG_DATA_READY:
		return bmi160_trigger_drdy_set(dev, trig->chan, handler);

	case SENSOR_TRIG_DELTA:
		return bmi160_trigger_anym_set(dev, handler);

	case SENSOR_TRIG_TAP:
		return bmi160_trigger_tap_set(dev, handler, true);

	case SENSOR_TRIG_DOUBLE_TAP:
		return bmi160_trigger_tap_set(dev, handler, false);

	default:
		return -ENOTSUP;
	}
}

int bmi160_acc_tap_config(const struct device *dev, enum sensor_attribute attr,
			  const struct sensor_value *val)
{
	uint8_t rval;
	int i, ret;

	if (attr == SENSOR_ATTR_LOWER_THRESH) {
		if (val->val1 == BMI160_INT_QUIET_20_MSEC) {
			rval = BMI160_INT_TAP_QUIET;
		} else if (val->val1 == BMI160_INT_QUIET_30_MSEC) {
			rval = 0;
		} else {
			return -EINVAL;
		}

		ret = bmi160_reg_update(dev, BMI160_REG_INT_TAP0,
					BMI160_INT_TAP_QUIET, rval);
		if (ret < 0) {
			return ret;
		}
	} else {
		for (i = 0; i < ARRAY_SIZE(tap_duration); i++) {
			if (tap_duration[i] == val->val1) {
				rval = i;
				break;
			}
		}

		if (i == ARRAY_SIZE(tap_duration)) {
			return -EINVAL;
		}

		ret = bmi160_reg_update(dev, BMI160_REG_INT_TAP0,
					BMI160_INT_TAP_DURATION, rval);
		if (ret < 0) {
			return ret;
		}
	}

	return 0;
}

int bmi160_acc_slope_config(const struct device *dev,
			    enum sensor_attribute attr,
			    const struct sensor_value *val)
{
	uint8_t acc_range_g, reg_val;
	uint32_t slope_th_ums2;
	int ret;

	if (attr == SENSOR_ATTR_SLOPE_TH) {
		ret = bmi160_byte_read(dev, BMI160_REG_ACC_RANGE, &reg_val);
		if (ret < 0) {
			return ret;
		}

		acc_range_g = bmi160_acc_reg_val_to_range(reg_val);

		slope_th_ums2 = val->val1 * 1000000 + val->val2;

		/* make sure the provided threshold does not exceed range / 2 */
		if (slope_th_ums2 > (acc_range_g / 2 * SENSOR_G)) {
			return -EINVAL;
		}

		reg_val = (slope_th_ums2 - 1) * 512U / (acc_range_g * SENSOR_G);

		ret = bmi160_byte_write(dev, BMI160_REG_INT_MOTION1, reg_val);
		if (ret < 0) {
			return ret;
		}
	} else { /* SENSOR_ATTR_SLOPE_DUR */
		/* slope duration is measured in number of samples */
		if (val->val1 < 1 || val->val1 > 4) {
			return -ENOTSUP;
		}

		ret = bmi160_reg_field_update(dev, BMI160_REG_INT_MOTION0,
					      BMI160_ANYM_DUR_POS,
					      BMI160_ANYM_DUR_MASK,
					      val->val1);
		if (ret < 0) {
			return ret;
		}
	}

	return 0;
}
#endif

#if !defined(CONFIG_BMI160_GYRO_PMU_SUSPEND)
static int bmi160_trigger_set_gyr(const struct device *dev,
				  const struct sensor_trigger *trig,
				  sensor_trigger_handler_t handler)
{
	if (trig->type == SENSOR_TRIG_DATA_READY) {
		return bmi160_trigger_drdy_set(dev, trig->chan, handler);
	}

	return -ENOTSUP;
}
#endif

#if defined(CONFIG_BMX160_MAG)
static int bmi160_trigger_set_mag(const struct device *dev,
				  const struct sensor_trigger *trig,
				  sensor_trigger_handler_t handler)
{
	if (trig->type == SENSOR_TRIG_DATA_READY) {
		return bmi160_trigger_drdy_set(dev, trig->chan, handler);
	}

	return -ENOTSUP;
}
#endif

int bmi160_trigger_set(const struct device *dev,
		       const struct sensor_trigger *trig,
		       sensor_trigger_handler_t handler)
{
#if !defined(CONFIG_BMI160_ACCEL_PMU_SUSPEND)
	if (trig->chan == SENSOR_CHAN_ACCEL_XYZ) {
		return bmi160_trigger_set_acc(dev, trig, handler);
	}
#endif
#if !defined(CONFIG_BMI160_GYRO_PMU_SUSPEND)
	if (trig->chan == SENSOR_CHAN_GYRO_XYZ) {
		return bmi160_trigger_set_gyr(dev, trig, handler);
	}
#endif
#if defined(CONFIG_BMX160_MAG)
	if (trig->chan == SENSOR_CHAN_MAGN_XYZ) {
		return bmi160_trigger_set_mag(dev, trig, handler);
	}
#endif

	return -ENOTSUP;
}

int bmi160_trigger_mode_init(const struct device *dev)
{
	struct bmi160_device_data *bmi160 = dev->data;

	const struct bmi160_device_config *cfg = dev->config;

	bmi160->gpio = device_get_binding((char *)cfg->gpio_port);
	if (!bmi160->gpio) {
		LOG_DBG("Gpio controller %s not found.", cfg->gpio_port);
		return -EINVAL;
	}

	bmi160->dev = dev;

#if defined(CONFIG_BMI160_TRIGGER_OWN_THREAD)
	k_sem_init(&bmi160->sem, 0, UINT_MAX);

	k_thread_create(&bmi160_thread, bmi160_thread_stack,
			CONFIG_BMI160_THREAD_STACK_SIZE,
			(k_thread_entry_t)bmi160_thread_main,
			bmi160, NULL, NULL,
			K_PRIO_COOP(CONFIG_BMI160_THREAD_PRIORITY),
			 0, K_NO_WAIT);
#elif defined(CONFIG_BMI160_TRIGGER_GLOBAL_THREAD)
	bmi160->work.handler = bmi160_work_handler;
#endif

	/* map all interrupts to INT1 pin */
	if (bmi160_word_write(dev, BMI160_REG_INT_MAP0, 0xf0ff) < 0) {
		LOG_DBG("Failed to map interrupts.");
		return -EIO;
	}

	gpio_pin_configure(bmi160->gpio, cfg->int_pin,
			   GPIO_INPUT | cfg->int_flags);

	gpio_init_callback(&bmi160->gpio_cb,
			   bmi160_gpio_callback,
			   BIT(cfg->int_pin));

	gpio_add_callback(bmi160->gpio, &bmi160->gpio_cb);
	gpio_pin_interrupt_configure(bmi160->gpio, cfg->int_pin,
				     GPIO_INT_EDGE_TO_ACTIVE);

	return bmi160_byte_write(dev, BMI160_REG_INT_OUT_CTRL,
				 BMI160_INT1_OUT_EN | BMI160_INT1_EDGE_CTRL |
				 BMI160_INT1_LVL_HIGH);
}