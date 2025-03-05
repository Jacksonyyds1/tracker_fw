#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <stdio.h>

LOG_MODULE_REGISTER(imu, LOG_LEVEL_DBG);

#include "imu.h"
#include "utils.h"

#define LSM6DSV DT_INST(0, st_lsm6dsv16x)
#define LSM6DSV_LABEL DT_PROP(LSM6DSV, label)
#define ZEPHYR_USER_NODE DT_PATH(zephyr_user)

#define ACCEL_ON (7)

static imu_output_cb_t callback; // callback for sampled data
const struct device *imu;
static int trig_cnt;
static bool verbose;

static int imu_sampling(void);

static void trigger_handler(const struct device *dev,
                            const struct sensor_trigger *trig)
{
    imu_sampling();
}

/* IMU Initialization */
int imu_init(void)
{
    int rc = -1;

    LOG_DBG("IMU Init ...");

    imu = device_get_binding(LSM6DSV_LABEL);
    if (!imu)
    {
        LOG_ERR("Error: Cannot bind device %s", LSM6DSV_LABEL);
        return rc;
    }
    else
    {
        LOG_DBG("Found device %s.  Good!!!", LSM6DSV_LABEL);
    }

    struct sensor_trigger trig;
    trig.type = SENSOR_TRIG_DATA_READY;
    trig.chan = SENSOR_CHAN_ACCEL_XYZ;
    sensor_trigger_set(imu, &trig, trigger_handler);

    return 0;
}

// TODO: are we using a FIFO?
static int imu_sampling(void)
{
    struct sensor_value x, y, z;
    struct sensor_value gx, gy, gz;
    static char string[100];

    /* LSM6DSV accel */
    sensor_sample_fetch_chan(imu, SENSOR_CHAN_ACCEL_XYZ);
    sensor_channel_get(imu, SENSOR_CHAN_ACCEL_X, &x);
    sensor_channel_get(imu, SENSOR_CHAN_ACCEL_Y, &y);
    sensor_channel_get(imu, SENSOR_CHAN_ACCEL_Z, &z);
    int count = sprintf(string, "%08d,%f,%f,%f,", k_uptime_get_32(), sensor_value_to_double(&x), sensor_value_to_double(&y), sensor_value_to_double(&z));

    /* LSM6DSV gyro */
    sensor_sample_fetch_chan(imu, SENSOR_CHAN_GYRO_XYZ);
    sensor_channel_get(imu, SENSOR_CHAN_GYRO_X, &gx);
    sensor_channel_get(imu, SENSOR_CHAN_GYRO_Y, &gy);
    sensor_channel_get(imu, SENSOR_CHAN_GYRO_Z, &gz);
    sprintf(&string[count], "%f,%f,%f", sensor_value_to_double(&gx), sensor_value_to_double(&gy), sensor_value_to_double(&gz));
    if (verbose)
    {
        LOG_DBG("%s", string);
    }

    imu_sample_t sample = {0};
    sample.timestamp = utils_get_currentmillis();
    sample.sample_count = trig_cnt;
    sample.ax = sensor_value_to_double(&x);
    sample.ay = sensor_value_to_double(&y);
    sample.az = sensor_value_to_double(&z);
    sample.gx = sensor_value_to_double(&gx);
    sample.gy = sensor_value_to_double(&gy);
    sample.gz = sensor_value_to_double(&gz);

    if (callback != NULL)
    {
        callback(sample);
    }

    trig_cnt++;
    return 0;
}

/* Enable IMU with sampling freq; 0 to disable */
int imu_enable(output_data_rate_t rate, imu_output_cb_t cb)
{
    int ret = 0;
    struct sensor_trigger trig;
    struct sensor_value odr_attr;

    switch (rate)
    {
    case IMU_ODR_0_HZ:
        // don't clear trig_cnt when stopping, clear callback
        callback = NULL;
        break;
    case IMU_ODR_15_HZ:
    case IMU_ODR_30_HZ:
    case IMU_ODR_60_HZ:
    case IMU_ODR_120_HZ:
    case IMU_ODR_240_HZ:
    case IMU_ODR_480_HZ:
    case IMU_ODR_960_HZ:
    case IMU_ODR_1920_HZ:
    case IMU_ODR_3840_HZ:
    case IMU_ODR_7680_HZ:
        trig_cnt = 0; // clear count when enabling with a non-zero rate
        break;
    default:
        LOG_ERR("invalid rate: %d", rate);
        return -1;
    }

    LOG_DBG("setting output data rate to %d", rate);

    trig.type = SENSOR_TRIG_DATA_READY;
    trig.chan = SENSOR_CHAN_ACCEL_XYZ;

    odr_attr.val1 = rate;
    odr_attr.val2 = 0; // fractional part

    ret = sensor_attr_set(imu, SENSOR_CHAN_ACCEL_XYZ,
                          SENSOR_ATTR_SAMPLING_FREQUENCY, &odr_attr);
    if (ret != 0)
    {
        LOG_ERR("Cannot set output data rate for accelerometer, ret=%d", ret);
        return ret;
    }

    ret = sensor_attr_set(imu, SENSOR_CHAN_GYRO_XYZ,
                          SENSOR_ATTR_SAMPLING_FREQUENCY, &odr_attr);

    // TODO: gryo returns error when setting a 0 rate
    if (rate != IMU_ODR_0_HZ && ret != 0)
    {
        LOG_ERR("Cannot set output data rate for gyro, ret=%d", ret);
        return ret;
    }

    callback = cb;

    return 0;
}

int imu_get_trigger_count(void)
{
    return trig_cnt;
}

int imu_set_verbose(bool enable)
{
    verbose = enable;
    return 0;
}

static int print_samples_cb(imu_sample_t data)
{
    LOG_INF("x=%.2f, y=%.2f, z=%.2f, gx=%.2f, gy=%.2f, gz=%.2f",
            data.ax, data.ay, data.az,
            data.gx, data.gy, data.gz);

    return 0;
}

static int imu_start_shell(const struct shell *sh, size_t argc, char **argv)
{
    shell_fprintf(sh, SHELL_NORMAL, "starting IMU...\r\n");
    return imu_enable(IMU_ODR_15_HZ, print_samples_cb);
}

static int imu_stop_shell(const struct shell *sh, size_t argc, char **argv)
{
    imu_enable(0, NULL);
    shell_fprintf(sh, SHELL_NORMAL, "stopped IMU\r\n");
    return 0;
}
static int imu_read_sample(const struct shell *sh, size_t argc, char **argv)
{
    imu_sampling();
    return 0;
}

// NOTE: this needs to be done at system init time, otherwise IMU
// initialization in the kernel fails
static int board_init(void)
{
	static const struct device *en_imu = DEVICE_DT_GET(DT_NODELABEL(gpio0));
	if (en_imu == NULL) {
		printk("Error: failed to find gpio0\n");
		return 0;
	} else {
		int ret = gpio_pin_configure(en_imu, ACCEL_ON, GPIO_OUTPUT_ACTIVE);
		if (ret != 0) {
			printk("Error: failed to configure ACCEL_ON\n");
		} else {
			printk("powered on the IMU\n");
		}
	}

	return 0;
}

SYS_INIT(board_init, PRE_KERNEL_1, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

SHELL_CMD_REGISTER(imu_start, NULL, "start IMU at 15Hz, print to console", imu_start_shell);
SHELL_CMD_REGISTER(imu_stop, NULL, "stop IMU", imu_stop_shell);
SHELL_CMD_REGISTER(imu_read, NULL, "stop IMU", imu_read_sample);
