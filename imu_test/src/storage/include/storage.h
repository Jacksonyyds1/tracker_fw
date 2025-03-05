#pragma once
#include <stdint.h>
#include "imu.h"

int storage_init(void);

// open a new file for writing.  returns new file name or NULL
int storage_open_file(char *fname);

// close and save file
int storage_close_file(char *fname);

// write an IMU sample to opened file
int storage_write_imu_sample(imu_sample_t imu_sample);
