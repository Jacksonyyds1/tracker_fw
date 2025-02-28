#pragma once
#include <stdint.h>
#include <stddef.h>
#include "imu.h"
#include "storage.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BT_UUID_CHEKR_SERVICE_VAL                                                                  \
	BT_UUID_128_ENCODE(0x00c74f02, 0xd1bc, 0x11ed, 0xafa1, 0x0242ac120002)

#define RECORD_SAMPLE_RATE (IMU_ODR_15_HZ)

// public interfaces
int chekr_service_init(void);
int chekr_record_samples(imu_sample_t data);

// used for testing from shell
int chekr_set_filehandle(file_handle_t handle);

// private interfaces

#define SET_EPOCH_RTC_FRAME_LEN            (0x0e)
#define GET_EPOCH_RTC_FRAME_LEN            (0x07)
#define SET_DOG_COLLAR_POS_FRAME_LEN       (0x07)
#define SET_DOG_SIZE_FRAME_LEN             (0x07)
#define READ_DASHBOARD_INFO_FRAME_LEN      (0x07)
#define START_STOP_REC_SESSION_FRAME_LEN   (0x0f)
#define READ_REC_SESSION_DETAILS_FRAME_LEN (0x0f)
#define READ_REC_SESSION_DATA_FRAME_LEN    (0x13)
#define GET_DEVICE_MAC_ADDR_FRAME_LEN      (0x07)
#define GET_DEVICE_SYSTEM_INFO_FRAME_LEN   (0x07)
#define FACTORY_RESET_FRAME_LEN            (0x07)
#define REBOOT_FRAME_LEN                   (0x07)
#define BLE_STATUS_SHOW_FRAME_LEN          (0x07)
#define BLE_CONNECTED_SHOW_FRAME_LEN       (0x07)
#define START_STOP_PERIODIC_DASH_FRAME_LEN (0x8)

#define NESTLE_COMMERCIAL_PET_COLLAR (6)
typedef enum {
	GET_DEVICE_MAC_ADDR = 1,
	GET_DEVICE_SYSTEM_INFO,
	GET_USER_INFO,
	SET_DOG_COLLAR_POSITION,
	GET_EPOCH_RTC,
	SET_EPOCH_RTC,
	START_STOP_RAW_DATA_HARVESTING,
	START_STOP_ACTIVITY_DATA_HARVESTING,
	START_STOP_PERIODIC_DASHBOARD_STATUS_INFO,
	BLE_STATUS_SHOW,
	BLE_CONNECTED_SHOW,
	FACTORY_RESET,
	REBOOT,
	SYSTEM_ALARM_STATUS_NOTIF,
	SYSTEM_GENERAL_NOTIF,
	START_STOP_RECORDING_SESSION,
	READ_REC_SESSION_DETAILS_RAW_IMU,
	READ_REC_SESSION_DATA_RAW_IMU,
	READ_REC_SESSION_DETAILS_ACTIVITY,
	READ_REC_SESSION_DATA_ACTIVITY,
	READ_DEVICE_LOG_INFO,
	READ_DASHBOARD_INFO,
	START_BLE_LIVE_ACTIVITY_SESSION,
	START_BLE_LIVE_RAW_IMU_RECORDING,
	START_DISCO_LIGHTS,
	SET_DOG_SIZE,
} commands_list_t;

typedef enum {
	SB_MOBILE_APP_TO_DEVICE = 0x01,
	SB_DEVICE_TO_MOBILE_APP,
	SB_RESERVED,
} start_byte_t;

typedef enum {
	FT_INVALID = 0xf0,
	FT_REQUEST,
	FT_REPORT,
	FT_NOTIF,
	FT_DATA_FRAME,
} frame_type_t;

typedef enum {
	ERR_NONE = 0x11,
	ERR_NEGATIVE_ACK,
	ERR_BAD_CHECKSUM,
	ERR_LENGTH_MISMATCH,
	ERR_INVALID_START_BYTE,
	ERR_UNRECOGNIZED_FRAME_TYPE,
	ERR_UNRECOGNIZED_CMD,
	ERR_INVALID_DATA,
	ERR_CANNOT_PROCESS_OR_BUSY,
	ERR_SLAVE_DEVICE_FAILURE,
	ERR_TIMEOUT,
} error_code_t;

typedef enum {
	DASH_START,
	DASH_STOP,
	DASH_ONCE,
} dashboard_ctrl_t;

typedef enum {
	PET_SIZE_GIANT = 0,
	PET_SIZE_LARGE,
	PET_SIZE_MEDIUM,
	PET_SIZE_SMALL,
	PET_SIZE_TOY,
} pet_size_t;

typedef struct __attribute__((__packed__)) {
	uint8_t start_byte;
	uint8_t frame_len;
	uint8_t frame_type;
	uint8_t cmd;
} frame_format_header_t;

int dashboard_init(void);
int dashboard_ctrl(dashboard_ctrl_t ctrl, uint8_t interval);
int dashboard_response(void);
void write_to_central(uint8_t *data, int len);

// recording related jump table functions in chekr_record module
int start_stop_rec_session(char *data, int len);
int read_rec_session_details_raw_imu(char *data, int len);
int read_rec_session_data_raw_imu(char *data, int len);
int read_rec_session_details_activity(char *data, int len);
int read_rec_session_data_activity(char *data, int len);

#ifdef __cplusplus
}
#endif
