/* ST Microelectronics LSM6DSV16X_D1 6-axis IMU sensor driver
 *
 * Copyright (c) 2023 STMicroelectronics
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Datasheet:
 * https://www.st.com/resource/en/datasheet/lsm6dsv16x.pdf
 */

#define DT_DRV_COMPAT st_lsm6dsv16x_d1

#include <zephyr/kernel.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

#include "lsm6dsv16x.h"

LOG_MODULE_DECLARE(LSM6DSV16X_D1, CONFIG_SENSOR_LOG_LEVEL);

#define RETURN_ON_FAIL(ret)	if (ret) { return ret; }
#define CHECK_OK(expr)		do { if ((expr)) { LOG_ERR( #expr); return -EIO; } } while(0)

#if defined(CONFIG_LSM6DSV16X_D1_ENABLE_TEMP)
/**
 * lsm6dsv16x_enable_t_int - TEMP enable selected int pin to generate interrupt
 */
static int lsm6dsv16x_enable_t_int(const struct device *dev, int enable)
{
	const struct lsm6dsv16x_config *cfg = dev->config;
	stmdev_ctx_t *ctx = (stmdev_ctx_t *)&cfg->ctx;
	lsm6dsv16x_pin_int_route_t val;
	int ret;

	if (enable) {
		int16_t buf;

		/* dummy read: re-trigger interrupt */
		lsm6dsv16x_temperature_raw_get(ctx, &buf);
	}

	/* set interrupt (TEMP DRDY interrupt is only on INT2) */
	if (cfg->drdy_pin == 1) {
		return -EIO;
	}

	ret = lsm6dsv16x_pin_int2_route_get(ctx, &val);
	if (ret < 0) {
		LOG_ERR("pint_int2_route_get error");
		return ret;
	}

	val.drdy_temp = 1;

	return lsm6dsv16x_pin_int2_route_set(ctx, &val);
}
#endif

/**
 * lsm6dsv16x_enable_xl_int - XL enable selected int pin to generate interrupt
 */
static int lsm6dsv16x_enable_xl_int(const struct device *dev, int enable)
{
	const struct lsm6dsv16x_config *cfg = dev->config;
	stmdev_ctx_t *ctx = (stmdev_ctx_t *)&cfg->ctx;
	int ret;

	if (enable) {
		int16_t buf[3];

		/* dummy read: re-trigger interrupt */
		lsm6dsv16x_acceleration_raw_get(ctx, buf);
	}

	/* set interrupt */
	if (cfg->drdy_pin == 1) {
		lsm6dsv16x_pin_int_route_t val;

		ret = lsm6dsv16x_pin_int1_route_get(ctx, &val);
		if (ret < 0) {
			LOG_ERR("pint_int1_route_get error");
			return ret;
		}

		val.drdy_xl = 1;

		ret = lsm6dsv16x_pin_int1_route_set(ctx, &val);
	} else {
		lsm6dsv16x_pin_int_route_t val;

		ret = lsm6dsv16x_pin_int2_route_get(ctx, &val);
		if (ret < 0) {
			LOG_ERR("pint_int2_route_get error");
			return ret;
		}

		val.drdy_xl = 1;

		ret = lsm6dsv16x_pin_int2_route_set(ctx, &val);
	}

	return ret;
}

/**
 * lsm6dsv16x_enable_g_int - Gyro enable selected int pin to generate interrupt
 */
static int lsm6dsv16x_enable_g_int(const struct device *dev, int enable)
{
	const struct lsm6dsv16x_config *cfg = dev->config;
	stmdev_ctx_t *ctx = (stmdev_ctx_t *)&cfg->ctx;
	int ret;

	if (enable) {
		int16_t buf[3];

		/* dummy read: re-trigger interrupt */
		lsm6dsv16x_angular_rate_raw_get(ctx, buf);
	}

	/* set interrupt */
	if (cfg->drdy_pin == 1) {
		lsm6dsv16x_pin_int_route_t val;

		ret = lsm6dsv16x_pin_int1_route_get(ctx, &val);
		if (ret < 0) {
			LOG_ERR("pint_int1_route_get error");
			return ret;
		}

		val.drdy_g = 1;

		ret = lsm6dsv16x_pin_int1_route_set(ctx, &val);
	} else {
		lsm6dsv16x_pin_int_route_t val;

		ret = lsm6dsv16x_pin_int2_route_get(ctx, &val);
		if (ret < 0) {
			LOG_ERR("pint_int2_route_get error");
			return ret;
		}

		val.drdy_g = 1;

		ret = lsm6dsv16x_pin_int2_route_set(ctx, &val);
	}

	return ret;
}

