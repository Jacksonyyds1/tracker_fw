#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#define RAW_FRAMES_PER_RECORD (10)

// opaque type for handle, internal structure is hidden
typedef struct file_data_t file_data_t;
typedef struct file_data_t *file_handle_t;

typedef struct __attribute__((__packed__)) {
	float ax, ay, az; // accelerometer
	float gx, gy, gz; // gyro
} raw_imu_data_frame_t;

// ChekrAppLink API sends raw data as a 252-byte record,
// containing 10 samples per record
typedef struct __attribute__((__packed__)) {
	uint32_t record_num;
	uint64_t timestamp; // currentmillis
	raw_imu_data_frame_t raw_data[RAW_FRAMES_PER_RECORD];
} raw_imu_record_t;

typedef struct __attribute__((__packed__)) {
	uint64_t timestamp; // currentmillis
	uint8_t start_byte;
	uint8_t model_type;
	uint8_t activity_type;
	float repetition_count;
	uint8_t joint_health;
} activity_data_frame_t;

typedef struct __attribute__((__packed__)) {
	uint32_t record_num;
	activity_data_frame_t activity_data; // only one per frame
} activity_record_t;

int storage_init(void);

// open a new file for writing.  returns file handle or NULL
file_handle_t storage_open_file(char *fname);

// close and save file
int storage_close_file(file_handle_t handle);

// raw IMU related interfaces
int storage_write_raw_imu_record(file_handle_t handle, raw_imu_record_t raw_record);
int storage_get_raw_imu_record_count(char *basename);
int storage_read_raw_imu_record(char *basename, int record_number, raw_imu_record_t *record);

// activity data related interfaces
int storage_write_activity_record(file_handle_t handle, activity_record_t raw_record);
int storage_get_activity_record_count(char *basename);
int storage_read_activity_record(char *basename, int record_number, activity_record_t *record);

// helper for tests (TODO: name this common for both file types)
int storage_delete_activity_file(char *basename);

#ifdef __cplusplus
}
#endif
