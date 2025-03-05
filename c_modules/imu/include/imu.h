/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */
#pragma once

#include <zephyr/drivers/sensor.h>
// note: we're currently not supporting 1.875 or 7.5 Hz rates
typedef enum
{
    IMU_ODR_0_HZ,
    IMU_ODR_15_HZ   = 15,
    IMU_ODR_30_HZ   = 30,
    IMU_ODR_60_HZ   = 60,
    IMU_ODR_120_HZ  = 120,
    IMU_ODR_240_HZ  = 240,
    IMU_ODR_480_HZ  = 480,
    IMU_ODR_960_HZ  = 960,
    IMU_ODR_1920_HZ = 1920,
    IMU_ODR_3840_HZ = 3840,
    IMU_ODR_7680_HZ = 7680,
    IMU_ODR_MAX_HZ,
} output_data_rate_t;

typedef struct
{
    float    ax, ay, az;    // accelerometer
    float    gx, gy, gz;    // gyro
    uint64_t timestamp;
    uint32_t sample_count;
} imu_sample_t;

// the following is actually defined in lsm6dsv16x.h in the d1 driver
// but redefine here for now
#define SENSOR_ATTR_SLEEP_STATE (SENSOR_ATTR_PRIV_START + 1)


typedef int (*imu_output_cb_t)(imu_sample_t output, bool is_sleeping);

int   imu_init(bool use_motion_detect);
int   imu_enable(output_data_rate_t rate, imu_output_cb_t callback);
int   imu_enable_significant_motion(sensor_trigger_handler_t cb);
int   imu_set_threshold(uint32_t ths);
int   imu_set_duration(uint32_t dur);
int   imu_get_trigger_count(void);
int   imu_set_verbose(bool enable);
int   imu_set_mot_det(bool is_on);
char *tde0032();
char *tde0033();