int lsm6dsv16x_cfg_access(const struct device *dev, uint8_t access)
{
	int ret = 0;
	const struct lsm6dsv16x_config *cfg = dev->config;
	stmdev_ctx_t *ctx = (stmdev_ctx_t *)&cfg->ctx;
	lsm6dsv16x_func_cfg_access_t func_cfg_access;

	ret = lsm6dsv16x_read_reg(ctx, LSM6DSV16X_FUNC_CFG_ACCESS, (uint8_t *)&func_cfg_access, 1);
	RETURN_ON_FAIL(ret);
    	func_cfg_access.emb_func_reg_access = access & 1;
	ret = lsm6dsv16x_write_reg(ctx, LSM6DSV16X_FUNC_CFG_ACCESS, (uint8_t *)&func_cfg_access, 1);
	return ret;
}

#ifdef CONFIG_LSM6DSV16X_D1_ENABLE_MOTION
/**
 * lsm6dsv16x_enable_motion_int - Embedded function enable selected int pin to generate interrupt
 */
static int lsm6dsv16x_enable_motion_int(const struct device *dev, int enable)
{
	const struct lsm6dsv16x_config *cfg = dev->config;
	stmdev_ctx_t *ctx = (stmdev_ctx_t *)&cfg->ctx;
	int ret;

	if (enable) {
		int16_t buf[3];

		/* dummy read: re-trigger accel interrupt -- needed??? */
		lsm6dsv16x_acceleration_raw_get(ctx, buf);
	}
	lsm6dsv16x_pin_int_route_t val;

	val.sleep_change = enable;
	/* set interrupt */
	if (cfg->drdy_pin == 1) {

		ret = lsm6dsv16x_pin_int1_route_get(ctx, &val);
		if (ret < 0) {
			LOG_ERR("pint_int1_route_get error");
			return ret;
		}

		val.sleep_change = enable;

		CHECK_OK(lsm6dsv16x_pin_int1_route_set(ctx, &val));
	} else {
		lsm6dsv16x_pin_int_route_t val;

		ret = lsm6dsv16x_pin_int2_route_get(ctx, &val);
		if (ret < 0) {
			LOG_ERR("pint_int2_route_get error");
			return ret;
		}

		val.sleep_change = enable;
		CHECK_OK(lsm6dsv16x_pin_int2_route_set(ctx, &val));
	}

	return ret;
}

