#include <stdio.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>

LOG_MODULE_REGISTER(imu, LOG_LEVEL_DBG);

#include "imu.h"
#include "utils.h"

// this is in libc math.h, not in newlibc nano
#define M_PI 3.14159265358979323846

// this is 'g', in m/s^2
#define ACCEL_GRAVITY_EARTH (9.80665)

#define LSM6DSV          DT_INST(0, st_lsm6dsv16x)
#define LSM6DSV_LABEL    DT_PROP(LSM6DSV, label)
#define ZEPHYR_USER_NODE DT_PATH(zephyr_user)

#define ACCEL_ON (7)
typedef enum { // IMU operation mode
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

static void trigger_handler(const struct device *dev, const struct sensor_trigger *trig)
{
	if (imu_operation_mode == IMU_INTERRUPT_MODE) {
		imu_sampling();
	}
}

/* IMU Initialization */
int imu_init(void)
{
	int rc = -1;

	LOG_DBG("IMU Init ...");

	imu = device_get_binding(LSM6DSV_LABEL);
	if (!imu) {
		LOG_ERR("Error: Cannot bind device %s", LSM6DSV_LABEL);
		return rc;
	} else {
		LOG_DBG("Found device %s.  Good!!!", LSM6DSV_LABEL);
	}

	struct sensor_trigger trig;
	trig.type = SENSOR_TRIG_DATA_READY;
	trig.chan = SENSOR_CHAN_ACCEL_XYZ;
	sensor_trigger_set(imu, &trig, trigger_handler);

	return 0;
}

// convert rad/s to dps
double rad_s_to_dps(double rad_s)
{
	return rad_s * (180.0 / M_PI);
}

// convert m/s^2 to g
double m_s2_to_g(double m_s2)
{
	return m_s2 / ACCEL_GRAVITY_EARTH;
}

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
	int count = sprintf(string, "%08d,%f,%f,%f,", k_uptime_get_32(), out_ev(&x), out_ev(&y),
			    out_ev(&z));

	/* LSM6DSV gyro */
	sensor_sample_fetch_chan(imu, SENSOR_CHAN_GYRO_XYZ);
	sensor_channel_get(imu, SENSOR_CHAN_GYRO_X, &gx);
	sensor_channel_get(imu, SENSOR_CHAN_GYRO_Y, &gy);
	sensor_channel_get(imu, SENSOR_CHAN_GYRO_Z, &gz);
	sprintf(&string[count], "%f,%f,%f", out_ev(&gx), out_ev(&gy), out_ev(&gz));
	if (verbose) {
		LOG_DBG("%s", string);
	}

	imu_sample_t sample = {0};
	sample.timestamp = utils_get_currentmillis();
	sample.sample_count = trig_cnt;

	// we get m/s^2 from SENSOR_CHAN_ACCEL_N API
	float ax_m_s2, ay_m_s2, az_m_s2;
	ax_m_s2 = out_ev(&x);
	ay_m_s2 = out_ev(&y);
	az_m_s2 = out_ev(&z);

	// convert m/s^2 to g's
	sample.ax = m_s2_to_g(ax_m_s2);
	sample.ay = m_s2_to_g(ay_m_s2);
	sample.az = m_s2_to_g(az_m_s2);

	float gx_rad_s, gy_rad_s, gz_rad_s;

	gx_rad_s = out_ev(&gx);
	gy_rad_s = out_ev(&gy);
	gz_rad_s = out_ev(&gz);

	sample.gx = rad_s_to_dps(gx_rad_s);
	sample.gy = rad_s_to_dps(gy_rad_s);
	sample.gz = rad_s_to_dps(gz_rad_s);

	if (callback != NULL) {
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

	ret = sensor_attr_set(imu, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_SAMPLING_FREQUENCY,
			      &odr_attr);
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
	if (argc < 2) {
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

SHELL_CMD_REGISTER(imu_get, NULL, "IMU read a sample", imu_get);
SHELL_CMD_REGISTER(imu_enable, NULL, "IMU enable with sampling <freq>", imu_enable_shell);
SHELL_CMD_REGISTER(imu_verbose, NULL, "IMU toggle verbosity", imu_verbose);
