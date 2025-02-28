/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */
#pragma once

#include <stdlib.h>
#include <stdio.h>
#include <stddef.h>

typedef enum
{
    MESSAGE_TYPE_NO_OP               = 0,
    MESSAGE_TYPE_RESPONSE            = 1,    // generic response to a message
    MESSAGE_TYPE_COMMAND             = 2,    // command to device command_type_t
    MESSAGE_TYPE_AT                  = 3,    // AT command
    MESSAGE_TYPE_MQTT                = 5,    // MQTT message
    MESSAGE_TYPE_DEVICE_PING         = 7,
    MESSAGE_TYPE_COMMAND_RESP        = 9,    // response to a command
    MESSAGE_TYPE_DEVICE_PONG         = 10,
    MESSAGE_TYPE_DEVICE_STATUS       = 11,    // constantly updating things like connection status, etc
    MESSAGE_TYPE_GET_TIME            = 13,    // get the current time from the cell network
    MESSAGE_TYPE_FOTA_FROM_HTTPS     = 14,    // download a file from a URL and update
    MESSAGE_TYPE_DOWNLOAD_FROM_HTTPS = 15,    // download a file from a URL
    MESSAGE_TYPE_DEVICE_INFO         = 16,    // one time things like IMEI, ICCID, etc
    MESSAGE_TYPE_DEVICE_VERSION =
        17,    // get the version of the modem and FW - moving from COMMAND_GET_VERSION for compatability
    MESSAGE_TYPE_LTE_DEBUG     = 18,    // get debug info from the modem
    MESSAGE_TYPE_GPS_DATA      = 19,    // get GPS data
    MESSAGE_TYPE_CELL_INFO     = 20,    // get cell info
    MESSAGE_TYPE_CELL_TRACKING = 21,    // get cell tracking info
    MESSAGE_TYPE_FW_UPLOAD     = 22,    // upload a file to the modem
    MESSAGE_TYPE_NULL          = 0xff
} modem_message_type_t;

typedef enum
{
    COMMAND_NO_OP       = 0,
    COMMAND_REBOOT      = 1,    // reboot the modem
    COMMAND_START       = 2,    // start the modem, (used at factory, for letting the modem boot the rest of the way)
    COMMAND_HARD_STOP   = 3,    // stop the modem and don't restart
    COMMAND_GET_VERSION = 4,    // get the version of the modem and FW - DEPRECATED
    COMMAND_SET_MQTT_PARAMS = 5,    // takes a JSON string with a set of values needed to login/connect (see below)
    COMMAND_SET_MQTT_SUBSCRIPTIONS = 6,    // takes a JSON string with an array of topics to subscribe to (see below)
    COMMAND_SET_MQTT_CONNECT    = 7,    // connect to the MQTT broker, called after sending the params and subscriptions
    COMMAND_SET_MQTT_DISCONNECT = 8,    // disconnect from the MQTT broker
    COMMAND_SET_FOTA_CANCEL     = 9,    // cancel any ongoing fota
    COMMAND_SET_PAGE_CYCLE      = 10,    // set the eDRX and PTW time
    COMMAND_SET_AIRPLANE_MODE   = 11,    // set airplane mode on or off
    COMMAND_GPS_START           = 12,    // start the GPS
    COMMAND_GPS_STOP            = 13,    // stop the GPS
    COMMAND_SET_DNS             = 14,    // set the DNS server
    COMMAND_PING                = 15,    // ping a server
    COMMAND_NULL                = 0xff
} command_type_t;

typedef enum
{
    DOWNLOAD_COMPLETE   = 0,
    DOWNLOAD_ERROR      = 1,
    DOWNLOAD_INPROGRESS = 2
} download_response_type_t;

// JSON string for COMMAND_SET_MQTT_PARAMS
// {
//     "host": "mqtt.example.com",
//     "port", 1883,
//     "client_id": "client_id",
// }