static int lsm6dsv16x_set_motion(const struct device *dev, sensor_trigger_handler_t handler)
{
	int ret = 0;
	const struct lsm6dsv16x_config *cfg = dev->config;
	struct lsm6dsv16x_data *lsm6dsv16x = dev->data;
	stmdev_ctx_t *ctx = (stmdev_ctx_t *)&cfg->ctx;

	if (handler) {
		/* use the activity / inactivity / wakeup functionality */
		lsm6dsv16x_act_mode_t mode = LSM6DSV16X_XL_LOW_POWER_GY_POWER_DOWN;
		lsm6dsv16x_interrupt_mode_t intmode;
		lsm6dsv16x_act_thresholds_t thresholds;

		intmode.enable = 1;
		intmode.lir = 1;
		CHECK_OK(lsm6dsv16x_interrupt_enable_set(ctx, intmode));
		CHECK_OK(lsm6dsv16x_act_mode_set(ctx, mode));
		/* might want to associate some of these threshold values with attributes */
		thresholds.inactivity_cfg.inact_dur = 2; /* wakeup after 3 transitions */
		thresholds.inactivity_cfg.xl_inact_odr = 0;
		thresholds.inactivity_cfg.wu_inact_ths_w = lsm6dsv16x->sleep_thresh;
		thresholds.inactivity_cfg.sleep_status_on_int = 0;
		thresholds.inactivity_ths = 1;
		thresholds.threshold = 0;
		thresholds.duration = 1; /* 1 * (512 / ODR_XL) */
		CHECK_OK(lsm6dsv16x_act_thresholds_set(ctx, &thresholds));

		CHECK_OK(lsm6dsv16x_enable_motion_int(dev, handler ? 1 : 0));
		CHECK_OK(lsm6dsv16x_gyro_range_set(dev, 500));
		CHECK_OK(lsm6dsv16x_accel_range_set(dev, 4));
		CHECK_OK(lsm6dsv16x_accel_odr_set(dev, 15));
		CHECK_OK(lsm6dsv16x_gyro_odr_set(dev, 15));

		lsm6dsv16x_act_wkup_time_windows_t val;
		val.shock = 0;
		val.quiet = lsm6dsv16x->sleep_dur;
		lsm6dsv16x_act_wkup_time_windows_set(ctx, val);
	} else {
		lsm6dsv16x_act_mode_t mode = LSM6DSV16X_XL_AND_GY_NOT_AFFECTED;
		CHECK_OK(lsm6dsv16x_act_mode_set(ctx, mode));
	}
	return ret;

};
#endif

/**
 * lsm6dsv16x_trigger_set - link external trigger to event data ready
 */
int lsm6dsv16x_trigger_set(const struct device *dev,
			  const struct sensor_trigger *trig,
			  sensor_trigger_handler_t handler)
{
	const struct lsm6dsv16x_config *cfg = dev->config;
	struct lsm6dsv16x_data *lsm6dsv16x = dev->data;

	if (!cfg->trig_enabled) {
		LOG_ERR("trigger_set op not supported");
		return -ENOTSUP;
	}

	if (trig->type == SENSOR_TRIG_DATA_READY) {
		if (trig->chan == SENSOR_CHAN_ACCEL_XYZ) {
			lsm6dsv16x->handler_drdy_acc = handler;
			lsm6dsv16x->trig_drdy_acc = trig;
			if (handler) {
				return lsm6dsv16x_enable_xl_int(dev, LSM6DSV16X_D1_EN_BIT);
			} else {
				return lsm6dsv16x_enable_xl_int(dev, LSM6DSV16X_D1_DIS_BIT);
			}
		} else if (trig->chan == SENSOR_CHAN_GYRO_XYZ) {
			lsm6dsv16x->handler_drdy_gyr = handler;
			lsm6dsv16x->trig_drdy_gyr = trig;
			if (handler) {
				return lsm6dsv16x_enable_g_int(dev, LSM6DSV16X_D1_EN_BIT);
			} else {
				return lsm6dsv16x_enable_g_int(dev, LSM6DSV16X_D1_DIS_BIT);
			}
		}
#if defined(CONFIG_LSM6DSV16X_D1_ENABLE_TEMP)
		else if (trig->chan == SENSOR_CHAN_DIE_TEMP) {
			lsm6dsv16x->handler_drdy_temp = handler;
			lsm6dsv16x->trig_drdy_temp = trig;
			if (handler) {
				return lsm6dsv16x_enable_t_int(dev, LSM6DSV16X_D1_EN_BIT);
			} else {
				return lsm6dsv16x_enable_t_int(dev, LSM6DSV16X_D1_DIS_BIT);
			}
		}
#endif
	}
#if defined(CONFIG_LSM6DSV16X_D1_ENABLE_MOTION)
	else if (trig->type == SENSOR_TRIG_MOTION) {
		lsm6dsv16x->handler_motion = handler;
		lsm6dsv16x->trig_motion = trig;
		return lsm6dsv16x_set_motion(dev, handler);
	}
#endif

	return -ENOTSUP;
}

/**
 * lsm6dsv16x_handle_interrupt - handle the drdy or embedded function event
 * read data and call handler if registered any
 */
