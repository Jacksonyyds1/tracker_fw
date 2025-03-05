#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "chekr.h"
#include "imu.h"

// the amount by which we have to downsample RECORD_SAMPLE_RATE to equal 15Hz.
#define ML_15HZ_DOWNSAMPLE_RATE (1)

int ml_init(void);
int ml_set_dog_size(pet_size_t pet_size);
int ml_start(bool start, file_handle_t handle);

// called from record callback
int ml_feed_sample(imu_sample_t data);

#ifdef __cplusplus
}
#endif
