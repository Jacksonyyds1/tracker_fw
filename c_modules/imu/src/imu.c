/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <stdio.h>

LOG_MODULE_REGISTER(imu, LOG_LEVEL_DBG);

//#include "lsm6dsv16x.h"
#include "imu.h"
#include "utils.h"

#if IS_ENABLED(CONFIG_LSM6DSV16X_D1)
#define LSM6DSV DT_INST(0, st_lsm6dsv16x_d1)
#else
#define LSM6DSV DT_INST(0, st_lsm6dsv16x)
#endif
#define LSM6DSV_LABEL    DT_PROP(LSM6DSV, label)
#define ZEPHYR_USER_NODE DT_PATH(zephyr_user)

#define ACCEL_ON (7)

static imu_output_cb_t callback;    // callback for sampled data
const struct device   *imu = DEVICE_DT_GET(DT_ALIAS(imu));
static int             trig_cnt;
static bool            verbose;
static bool            is_asleep;
static int  imu_sampling(void);
static void motion_cb(const struct device *dev, const struct sensor_trigger *trig);

static void trigger_handler(const struct device *dev, const struct sensor_trigger *trig)
{
    imu_sampling();
}

int imu_enable_significant_motion(sensor_trigger_handler_t cb)
{
    struct sensor_trigger trig;

    trig.type = SENSOR_TRIG_MOTION;
    trig.chan = SENSOR_CHAN_ACCEL_XYZ;
    LOG_ERR("%s significant motion", cb ? "Enable" : "Disable");
    if (cb) {
        is_asleep = false;
    }
    return sensor_trigger_set(imu, &trig, cb);
}

/* IMU Initialization */
int imu_init(bool use_mot_det)
{
    // int rc = -1;

    LOG_DBG("IMU Init ...");

    /*
    imu = device_get_binding(LSM6DSV_LABEL);
    if (!imu) {
        LOG_ERR("Error: Cannot bind device %s", LSM6DSV_LABEL);
        return rc;
    } else {
        LOG_DBG("Found device %s.  Good!!!", LSM6DSV_LABEL);
    }
    */

    struct sensor_trigger trig;
    trig.type = SENSOR_TRIG_DATA_READY;
    trig.chan = SENSOR_CHAN_ACCEL_XYZ;
    sensor_trigger_set(imu, &trig, trigger_handler);
    if( use_mot_det){
        /* setup activity / inactivity */
        imu_enable_significant_motion(motion_cb);
    } else {
        imu_enable_significant_motion(NULL);
    }
    return 0;
}

// TODO: are we using a FIFO?
static int imu_sampling(void)
{
    struct sensor_value x, y, z;
    struct sensor_value gx, gy, gz;
    static char         string[100];

    /* LSM6DSV accel */
    sensor_sample_fetch_chan(imu, SENSOR_CHAN_ACCEL_XYZ);
    sensor_channel_get(imu, SENSOR_CHAN_ACCEL_X, &x);
    sensor_channel_get(imu, SENSOR_CHAN_ACCEL_Y, &y);
    sensor_channel_get(imu, SENSOR_CHAN_ACCEL_Z, &z);
    int count = sprintf(
        string,
        "%08d,%f,%f,%f,",
        k_uptime_get_32(),
        sensor_value_to_double(&x),
        sensor_value_to_double(&y),
        sensor_value_to_double(&z));

    /* LSM6DSV gyro */
    sensor_sample_fetch_chan(imu, SENSOR_CHAN_GYRO_XYZ);
    sensor_channel_get(imu, SENSOR_CHAN_GYRO_X, &gx);
    sensor_channel_get(imu, SENSOR_CHAN_GYRO_Y, &gy);
    sensor_channel_get(imu, SENSOR_CHAN_GYRO_Z, &gz);
    sprintf(
        &string[count], "%f,%f,%f", sensor_value_to_double(&gx), sensor_value_to_double(&gy), sensor_value_to_double(&gz));
    if (verbose) {
        LOG_DBG("%s", string);
    }

    imu_sample_t sample = { 0 };
    sample.timestamp    = utils_get_currentmillis();
    sample.sample_count = trig_cnt;
    sample.ax           = sensor_value_to_double(&x);
    sample.ay           = sensor_value_to_double(&y);
    sample.az           = sensor_value_to_double(&z);
    sample.gx           = sensor_value_to_double(&gx);
    sample.gy           = sensor_value_to_double(&gy);
    sample.gz           = sensor_value_to_double(&gz);

    if (callback != NULL) {
        callback(sample, is_asleep);
    }

    trig_cnt++;
    return 0;
}