static void lsm6dsv16x_handle_interrupt(const struct device *dev)
{
	struct lsm6dsv16x_data *lsm6dsv16x = dev->data;
	const struct lsm6dsv16x_config *cfg = dev->config;
	stmdev_ctx_t *ctx = (stmdev_ctx_t *)&cfg->ctx;
	/* to simplify debugging, make a union with the value and the individual bits */
	union {
		uint8_t val;
		lsm6dsv16x_data_ready_t bits;
	} status;

	while (1) {
		if (lsm6dsv16x_flag_data_ready_get(ctx, &status.bits) < 0) {
			LOG_DBG("failed reading status reg");
			return;
		}

#if defined(CONFIG_LSM6DSV16X_D1_ENABLE_MOTION)
		bool is_motion_interrupt;
		union {
			uint8_t val;
			lsm6dsv16x_all_int_src_t bits;
		} all_status;
		lsm6dsv16x_read_reg(ctx, LSM6DSV16X_ALL_INT_SRC, (uint8_t *)&all_status, 1);
		is_motion_interrupt = all_status.bits.sleep_change_ia;
#endif

		if ((status.bits.drdy_xl == 0) && (status.bits.drdy_gy == 0)
#if defined(CONFIG_LSM6DSV16X_D1_ENABLE_TEMP)
					&& (status.drdy_temp == 0)
#endif
#if defined(CONFIG_LSM6DSV16X_D1_ENABLE_MOTION)
					&& !is_motion_interrupt
#endif
					) {
			break;
		}

#if defined(CONFIG_LSM6DSV16X_D1_ENABLE_TEMP)
		if ((status.drdy_temp) && (lsm6dsv16x->handler_drdy_temp != NULL)) {
			lsm6dsv16x->handler_drdy_temp(dev, lsm6dsv16x->trig_drdy_temp);
		}
#endif
#if defined(CONFIG_LSM6DSV16X_D1_ENABLE_MOTION)
		if ((is_motion_interrupt) && (lsm6dsv16x->handler_motion != NULL)) {
			uint8_t wakeup_src;
			lsm6dsv16x_read_reg(ctx, LSM6DSV16X_WAKE_UP_SRC, &wakeup_src, 1);
			lsm6dsv16x->sleep_state = wakeup_src & 0x10; // should really use the structure here

			lsm6dsv16x->handler_motion(dev, lsm6dsv16x->trig_motion);
		}
#endif

		if ((status.bits.drdy_xl) && (lsm6dsv16x->handler_drdy_acc != NULL)) {
			lsm6dsv16x->handler_drdy_acc(dev, lsm6dsv16x->trig_drdy_acc);
		}

		if ((status.bits.drdy_gy) && (lsm6dsv16x->handler_drdy_gyr != NULL)) {
			lsm6dsv16x->handler_drdy_gyr(dev, lsm6dsv16x->trig_drdy_gyr);
		}

	}

	gpio_pin_interrupt_configure_dt(lsm6dsv16x->drdy_gpio,
					GPIO_INT_EDGE_TO_ACTIVE);
}

static void lsm6dsv16x_gpio_callback(const struct device *dev,
				    struct gpio_callback *cb, uint32_t pins)
{
	struct lsm6dsv16x_data *lsm6dsv16x =
		CONTAINER_OF(cb, struct lsm6dsv16x_data, gpio_cb);

	ARG_UNUSED(pins);

	gpio_pin_interrupt_configure_dt(lsm6dsv16x->drdy_gpio, GPIO_INT_DISABLE);

#if defined(CONFIG_LSM6DSV16X_D1_TRIGGER_OWN_THREAD)
	k_sem_give(&lsm6dsv16x->gpio_sem);
#elif defined(CONFIG_LSM6DSV16X_D1_TRIGGER_GLOBAL_THREAD)
	k_work_submit(&lsm6dsv16x->work);
#endif /* CONFIG_LSM6DSV16X_D1_TRIGGER_OWN_THREAD */
}

