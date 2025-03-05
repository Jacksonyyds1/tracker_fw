/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */
#include "d1_zbus.h"
#include "modem_interface_types.h"
#include <net/mqtt_helper.h>
#include <zephyr/zbus/zbus.h>


ZBUS_CHAN_DEFINE(
    MQTT_DEV_TO_CLOUD_MESSAGE,
    mqtt_payload_t,
    NULL,
    NULL,
    ZBUS_OBSERVERS(comm_mgr_sub),
    ZBUS_MSG_INIT(.qos = MQTT_QOS_0_AT_MOST_ONCE, .topic_length = 0, .payload_length = 0, .topic = NULL, .payload = NULL));

ZBUS_CHAN_DEFINE(
    MQTT_CLOUD_TO_DEV_MESSAGE,
    mqtt_payload_t,
    NULL,
    NULL,
    ZBUS_OBSERVERS(comm_mgr_sub),
    ZBUS_MSG_INIT(.qos = MQTT_QOS_0_AT_MOST_ONCE, .topic_length = 0, .payload_length = 0, .topic = NULL, .payload = NULL));

// general 9160 status bits, connections, etc
ZBUS_CHAN_DEFINE(
    LTE_STATUS_UPDATE, modem_status_update_t, NULL, NULL, ZBUS_OBSERVERS(comm_mgr_sub, radio_mgr_sub), ZBUS_MSG_INIT(0));

// fota state messages from the 9160 to be processed and sent to the cloud
ZBUS_CHAN_DEFINE(
    FOTA_STATE_UPDATE,
    fota_status_t,
    NULL,
    NULL,
    ZBUS_OBSERVERS(comm_mgr_sub),
    ZBUS_MSG_INIT(.status = 0, .percentage = 0, .device_type = COMM_DEVICE_NONE));

//
ZBUS_CHAN_DEFINE(BATTERY_PERCENTAGE_UPDATE, float, NULL, NULL, ZBUS_OBSERVERS(comm_mgr_sub), ZBUS_MSG_INIT(0.0f));


ZBUS_CHAN_DEFINE(
    USB_POWER_STATE_UPDATE, bool, NULL, NULL, ZBUS_OBSERVERS(comm_mgr_sub, radio_mgr_sub), ZBUS_MSG_INIT(false));


ZBUS_CHAN_DEFINE(BT_CONN_STATE_UPDATE, bool, NULL, NULL, ZBUS_OBSERVERS(radio_mgr_sub), ZBUS_MSG_INIT(false));