/* Enable IMU with sampling freq; 0 to disable */
int imu_enable(output_data_rate_t rate, imu_output_cb_t cb)
{
    int                   ret = 0;
    struct sensor_trigger trig;
    struct sensor_value   odr_attr;

    switch (rate) {
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
        trig_cnt = 0;    // clear count when enabling with a non-zero rate
        break;
    default:
        LOG_ERR("invalid rate: %d", rate);
        return -1;
    }

    LOG_DBG("setting output data rate to %d", rate);

    trig.type = SENSOR_TRIG_DATA_READY;
    trig.chan = SENSOR_CHAN_ACCEL_XYZ;

    odr_attr.val1 = rate;
    odr_attr.val2 = 0;    // fractional part

    ret = sensor_attr_set(imu, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_SAMPLING_FREQUENCY, &odr_attr);
    if (ret != 0) {
        LOG_ERR("Cannot set output data rate for accelerometer, ret=%d", ret);
        return ret;
    }

    ret = sensor_attr_set(imu, SENSOR_CHAN_GYRO_XYZ, SENSOR_ATTR_SAMPLING_FREQUENCY, &odr_attr);

    // TODO: gryo returns error when setting a 0 rate
    if (rate != IMU_ODR_0_HZ && ret != 0) {
        LOG_ERR("Cannot set output data rate for gyro, ret=%d", ret);
        return ret;
    }

    callback = cb;

    return 0;
}

static void motion_cb(const struct device *dev, const struct sensor_trigger *trig)
{
    struct sensor_value sleep_state;
    LOG_WRN("Active / inactive change!");

    sensor_attr_get(dev, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_SLEEP_STATE, &sleep_state);
    LOG_WRN("Now %s", sleep_state.val1 ? "Sleeping" : "Awake");
    is_asleep = sleep_state.val1;
}

int imu_get_trigger_count(void)
{
    int local_trig_cnt = trig_cnt;
    trig_cnt           = 0;
    return local_trig_cnt;
}

int imu_set_verbose(bool enable)
{
    verbose = enable;
    return 0;
}

static int print_samples_cb(imu_sample_t data, bool is_sleeping)
{
    printk(
        "%lld, %+.5f, %+.5f, %+.5f, %+.5f, %+.5f, %+.5f, // %s\n",
        utils_get_currentmillis(),
        data.ax,
        data.ay,
        data.az,
        data.gx,
        data.gy,
        data.gz,
        is_sleeping ? "sleeping" : "awake");

    return 0;
}
enum test_mode_e
{
    TEST_MODE_NONE,
    TEST_MODE_ACCEL,
    TEST_MODE_GYRO,
    TEST_MODE_ACCEL_AND_GYRO
};
static enum test_mode_e test_mode = TEST_MODE_NONE;
static int              num_times = 0;
static char             test_mode_string[32];
static int              read_once(imu_sample_t data, bool is_sleeping)
{
    ARG_UNUSED(is_sleeping);
    if (num_times-- > 0) {
        // spin, first result is nasty
    } else {
        if (test_mode == TEST_MODE_ACCEL) {
            snprintf(
                test_mode_string, sizeof(test_mode_string), "tde 0032.%+.2f %+.2f %+.2f \r\n", data.ax, data.ay, data.az);
        } else if (test_mode == TEST_MODE_GYRO) {
            snprintf(
                test_mode_string, sizeof(test_mode_string), "tde 0033.%+.2f %+.2f %+.2f \r\n", data.gx, data.gy, data.gz);
        }
        imu_enable(0, NULL);    // disable the imu
        test_mode = TEST_MODE_NONE;
    }
    return 0;
}

char       boat_sample_string[50] = { 0 };
static int read_some_samples(imu_sample_t data, bool is_sleeping)
{
    ARG_UNUSED(is_sleeping);
    if (num_times-- > 0) {
        // printf("tde0065.%+.2f %+.2f %+.2f ",data.ax, data.ay, data.az);
        // printf("%+.2f %+.2f %+.2f\r\n",data.gx, data.gy, data.gz);
    } else {
        snprintf(
            boat_sample_string,
            sizeof(boat_sample_string),
            "tde 0065.%+.2f %+.2f %+.2f %+.2f %+.2f %+.2f\r\n",
            data.ax,
            data.ay,
            data.az,
            data.gx,
            data.gy,
            data.gz);
        imu_enable(0, NULL);    // disable the imu
        test_mode = TEST_MODE_NONE;
    }
    return 0;
}

static int imu_start_shell(const struct shell *sh, size_t argc, char **argv)
{
    shell_fprintf(sh, SHELL_NORMAL, "starting IMU...\r\n");
    imu_enable_significant_motion(motion_cb);
    return imu_enable(IMU_ODR_15_HZ, print_samples_cb);
}

char *tde0032()
{
    test_mode = TEST_MODE_ACCEL;
    num_times = 1;
    memset(test_mode_string, 0, sizeof(test_mode_string));
    imu_enable_significant_motion(NULL);
    imu_enable(IMU_ODR_120_HZ, read_once);
    while (test_mode != TEST_MODE_NONE) {
        k_usleep(100);
    }
    return test_mode_string;
}
char *tde0033()
{
    test_mode = TEST_MODE_GYRO;
    num_times = 1;
    memset(test_mode_string, 0, sizeof(test_mode_string));
    imu_enable_significant_motion(NULL);
    imu_enable(IMU_ODR_120_HZ, read_once);
    while (test_mode != TEST_MODE_NONE) {
        k_usleep(100);
    }
    return test_mode_string;
}