// JSON string for COMMAND_SET_MQTT_SUBSCRIPTIONS
// {
//     "topics": [
//         { "topic": "topicstring1", "qos": 0 },
//         { "topic": "topicstring2", "qos": 0 },
//         { "topic": "topicstring3", "qos": 0 },
//     ]
// }

typedef struct
{
    uint16_t msg_length;
    uint16_t topic_length;
    uint8_t  qos;
    // follow with raw data for topic and message according to sizes above
} __attribute__((__packed__)) spi_mqtt_t;

typedef struct
{
    uint8_t  version;
    uint8_t  messageType;
    uint8_t  messageHandle;
    uint16_t dataLen;
    uint8_t  chunkNum;
    uint8_t  chunkTotal;
    // raw data follows
} __attribute__((__packed__)) message_command_v1_t;

typedef struct version_response_t
{
    uint8_t major;
    uint8_t minor;
    uint8_t patch;
    char    githash[16];
    char    build_date[32];
    char    build_machine[16];
    char    modem_fw[32];
} __attribute__((__packed__)) version_response_t;

typedef struct download_response_t
{
    uint8_t  status;
    uint8_t  progress_percent;
    uint16_t current_payload_size;
    uint64_t progress_bytes;
    uint64_t total_size;
    uint32_t crc;
} __attribute__((__packed__)) download_response_t;

typedef struct modem_status_type
{
    uint32_t status_flags;
    char     timestamp[32];      // str like: "24/02/28,23:27:31-32"
    int32_t  rssi;               // negative value float * 10 to get integer value
    uint32_t fota_state;         // 0 = idle, 1 = downloading, 2 = installing, 3 = rebooting, 99 = error
    uint8_t  fota_percentage;    // 0-100
    uint8_t  temperature;        // 0-100
    uint8_t  pad2;               // 0-100
    uint8_t  pad3;               // 0-100
    uint64_t uptime;             // seconds since last boot
} modem_status_t;

typedef struct gps_info_type
{
    int64_t  latitude;          // float * 10000 to get integer value
    int64_t  longitude;         // float * 10000 to get integer value
    int32_t  altitude;          // float * 100 to get integer value
    uint16_t accuracy;          // float * 100 to get integer value
    uint16_t speed;             // float * 100 to get integer value
    uint16_t heading;           // float * 100 to get integer value
    uint16_t speed_accuracy;    // float * 100 to get integer value
    uint16_t numSatellites;     // number of satellites used by gnss system
    uint16_t pad1;              // filler - alignment
    int32_t  secSinceLock;      // seconds since last lock
    int32_t  timeToLock;        // seconds to lock
    uint64_t timestamp;         // seconds since epoch
} __attribute__((__packed__)) gps_info_t;

typedef struct modem_info_type
{
    char iccid[32];
    char imei[32];
    char operator[32];
    char cellID[16];
    char ap[16];
    char subscriber[32];
} __attribute__((__packed__)) modem_info_t;

typedef struct inc_mqtt_event
{
    char *mqtt_msg;
    int   msg_length;
    int   topic_length;
    char *topic;
} inc_mqtt_event_t;

typedef struct firmware_upload
{    // followed by data_len of raw data
    uint16_t chunk_num;
    uint16_t chunk_total;
    uint16_t data_len;
    uint16_t return_code;    // used in the reply, keeping the rest of the data the same.  not followed by raw data on reply
                             // 0 = ready for next chunk, or if last chunk, ready for install
                             // 1 = invalid chunk number, maybe skipped one?
                             // 2 = invalid chunk size, maybe too big or too small
                             // 3 = invalid crc
                             // 4 = invalid data
                             // 5 = invalid state, not ready for data
                             // 6 = unknown error
    uint32_t crc;
} __attribute__((__packed__)) firmware_upload_t;


typedef struct debug_info_message_type
{
    uint8_t  error_code;
    uint8_t  debug_level;
    uint16_t debug_string_length;
} debug_info_message_t;


