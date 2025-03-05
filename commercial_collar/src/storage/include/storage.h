/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

// opaque type for handle, internal structure is hidden
typedef struct file_data_t  file_data_t;
typedef struct file_data_t *file_handle_t;

int storage_init(void);

// open a new file for writing.  returns file handle or NULL
file_handle_t storage_open_file(const char *fname);

// close and save file
int storage_close_file(file_handle_t handle);

// activity data related interfaces
int storage_write_activity_record(file_handle_t handle, const uint8_t *data, size_t nbytes);
int storage_get_activity_record_count(char *basename);
int storage_read_activity_record(char *basename, int record_number, uint8_t *data, size_t dsize);

// helper for tests
int storage_delete_activity_file(char *basename);

#ifdef __cplusplus
}
#endif