void tde0064()
{
    num_times = 30;
    test_mode = TEST_MODE_ACCEL_AND_GYRO;
    memset(boat_sample_string, 0, sizeof(boat_sample_string));
    imu_enable_significant_motion(NULL);
    imu_enable(IMU_ODR_30_HZ, read_some_samples);
    // NB this test is asynchronous ... the boat starts moving when this returns,
    // so don't wait for samples, just return (and check that we got samples in tde0065)
    return;
}

char *tde0065()
{
    while (test_mode != TEST_MODE_NONE) {
        k_usleep(100);
    }
    return boat_sample_string;
}

int imu_set_mot_det(bool is_on)
{
    LOG_DBG("imu_set_mot_det");
    return imu_enable_significant_motion(is_on ? motion_cb : NULL);
}

int imu_set_threshold(uint32_t ths)
{
    struct sensor_value val;

    val.val1 = ths;
    return sensor_attr_set(imu, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_SLOPE_TH, &val);
}

int imu_get_threshold(uint32_t *ths)
{
    struct sensor_value val;
    sensor_attr_get(imu, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_SLOPE_TH, &val);
    *ths = val.val1;
    return 0;
}

int imu_set_duration(uint32_t dur)
{
    struct sensor_value val;

    val.val1 = dur;
    return sensor_attr_set(imu, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_SLOPE_DUR, &val);
}

int imu_get_duration(uint32_t *dur)
{
    struct sensor_value val;
    sensor_attr_get(imu, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_SLOPE_DUR, &val);
    *dur = val.val1;
    return 0;
}

static int imu_stop_shell(const struct shell *sh, size_t argc, char **argv)
{
    imu_enable(0, NULL);
    shell_fprintf(sh, SHELL_NORMAL, "stopped IMU\r\n");
    return 0;
}

static int imu_sigmot_shell(const struct shell *shell, size_t argc, char **argv)
{
    bool is_on = 0;
    int  err   = 0;

    if (argc >= 1) {
        is_on = shell_strtobool(argv[1], 0, &err);
    }
    if (!err) {
        imu_enable_significant_motion(is_on ? motion_cb : NULL);
    }
    return err;
}

static int imu_stats_shell(const struct shell *sh, size_t argc, char **argv)
{
    shell_print(sh, "%d", trig_cnt);
    return 0;
}

static int imu_dur_shell(const struct shell *sh, size_t argc, char **argv)
{
    if (argc > 1) {
        struct sensor_value val;

        val.val1 = strtoul(argv[1], NULL, 10);
        return sensor_attr_set(imu, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_SLOPE_DUR, &val);
    }
    uint32_t dur;
    imu_get_duration(&dur);
    shell_print(sh, "Duration: %d", dur);
    return 0;
}

static int imu_ths_shell(const struct shell *sh, size_t argc, char **argv)
{
    if (argc > 1) {
        struct sensor_value val;

        val.val1 = strtoul(argv[1], NULL, 10);
        return sensor_attr_set(imu, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_SLOPE_TH, &val);
    }
    uint32_t ths;
    imu_get_threshold(&ths);
    shell_print(sh, "Threshold: %d", ths);
    return 0;
}

static int imu_read_sample(const struct shell *sh, size_t argc, char **argv)
{
    imu_sampling();
    return 0;
}

#ifdef CONFIG_LSM6DSV16X_D1_ENABLE_TEMP
static int imu_temp_shell(const struct shell *sh, size_t argc, char **argv)
{
    struct sensor_value val;
    sensor_channel_get(imu, SENSOR_CHAN_DIE_TEMP, &val);
    shell_print(sh, "%d.%03d", val.val1, val.val2);
    return 0;
}
#endif

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

SHELL_STATIC_SUBCMD_SET_CREATE(
    sub_imu,
    SHELL_CMD(start, NULL, "start IMU at 15Hz, print to console", imu_start_shell),
    SHELL_CMD(stop, NULL, "stop IMU", imu_stop_shell),
    SHELL_CMD(dur, NULL, "set sleep duration", imu_dur_shell),
    SHELL_CMD(ths, NULL, "set sleep threshold", imu_ths_shell),
    SHELL_CMD(read, NULL, "stop IMU", imu_read_sample),
    SHELL_CMD(sigmot, NULL, "Turn on/off significant motion detection", imu_sigmot_shell),
    SHELL_CMD(stats, NULL, "Print number of IMU interrupts", imu_stats_shell),
#ifdef CONFIG_LSM6DSV16X_D1_ENABLE_TEMP
    SHELL_CMD(temp, NULL, "Read the temperature from the IMU", imu_temp_shell),
#endif
    SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(imu, &sub_imu, "Control the IMU", NULL);

SHELL_CMD_REGISTER(tde65, NULL, "stop IMU", tde0065);
