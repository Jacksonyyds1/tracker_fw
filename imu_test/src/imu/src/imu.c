#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <stdio.h>

LOG_MODULE_REGISTER(imu, LOG_LEVEL_DBG);

#include "imu.h"

#define LSM6DSV DT_INST(0, st_lsm6dsv16x)
#define LSM6DSV_LABEL DT_PROP(LSM6DSV, label)

typedef enum
{ // IMU operation mode
    IMU_POLL_MODE = 0,
    IMU_INTERRUPT_MODE
} imu_operation_mode_t;

static imu_output_cb_t callback; // callback for sampled data
const struct device *imu;
static int trig_cnt;
static bool verbose;

static imu_operation_mode_t imu_operation_mode = IMU_INTERRUPT_MODE;
static int imu_sampling(void);

static inline float out_ev(struct sensor_value *val)
{
    return (val->val1 + (float)val->val2 / 1000000);
}

static void trigger_handler(const struct device *dev,
                            const struct sensor_trigger *trig)
{
    if (imu_operation_mode == IMU_INTERRUPT_MODE)
    {
        imu_sampling();
    }
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

    return 0;
}

static int imu_sampling(void)
{
    struct sensor_value x, y, z;
    struct sensor_value gx, gy, gz;
    static char string[100];
    trig_cnt++;

    /* LSM6DSV accel */
    sensor_sample_fetch_chan(imu, SENSOR_CHAN_ACCEL_XYZ);
    sensor_channel_get(imu, SENSOR_CHAN_ACCEL_X, &x);
    sensor_channel_get(imu, SENSOR_CHAN_ACCEL_Y, &y);
    sensor_channel_get(imu, SENSOR_CHAN_ACCEL_Z, &z);
    int count = sprintf(string, "%08d,%f,%f,%f,", k_uptime_get_32(), out_ev(&x), out_ev(&y), out_ev(&z));

    /* LSM6DSV gyro */
    sensor_sample_fetch_chan(imu, SENSOR_CHAN_GYRO_XYZ);
    sensor_channel_get(imu, SENSOR_CHAN_GYRO_X, &gx);
    sensor_channel_get(imu, SENSOR_CHAN_GYRO_Y, &gy);
    sensor_channel_get(imu, SENSOR_CHAN_GYRO_Z, &gz);
    sprintf(&string[count], "%f,%f,%f", out_ev(&gx), out_ev(&gy), out_ev(&gz));
    if (verbose)
    {
        LOG_DBG("%s", string);
    }

    imu_sample_t sample = {0};
    sample.timestamp = k_uptime_get_32(); // TODO: use correct timestamp format
    sample.ax = out_ev(&x);
    sample.ay = out_ev(&y);
    sample.az = out_ev(&z);
    sample.gx = out_ev(&gx);
    sample.gy = out_ev(&gy);
    sample.gz = out_ev(&gz);

    if (callback != NULL)
    {
        callback(sample);
    }

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

    trig.type = SENSOR_TRIG_DATA_READY;
    trig.chan = SENSOR_CHAN_ACCEL_XYZ;

    ret = sensor_trigger_set(imu, &trig, trigger_handler);
    if (ret != 0)
    {
        LOG_ERR("Could not set sensor type and channel");
        return ret;
    }
    else
    {
        LOG_INF("Set sensor type and channel successful.");
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

static int imu_get(const struct shell *sh, size_t argc, char **argv)
{
    imu_sampling();
    return 0;
}

static int imu_enable_shell(const struct shell *sh, size_t argc, char **argv)
{
    if (argc < 2)
    {
        shell_fprintf(sh, SHELL_NORMAL, "usage: imu_enable <freq>\r\n");
        return -1;
    }

    uint16_t freq = strtoul(argv[1], NULL, 10);
    return imu_enable(freq, NULL);
}

static int imu_verbose(const struct shell *sh, size_t argc, char **argv)
{
    verbose = !verbose;
    return 0;
}

SHELL_CMD_REGISTER(imu_get, NULL, "IMU read a sample", imu_get);
SHELL_CMD_REGISTER(imu_enable, NULL, "IMU enable with sampling <freq>", imu_enable_shell);
SHELL_CMD_REGISTER(imu_verbose, NULL, "IMU toggle verbosity", imu_verbose);