#ifdef CONFIG_LSM6DSV16X_D1_TRIGGER_OWN_THREAD
static void lsm6dsv16x_thread(struct lsm6dsv16x_data *lsm6dsv16x)
{
	while (1) {
		k_sem_take(&lsm6dsv16x->gpio_sem, K_FOREVER);
		lsm6dsv16x_handle_interrupt(lsm6dsv16x->dev);
	}
}
#endif /* CONFIG_LSM6DSV16X_D1_TRIGGER_OWN_THREAD */

#ifdef CONFIG_LSM6DSV16X_D1_TRIGGER_GLOBAL_THREAD
static void lsm6dsv16x_work_cb(struct k_work *work)
{
	struct lsm6dsv16x_data *lsm6dsv16x =
		CONTAINER_OF(work, struct lsm6dsv16x_data, work);

	lsm6dsv16x_handle_interrupt(lsm6dsv16x->dev);
}
#endif /* CONFIG_LSM6DSV16X_D1_TRIGGER_GLOBAL_THREAD */

int lsm6dsv16x_init_interrupt(const struct device *dev)
{
	struct lsm6dsv16x_data *lsm6dsv16x = dev->data;
	const struct lsm6dsv16x_config *cfg = dev->config;
	stmdev_ctx_t *ctx = (stmdev_ctx_t *)&cfg->ctx;
	int ret;

	lsm6dsv16x->drdy_gpio = (cfg->drdy_pin == 1) ?
			(struct gpio_dt_spec *)&cfg->int1_gpio :
			(struct gpio_dt_spec *)&cfg->int2_gpio;

	/* setup data ready gpio interrupt (INT1 or INT2) */
	if (!gpio_is_ready_dt(lsm6dsv16x->drdy_gpio)) {
		LOG_ERR("Cannot get pointer to drdy_gpio device");
		return -EINVAL;
	}

#if defined(CONFIG_LSM6DSV16X_D1_TRIGGER_OWN_THREAD)
	k_sem_init(&lsm6dsv16x->gpio_sem, 0, K_SEM_MAX_LIMIT);

	k_thread_create(&lsm6dsv16x->thread, lsm6dsv16x->thread_stack,
			CONFIG_LSM6DSV16X_D1_THREAD_STACK_SIZE,
			(k_thread_entry_t)lsm6dsv16x_thread, lsm6dsv16x,
			NULL, NULL, K_PRIO_COOP(CONFIG_LSM6DSV16X_D1_THREAD_PRIORITY),
			0, K_NO_WAIT);
	k_thread_name_set(&lsm6dsv16x->thread, "lsm6dsv16x");
#elif defined(CONFIG_LSM6DSV16X_D1_TRIGGER_GLOBAL_THREAD)
	lsm6dsv16x->work.handler = lsm6dsv16x_work_cb;
#endif /* CONFIG_LSM6DSV16X_D1_TRIGGER_OWN_THREAD */

	ret = gpio_pin_configure_dt(lsm6dsv16x->drdy_gpio, GPIO_INPUT);
	if (ret < 0) {
		LOG_DBG("Could not configure gpio");
		return ret;
	}

	gpio_init_callback(&lsm6dsv16x->gpio_cb,
			   lsm6dsv16x_gpio_callback,
			   BIT(lsm6dsv16x->drdy_gpio->pin));

	if (gpio_add_callback(lsm6dsv16x->drdy_gpio->port, &lsm6dsv16x->gpio_cb) < 0) {
		LOG_DBG("Could not set gpio callback");
		return -EIO;
	}


	/* set data ready mode on int1/int2 */
	LOG_DBG("drdy_pulsed is %d", (int)cfg->drdy_pulsed);
	lsm6dsv16x_data_ready_mode_t mode = cfg->drdy_pulsed ? LSM6DSV16X_DRDY_PULSED :
							     LSM6DSV16X_DRDY_LATCHED;

	ret = lsm6dsv16x_data_ready_mode_set(ctx, mode);
	if (ret < 0) {
		LOG_ERR("drdy_pulsed config error %d", (int)cfg->drdy_pulsed);
		return ret;
	}

	return gpio_pin_interrupt_configure_dt(lsm6dsv16x->drdy_gpio,
					       GPIO_INT_EDGE_TO_ACTIVE);
}
