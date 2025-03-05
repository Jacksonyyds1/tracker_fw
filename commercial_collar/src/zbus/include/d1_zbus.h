/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */
#pragma once
#include <zephyr/zbus/zbus.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "d1_json.h"
#include "modem_interface_types.h"

typedef struct
{
    char    *topic;
    uint16_t topic_length;
    char    *payload;
    uint16_t payload_length;
    uint8_t  qos;
    uint8_t  radio;
    bool     send_immidiately;
    void    *user_data;
} mqtt_payload_t;

typedef struct
{
    int32_t            status;
    int8_t             percentage;
    comm_device_type_t device_type;
} fota_status_t;

typedef struct
{
    uint32_t       change_bits;
    modem_status_t status;
} modem_status_update_t;

ZBUS_CHAN_DECLARE(
    MQTT_DEV_TO_CLOUD_MESSAGE,
    MQTT_CLOUD_TO_DEV_MESSAGE,
    LTE_STATUS_UPDATE,
    FOTA_STATE_UPDATE,
    BATTERY_PERCENTAGE_UPDATE,
    USB_POWER_STATE_UPDATE,
    BT_CONN_STATE_UPDATE);
