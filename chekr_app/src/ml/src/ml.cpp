#include <zephyr/shell/shell.h>
#include <zephyr/sys/byteorder.h>

LOG_MODULE_REGISTER(ml, LOG_LEVEL_DBG);

#include "baseLib.hpp"
#include "ml.h"
#include "storage.h"
#include "utils.h"

// NOTE: we assume a 15Hz IMU record sample rate, so we don't need to downsample when
// feeding ML model.  If it changes, ML_15HZ_DOWNSAMPLE_RATE must be adjusted
static_assert(RECORD_SAMPLE_RATE == IMU_ODR_15_HZ, "Expected 15Hz Sample Rate");

// recommended in API doc version 3.1.0
#define REPETITION_CALL_INTERVAL (10)

static bool m_verbose;
static int m_record_num;
static file_handle_t m_file_handle; // file handle for writing Activity data
static PetSize m_pet_size = size_medium;

static void handle_result(InferResult result);

int ml_init(void)
{
	LOG_DBG("ML Init ...");
	// TODO: any ML related initialization here

	// initialize with defaults, we will re-initialize during record session
	bool ret = initiate(handle_result, REPETITION_CALL_INTERVAL, m_pet_size);
	if (ret == false) {
		LOG_ERR("can't start ML library!");
		return -1;
	}

	return 0;
}

int ml_start(bool start, file_handle_t handle)
{
	const char *op;

	if (start) {
		op = "starting";
		m_record_num = 0;
		bool ret = initiate(handle_result, REPETITION_CALL_INTERVAL, m_pet_size);
		if (ret == false) {
			LOG_ERR("can't start ML library!");
			return -1;
		}
		m_file_handle = handle;
	} else {
		op = "stopping";
		m_file_handle = NULL;
	}

	LOG_DBG("%s stream to ML model", op);

	return 0;
}

// convert C enum to CPP enum
int ml_set_dog_size(pet_size_t pet_size)
{
	PetSize size = size_medium;

	switch (pet_size) {
	case PET_SIZE_GIANT:
		size = size_giant;
		break;
	case PET_SIZE_LARGE:
		size = size_large;
		break;
	case PET_SIZE_MEDIUM:
		size = size_medium;
		break;
	case PET_SIZE_SMALL:
		size = size_small;
		break;
	case PET_SIZE_TOY:
		size = size_toy;
		break;
	default:
		LOG_ERR("invalid pet size: %d", pet_size);
		break;
	}

	m_pet_size = size;
	LOG_DBG("set pet size to %d", size);
	return 0;
}

int ml_feed_sample(imu_sample_t data)
{
	if (data.sample_count % ML_15HZ_DOWNSAMPLE_RATE == 0) {
		// feed ML model

		if (m_verbose) {
			LOG_DBG("feeding sample at %llu", data.timestamp);
		}

		IMUData imu_sample = {0};
		imu_sample.timestamp = data.timestamp;
		imu_sample.imuValues[0] = data.ax;
		imu_sample.imuValues[1] = data.ay;
		imu_sample.imuValues[2] = data.az;
		imu_sample.imuValues[3] = data.gx;
		imu_sample.imuValues[4] = data.gy;
		imu_sample.imuValues[5] = data.gz;
		processIMUData(imu_sample);
	}

	return 0;
}

/*
*****************************
Required functions for ML library
*****************************
*/
uint64_t ei_read_timer_ms()
{
	return utils_get_currentmillis();
}

uint64_t ei_read_timer_us()
{
	return utils_get_currentmicros();
}

// handle inference results
static void handle_result(InferResult result)
{
	// Print the inference result
	LOG_DBG("Inference Result: %d", m_record_num);
	LOG_DBG("Model Type: %d", result.modelType);
	LOG_DBG("Current Activity: %d", result.curntActivity);
	LOG_DBG("IMU Timestamp: %llu", result.imu_timestamp);
	LOG_DBG("Repetition: %.2f", result.repetition);
	LOG_DBG("Joint Health: %d", result.jointHealth);

	activity_record_t activity_record = {0};
	activity_record.record_num = sys_cpu_to_be32(m_record_num);
	m_record_num++;
	activity_record.activity_data.timestamp = result.imu_timestamp;
	activity_record.activity_data.start_byte = '*';
	activity_record.activity_data.model_type = result.modelType;
	activity_record.activity_data.activity_type = result.curntActivity;
	activity_record.activity_data.repetition_count = result.repetition;
	activity_record.activity_data.joint_health = result.jointHealth;

	if (m_file_handle) {
		int ret = storage_write_activity_record(m_file_handle, activity_record);
		if (ret) {
			LOG_ERR("failed to write activity record: %d", ret);
		}
	}
}

//***************************

static int ml_verbose_shell(const struct shell *sh, size_t argc, char **argv)
{
	m_verbose = !m_verbose;
	return 0;
}

static int ml_version_shell(const struct shell *sh, size_t argc, char **argv)
{
	shell_fprintf(sh, SHELL_NORMAL, "version: %s\r\n", getVersion());
	return 0;
}

SHELL_CMD_REGISTER(ml_verbose, NULL, "ML toggle verbosity", ml_verbose_shell);
SHELL_CMD_REGISTER(ml_version, NULL, "Print version of ML library", ml_version_shell);
