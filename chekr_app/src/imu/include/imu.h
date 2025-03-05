#pragma once

#define NUM_FLOATS_PER_IMU_SAMPLE (6)

// note: we're currently not supporting 1.875 or 7.5 Hz rates
typedef enum {
	IMU_ODR_0_HZ,
	IMU_ODR_15_HZ = 15,
	IMU_ODR_30_HZ = 30,
	IMU_ODR_60_HZ = 60,
	IMU_ODR_120_HZ = 120,
	IMU_ODR_240_HZ = 240,
	IMU_ODR_480_HZ = 480,
	IMU_ODR_960_HZ = 960,
	IMU_ODR_1920_HZ = 1920,
	IMU_ODR_3840_HZ = 3840,
	IMU_ODR_7680_HZ = 7680,
	IMU_ODR_MAX_HZ,
} output_data_rate_t;

typedef struct {
	uint64_t timestamp;
	uint32_t sample_count;
	float ax, ay, az; // accelerometer
	float gx, gy, gz; // gyro
} imu_sample_t;

typedef int (*imu_output_cb_t)(imu_sample_t output);

int imu_init(void);
int imu_enable(output_data_rate_t rate, imu_output_cb_t callback);
int imu_get_trigger_count(void);
int imu_set_verbose(bool enable);