typedef struct cell_tracking_data
{
    uint32_t num_tower_data;
} cell_tracking_data_t;


typedef struct cell_info
{
    char     cellID[16];
    uint16_t tracking_area;
    uint8_t  ip[4];
    //uint8_t pad[12];  // was partially used for mac, which I cannot find in the data
    int32_t  rssi;
    uint8_t  lte_nbiot_mode;    // 0 == LTE, 1 == NB-IoT, 2 == unknown
    uint8_t  lte_band;
    uint8_t  pad3;
    uint8_t  neighbor_count;
    uint16_t mcc;
    uint16_t mnc;
    int16_t  pad2;
} __attribute__((__packed__)) cell_info_t;


enum status_bit_fields
{
    STATUS_LTE_CONNECTED,
    STATUS_LTE_ENABLED,
    STATUS_LTE_WORKING,
    STATUS_FOTA_IN_PROGRESS,
    STATUS_MQTT_CONNECTED,
    STATUS_GPS_CONNECTED,
    STATUS_MQTT_INITIALIZED,
    STATUS_POWERED_ON,
    STATUS_MQTT_WORKING,
    STATUS_CERTS_LOADED,
    STATUS_AIRPLANE_MODE,
    STATUS_MQTT_ENABLED,
    STATUS_GPS_ENABLED,
    STATUS_CELL_DATA_CHANGED,
    STATUS_CELL_TRACKING_CHANGED,
    STATUS_BIT15
};

// The following is sent when the status of the modem changes and a
// z_bus message is published to indicate what changed
enum status_change_bits
{
    UPDATE_STATUS_LTE_CONNECTED         = (1 << STATUS_LTE_CONNECTED),
    UPDATE_STATUS_LTE_ENABLED           = (1 << STATUS_LTE_ENABLED),
    UPDATE_STATUS_LTE_WORKING           = (1 << STATUS_LTE_WORKING),
    UPDATE_STATUS_FOTA_IN_PROGRESS      = (1 << STATUS_FOTA_IN_PROGRESS),
    UPDATE_STATUS_MQTT_CONNECTED        = (1 << STATUS_MQTT_CONNECTED),
    UPDATE_STATUS_GPS_CONNECTED         = (1 << STATUS_GPS_CONNECTED),
    UPDATE_STATUS_MQTT_INITIALIZED      = (1 << STATUS_MQTT_INITIALIZED),
    UPDATE_STATUS_POWERED_ON            = (1 << STATUS_POWERED_ON),
    UPDATE_STATUS_MQTT_WORKING          = (1 << STATUS_MQTT_WORKING),
    UPDATE_STATUS_CERTS_LOADED          = (1 << STATUS_CERTS_LOADED),
    UPDATE_STATUS_AIRPLANE_MODE         = (1 << STATUS_AIRPLANE_MODE),
    UPDATE_STATUS_MQTT_ENABLED          = (1 << STATUS_MQTT_ENABLED),
    UPDATE_STATUS_GPS_ENABLED           = (1 << STATUS_GPS_ENABLED),
    UPDATE_STATUS_CELL_DATA_CHANGED     = (1 << STATUS_CELL_DATA_CHANGED),
    UPDATE_STATUS_CELL_TRACKING_CHANGED = (1 << STATUS_CELL_TRACKING_CHANGED),
    UPDATE_STATUS_BIT15                 = (1 << STATUS_BIT15),
    UPDATE_STATUS_RSSI                  = (1 << 16),
    UPDATE_STATUS_FOTA_PERCENTAGE       = (1 << 17),
    UPDATE_STATUS_FOTA_STATE            = (1 << 18)
};


enum modem_error_codes
{
    MODEM_ERROR_NONE          = 0,
    MODEM_ERROR_UNKNOWN       = 1,
    MODEM_ERROR_GETADDR_ERROR = 2,
    MODEM_ACCESS_DENIED       = 3,
};