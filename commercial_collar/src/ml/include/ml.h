/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "imu.h"
#include "fqueue.h"

typedef enum
{
    PET_SIZE_GIANT = 0,
    PET_SIZE_LARGE,
    PET_SIZE_MEDIUM,
    PET_SIZE_SMALL,
    PET_SIZE_TOY,
} pet_size_t;

// the amount by which we have to downsample RECORD_SAMPLE_RATE to equal 15Hz.
#define ML_15HZ_DOWNSAMPLE_RATE (1)

#define MS2_TO_G(x)                   (x / 9.80665)
#define RAD_PER_SEC_TO_DEG_PER_SEC(x) (x * 57.29578)

int         ml_init(void);
int         ml_set_dog_size(pet_size_t pet_size);
int         ml_start(void);
int         ml_stop(void);
const char *ml_version(void);

// called from record callback
int ml_feed_sample(imu_sample_t data, bool is_sleeping);

#ifdef __cplusplus
}
#endif
