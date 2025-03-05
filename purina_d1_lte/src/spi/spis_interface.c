#include "spis_interface.h"
#include <stdlib.h>
#include <zephyr/kernel.h>
#include "nrfx_spis.h"
#include "nrfx_gpiote.h"
#include <zephyr/sys/printk.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/irq.h>
#include <zephyr/logging/log.h>
#include <math.h>
#include <nrf_modem_at.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/zbus/zbus.h>

#include "app_version.h"
#include "status.h"
#include "d1_gps.h"
#include <zephyr/pm/pm.h>
#include <zephyr/pm/device.h>
#include <zephyr/pm/policy.h>

#include <zephyr/sys/poweroff.h>
#include <zephyr/sys/crc.h>

#include "transport.h"
#include "zbus_msgs.h"
#include "fota.h"
#include "modem_interface_types.h"  // from c_modules/modem/include so its shared with the 5340
#include "network.h"
#include "wi.h"

LOG_MODULE_REGISTER(spis, CONFIG_PURINA_D1_SPIS_LOG_LEVEL); 

extern bool disable_modem;

void spis_zbus_mgr_listener(const struct zbus_channel *chan);
ZBUS_LISTENER_DEFINE(spis_sub, spis_zbus_mgr_listener);

extern struct zbus_channel incoming_mqtt_msg_chan;


message_command_v1_t default_response = {
    .version = 0x01,
    .messageType = MESSAGE_TYPE_DEVICE_STATUS,
    .messageHandle = 255,
    .dataLen = 0,
    .chunkNum = 0,
    .chunkTotal = 1
};

static bool spis_initialized = false;
static bool spis_first_msg_received = false;

#define SPIS_INSTANCE 3 /**< SPIS instance index. */
static const nrfx_spis_t spis =
    NRFX_SPIS_INSTANCE(SPIS_INSTANCE); /**< SPIS instance. */

#define APP_SPIS_CS_PIN 13
#define APP_SPIS_SCK_PIN 16
#define APP_SPIS_MISO_PIN 5
#define APP_SPIS_MOSI_PIN 15
// #define RX_BUF_SIZE 2048
// #define TX_BUF_SIZE 1024
static const struct gpio_dt_spec DataReady = GPIO_DT_SPEC_GET(DT_ALIAS(dataready), gpios);

#define MAX_RESPONSES 8

#define MAX_INCOMING_BUFS MAX_RESPONSES
struct incoming_data_item_t {
    bool in_use;
    uint8_t data[CONFIG_PURINA_D1_SPIS_RX_BUFFER_SIZE];
    uint16_t len;
} __attribute__((__packed__));

typedef struct {
    workref_t *work;
    void* data;
    uint16_t len;
} incoming_data_item_t;

typedef struct {
    void* reserved;
    uint8_t messageType;
    uint8_t messageHandle;
    uint8_t* data;
    int8_t simpleData;
    uint16_t dataLen;
    uint8_t chunkNum;
    uint8_t chunkTotal;
} outgoing_data_item_t;
K_FIFO_DEFINE(outgoing_fifo);
bool spi_buffer_is_generic_or_empty = true;


static nrfx_spis_config_t spis_config =
    NRFX_SPIS_DEFAULT_CONFIG(APP_SPIS_SCK_PIN, APP_SPIS_MOSI_PIN, APP_SPIS_MISO_PIN, APP_SPIS_CS_PIN);
static uint8_t m_rx_buf[CONFIG_PURINA_D1_SPIS_RX_BUFFER_SIZE]; // RX buffer.

int64_t last_ssids_received_ts=0;
void set_next_response();

modem_status_t last_modem_status;
uint64_t       time_last_message_sent  = 0;
uint64_t       time_last_message_recvd = 0;

volatile bool spis_actual_response_tx_done = true;
volatile uint16_t spis_actual_response_tx_len = 0;

message_command_v1_t important_response = {
    .version = 0x01,
    .messageType = MESSAGE_TYPE_DEVICE_STATUS,
    .messageHandle = 255,
    .dataLen = 0,
    .chunkNum = 0,
    .chunkTotal = 1    
};
static uint8_t m_important_tx_buf[CONFIG_PURINA_D1_SPIS_TX_BUFFER_SIZE + sizeof(message_command_v1_t)]; // TX buffer. 
static uint16_t m_important_tx_buf_size = CONFIG_PURINA_D1_SPIS_TX_BUFFER_SIZE + sizeof(message_command_v1_t);


struct k_work_q spi_recv_work_q;
K_THREAD_STACK_DEFINE(spi_stack_area, 8192);

void prepare_response(uint8_t type, uint8_t handle, uint8_t* data, uint16_t dataLen);


///////////////////////////////
/// 
///     spis_zbus_msg_cb
/// 
void spis_send_mqtt(inc_mqtt_event_t msg) {
    log_panic();
    //LOG_DBG("new mqtt message for 5340 - %.*s\n", msg.msg_length, msg.mqtt_msg);
    if (msg.msg_length > m_important_tx_buf_size - sizeof(message_command_v1_t)) {
        LOG_ERR("msg.msg_length(%d) > m_important_tx_buf_size - sizeof(message_command_v1_t)", msg.msg_length);
        if(msg.mqtt_msg) k_free(msg.mqtt_msg);
        if(msg.topic) k_free(msg.topic);
        return;
    }
    char* response_data = k_malloc(msg.msg_length + msg.topic_length + 8 + 8);  // msg size + topic size + 8 for 'msg size' + 8 for topic size
    if (response_data == NULL) {
        LOG_ERR("response_data - malloc failed");
        SEND_FATAL_ERROR();
    }
    memcpy(response_data, (uint16_t*)(&(msg.topic_length)), sizeof(uint16_t));
    memcpy(response_data + sizeof(uint16_t), (uint16_t*)(&(msg.msg_length)), sizeof(uint16_t));
    memcpy(response_data + sizeof(uint16_t) + sizeof(uint16_t), msg.topic, msg.topic_length);
    memcpy(response_data + sizeof(uint16_t) + sizeof(uint16_t) + msg.topic_length, msg.mqtt_msg, msg.msg_length);
    prepare_response(MESSAGE_TYPE_MQTT, 255, response_data, msg.msg_length + msg.topic_length + 8 + 8);
    if (response_data) k_free(response_data);
    if(msg.mqtt_msg) k_free(msg.mqtt_msg);
    if(msg.topic) k_free(msg.topic);
}


///////////////////////////////
/// 
///     prepare_basic_response_simple
/// 
void prepare_basic_response_simple(uint8_t handle, int8_t data) {
    if (time_last_message_recvd < time_last_message_sent) {
        if (k_uptime_get() - time_last_message_sent > 1000) {
            LOG_ERR("seems like the 5340 is not responding, not sending message");
            return;
        }
    }
    outgoing_data_item_t* outgoing_data_item = k_malloc(sizeof(outgoing_data_item_t));
    if (outgoing_data_item == NULL) {
        LOG_ERR("outgoing_data_item - malloc failed");
        SEND_FATAL_ERROR();
        return;
    }

    outgoing_data_item->messageType = MESSAGE_TYPE_RESPONSE;
    outgoing_data_item->messageHandle = handle;
    outgoing_data_item->data = NULL;
    outgoing_data_item->simpleData = data;
    outgoing_data_item->dataLen = 1;
    outgoing_data_item->chunkNum = 0;
    outgoing_data_item->chunkTotal = 1;
    k_fifo_put(&outgoing_fifo, outgoing_data_item);

    set_next_response();
}

///////////////////////////////
/// 
///     prepare_basic_response
/// 
void prepare_basic_response(uint8_t handle, uint8_t* data, uint16_t dataLen) {
    prepare_response(MESSAGE_TYPE_RESPONSE, handle, data, dataLen);
}


///////////////////////////////
/// 
///     prepare_response
/// 
void prepare_response(uint8_t type, uint8_t handle, uint8_t* data, uint16_t dataLen) {
    if (time_last_message_recvd < time_last_message_sent) {
        if (k_uptime_get() - time_last_message_sent > 1000) {
            LOG_ERR("seems like the 5340 is not responding, not sending message");
            return;
        }
    }
    outgoing_data_item_t* outgoing_data_item = k_malloc(sizeof(outgoing_data_item_t));
    if (outgoing_data_item == NULL) {
        LOG_ERR("outgoing_data_item - malloc failed");
        SEND_FATAL_ERROR();
        return;
    }
    if (dataLen > CONFIG_PURINA_D1_SPIS_TX_BUFFER_SIZE) {
        LOG_ERR("dataLen > CONFIG_PURINA_D1_SPIS_TX_BUFFER_SIZE");
        return;
    }
    uint8_t* data_ptr = NULL;
    if ((dataLen > 0) && (data != NULL)) {
        data_ptr = (uint8_t*)k_malloc(dataLen);
        if (data_ptr == NULL) {
            LOG_ERR("data_ptr - malloc failed");
            SEND_FATAL_ERROR();
            return;
        }
        memcpy(data_ptr, data, dataLen);
    }

    outgoing_data_item->messageType = type;
    outgoing_data_item->messageHandle = handle;
    outgoing_data_item->data = data_ptr;
    outgoing_data_item->dataLen = dataLen;
    outgoing_data_item->chunkNum = 0;
    outgoing_data_item->chunkTotal = 1;
    outgoing_data_item->simpleData = 0;
    k_fifo_put(&outgoing_fifo, outgoing_data_item);

    set_next_response();
}



///////////////////////////////
/// 
///     set_next_response
/// 
void set_next_response() {
    if (spis_initialized == false) {  // || spis_first_msg_received == false
        LOG_DBG("spis not initialized or first msg not received");
        return;
    }
    if (k_fifo_is_empty(&outgoing_fifo) && spi_buffer_is_generic_or_empty) {
        modem_status_t* currStatus = &last_modem_status;
        // one change to the status is to set the uptime again
        // other wise use th last status
        currStatus->uptime = k_uptime_get();
        
        default_response.dataLen = sizeof(modem_status_t);
        memcpy(m_important_tx_buf, (uint8_t*)(&default_response), sizeof(message_command_v1_t));
        memcpy(m_important_tx_buf + sizeof(message_command_v1_t), (uint8_t*)(currStatus), sizeof(modem_status_t));
        spis_actual_response_tx_len = sizeof(message_command_v1_t) + sizeof(modem_status_t);
        nrfx_spis_buffers_set(&spis, ((uint8_t*)m_important_tx_buf),
            sizeof(message_command_v1_t) + sizeof(modem_status_t), m_rx_buf,
            CONFIG_PURINA_D1_SPIS_RX_BUFFER_SIZE); 
        spi_buffer_is_generic_or_empty = true;
    }
    else {
        if(!k_fifo_is_empty(&outgoing_fifo)) {
            if (spi_buffer_is_generic_or_empty == false) {
                LOG_DBG("waiting for last message to finish sending over spi");
                return;
            }
            outgoing_data_item_t* outgoing_data_item = k_fifo_get(&outgoing_fifo, K_NO_WAIT);
            if (outgoing_data_item == NULL) {
                return;
            }

            if ((outgoing_data_item->dataLen + sizeof(message_command_v1_t)) > m_important_tx_buf_size) {
                LOG_ERR("dataLen > m_important_tx_buf_size: %d > %d", outgoing_data_item->dataLen, m_important_tx_buf_size);
                //SEND_FATAL_ERROR();
                if (outgoing_data_item->data != NULL) {
                    k_free(outgoing_data_item->data);
                }
                if (outgoing_data_item) k_free(outgoing_data_item);
                spi_buffer_is_generic_or_empty = true;
                return;
            }

            important_response.version = 0x01;
            important_response.messageType = outgoing_data_item->messageType;
            important_response.messageHandle = outgoing_data_item->messageHandle;
            important_response.dataLen = outgoing_data_item->dataLen;
            important_response.chunkNum = outgoing_data_item->chunkNum;
            important_response.chunkTotal = outgoing_data_item->chunkTotal;

            memcpy(m_important_tx_buf, (uint8_t*)(&important_response), sizeof(message_command_v1_t));
            if (outgoing_data_item->data == NULL) {
                uint8_t* data = ((uint8_t*)m_important_tx_buf + sizeof(message_command_v1_t));
                data[0] = outgoing_data_item->simpleData;
            }
            else {
                memcpy(m_important_tx_buf + sizeof(message_command_v1_t), outgoing_data_item->data, outgoing_data_item->dataLen);
            }

            spis_actual_response_tx_len = sizeof(message_command_v1_t) + outgoing_data_item->dataLen;

            nrfx_spis_buffers_set(&spis, ((uint8_t*)m_important_tx_buf),
                spis_actual_response_tx_len, m_rx_buf,
                CONFIG_PURINA_D1_SPIS_RX_BUFFER_SIZE);
            gpio_pin_set(DataReady.port, DataReady.pin, 1);
            time_last_message_sent         = k_uptime_get();
            spi_buffer_is_generic_or_empty = false;
            if (outgoing_data_item->data != NULL) {
                k_free(outgoing_data_item->data);
            }
            if (outgoing_data_item) k_free(outgoing_data_item);
        }
    }
}



///////////////////////////////
/// 
///     nrf5340_recv_callback
/// 
void nrf5340_recv_callback(uint8_t *data, size_t len, void *user_data) {
    //uint8_t simple_response_data[1];

        if (len < sizeof(message_command_v1_t)) {
                //LOG_DBG("message wrong len = %d/%d\n", len, sizeof(message_command_v1_t));
                return;
        }

        message_command_v1_t *msg = (message_command_v1_t *)data;
        if (msg->version == 0x01) {
            // LOG_DBG("nrf5340_recv_callback  dataLen = %d\n", msg->dataLen);
            // LOG_DBG("nrf5340_recv_callback  spi interrupt data recvd size = %d\n",spis_rx_buff_recvd_len);
            // LOG_DBG("nrf5340_recv_callback  spi interrupt data sent size = %d\n",spis_tx_buff_sent_len);
            // LOG_DBG("nrf5340_recv_callback  handle = %d\n", msg->messageHandle);
            // LOG_DBG("nrf5340_recv_callback  msg data size = %d\n", msg->dataLen);

            switch (msg->messageType) {
                    case MESSAGE_TYPE_MQTT:
                            LOG_DBG("MESSAGE_TYPE_MQTT");
                            spi_mqtt_t* mqtt_msg = (spi_mqtt_t*)(data + sizeof(message_command_v1_t));
                            uint8_t ret = 0; 
                            bool err = false;

                            if (mqtt_msg->topic_length > (CONFIG_PURINA_D1_LTE_JSON_TOPIC_SIZE_MAX - 1) ) {
                                LOG_ERR("mqtt_msg->topic_length > CONFIG_PURINA_D1_LTE_JSON_TOPIC_SIZE_MAX: %d", mqtt_msg->topic_length);
                                ret = 0x01;  // return 0 for failure 
                                goto error_mqtt;
                            }
                            if (mqtt_msg->msg_length > (CONFIG_PURINA_D1_LTE_JSON_MESSAGE_SIZE_MAX - 1)) {
                                LOG_ERR("mqtt_msg->msg_length > CONFIG_PURINA_D1_LTE_JSON_MESSAGE_SIZE_MAX: %d", mqtt_msg->msg_length);
                                ret = 0x01;  // return 0 for failure 
                                goto error_mqtt;
                            }

                            mqtt_payload_t payload;
                            payload.qos = mqtt_msg->qos;

                            payload.topic = k_malloc(mqtt_msg->topic_length + 1);  // +1 for the null terminator
                            if (payload.topic == NULL) {
                                LOG_ERR("payload.topic - malloc failed");
                                SEND_FATAL_ERROR();
                            }
                            payload.topic[0] = 0;
                            strncat(payload.topic, data + sizeof(message_command_v1_t) + sizeof(spi_mqtt_t), MIN((mqtt_msg->topic_length), (CONFIG_PURINA_D1_LTE_JSON_TOPIC_SIZE_MAX - 1)));


                            payload.string = k_malloc(mqtt_msg->msg_length + 1);  // +1 for the null terminator
                            if (payload.string == NULL) {
                                LOG_ERR("payload.string - malloc failed");
                                SEND_FATAL_ERROR();
                            }
                            payload.string[0] = 0;
                            strncat(payload.string, data + sizeof(message_command_v1_t) + sizeof(spi_mqtt_t) + mqtt_msg->topic_length, (mqtt_msg->msg_length));


                            payload.topic_length = mqtt_msg->topic_length;
                            payload.string_length = mqtt_msg->msg_length;
                            
                            LOG_DBG("new MQTT message from 5340(%d) (topic)- %s", payload.topic_length, payload.topic);
                            LOG_DBG("new MQTT message from 5340 (qos) - %d", mqtt_msg->qos);

                            payload.msgHandle = msg->messageHandle;
                            err = zbus_chan_pub(&MQTT_DEV_TO_CLOUD_MESSAGE, &payload, K_SECONDS(10));
                                if (err) {
                                    LOG_ERR("zbus_chan_pub, error:%d", err);
                                    //SEND_FATAL_ERROR();
                                    k_free(payload.string);
                                    k_free(payload.topic);
                                }
                            return;
                            error_mqtt:
                                prepare_basic_response_simple(msg->messageHandle, ret);
                            break;
                    case MESSAGE_TYPE_RESPONSE:
                            LOG_DBG("MESSAGE_TYPE_RESPONSE, that's odd");
                            ret = 0x01;  // return 1 for error
                            prepare_basic_response_simple(msg->messageHandle, ret);
                            break;
                    case MESSAGE_TYPE_COMMAND:
                            LOG_DBG("MESSAGE_TYPE_COMMAND");

                            // get specific command
                            command_type_t modem_command = *((command_type_t*)(data + sizeof(message_command_v1_t)));
                            switch (modem_command) {
                                case COMMAND_NO_OP:
                                        LOG_DBG("COMMAND_NO_OP");
                                        ret = 0x00;  // return 0 for success, its null/no-op, I guess its always a success
                                        prepare_basic_response_simple(msg->messageHandle, ret);
                                        break;
                                case COMMAND_REBOOT:
                                        LOG_DBG("COMMAND_REBOOT");
                                        if (config_get_fota_in_progress()) {
                                            // not shutting down now, return error
                                            prepare_basic_response_simple(msg->messageHandle, 1);
                                            break;
                                        }
                                        config_set_powered_off(true);  
                                        prepare_basic_response_simple(msg->messageHandle, 0);
                                        k_sleep(K_SECONDS(2));  // give 5340 time to get the message
                                        SEND_SYS_REBOOT();
                                        break;
                                case COMMAND_START:
                                        LOG_DBG("COMMAND_START");
                                        disable_modem = false;
                                        config_set_lte_enabled(true);
                                        ret = 0x00;  // return 0 for success, its null/no-op, I guess its always a success
                                        prepare_basic_response_simple(msg->messageHandle, ret);
                                        break;
                                case COMMAND_NULL:
                                        LOG_DBG("COMMAND_NULL");
                                        ret = 0x00;  // return 0 for success, its null/no-op, I guess its always a success
                                        prepare_basic_response_simple(msg->messageHandle, ret);
                                        break;
                                case COMMAND_HARD_STOP:
                                        LOG_DBG("COMMAND_HARD_STOP");
                                        if (config_get_fota_in_progress()) {
                                            // not shutting down now, return error
                                            prepare_basic_response_simple(msg->messageHandle, 1);
                                            break;
                                        }
                                        config_set_powered_off(true);  
                                        prepare_basic_response_simple(msg->messageHandle, 0);
                                        k_sleep(K_SECONDS(2));  // give 5340 time to get the message
                                        SEND_SYS_SHUTDOWN();
                                        break;
                                case COMMAND_GET_VERSION:
                                        LOG_DBG("COMMAND_GET_VERSION");

                                        version_response_t version_response = {
                                            .major = APP_VERSION_MAJOR,
                                            .minor = APP_VERSION_MINOR,
                                            .patch = APP_VERSION_PATCH,
                                            .githash = GIT_HASH,
                                            .build_date = DBUILD_DATE,
                                            .build_machine = DBUILD_MACHINE
                                        };
                                        memcpy(version_response.modem_fw, getModemFWVersion(), 32);
                                        char* response_data = k_malloc(sizeof(version_response_t) + 1);  // plus 1 for the command type enum
                                        if (response_data == NULL) {
                                            LOG_ERR("response_data - malloc failed");
                                            SEND_FATAL_ERROR();
                                        }
                                        response_data[0] = COMMAND_GET_VERSION;  // return 0 for success, its null/no-op, I guess its always a success
                                        memcpy(response_data + 1, (uint8_t*)(&version_response), sizeof(version_response_t));
                                        prepare_response(MESSAGE_TYPE_COMMAND_RESP, msg->messageHandle, response_data, sizeof(version_response_t) + 1);
                                        k_free(response_data);
                                        break;
                                case COMMAND_SET_MQTT_PARAMS:
                                        LOG_DBG("COMMAND_SET_MQTT_PARAMS");
                                        char* mqtt_params = (char*)(data + sizeof(message_command_v1_t));
                                        mqtt_params[msg->dataLen] = 0; //Terminate the string
                                        LOG_DBG("recieved mqtt_params = %s", mqtt_params);
                                        transport_set_settings(mqtt_params);
                                        ret = 0x00;  // return 0 for success, its null/no-op, I guess its always a success
                                        prepare_basic_response_simple(msg->messageHandle, ret);
                                        break;
                                case COMMAND_SET_MQTT_SUBSCRIPTIONS:
                                        LOG_DBG("COMMAND_SET_MQTT_SUBSCRIPTIONS");
                                        char* mqtt_subs = (char*)(data + sizeof(message_command_v1_t));
                                        mqtt_subs[msg->dataLen] = 0; //Terminate the string
                                        LOG_DBG("recieved mqtt_subs = %s", mqtt_subs);
                                        transport_set_subscription_topics(mqtt_subs);
                                        ret = 0x00;  // return 0 for success, its null/no-op, I guess its always a success
                                        prepare_basic_response_simple(msg->messageHandle, ret);
                                        break;  
                                case COMMAND_SET_MQTT_CONNECT:
                                        LOG_DBG("COMMAND_SET_MQTT_CONNECT");
                                        ret = 0x00;  // return 0 for success, its null/no-op, I guess its always a success
                                        transport_allow_mqtt_connect(true);
                                        prepare_basic_response_simple(msg->messageHandle, ret);
                                        break;
                                case COMMAND_SET_MQTT_DISCONNECT:
                                        LOG_DBG("COMMAND_SET_MQTT_DISCONNECT");
                                        ret = 0x00;  // return 0 for success, its null/no-op, I guess its always a success
                                        transport_allow_mqtt_connect(false);
                                        prepare_basic_response_simple(msg->messageHandle, ret);
                                        break;
                                case COMMAND_SET_FOTA_CANCEL:
                                        LOG_DBG("COMMAND_SET_FOTA_CANCEL");
                                        ret = 0x00;  // return 0 for success, its null/no-op, I guess its always a success
                                        fotaCancel();
                                        prepare_basic_response_simple(msg->messageHandle, ret);
                                        break;
                                case COMMAND_GPS_START:
                                        uint8_t* gps_enabled = (uint8_t*)(data + sizeof(message_command_v1_t) + 1);
                                        uint8_t* gps_sample_period = (uint8_t*)(data + sizeof(message_command_v1_t) + 2);
                                        gps_settings_t gps_settings;
                                        if (*gps_enabled == 0) {
                                            gps_settings.gps_enable = 0;
                                            gps_settings.gps_fakedata_enable = 0;
                                        }
                                        else if (*gps_enabled == 1){
                                            gps_settings.gps_enable = 1;
                                            gps_settings.gps_fakedata_enable = 0;
                                        }
                                        else if (*gps_enabled == 2){
                                            gps_settings.gps_enable = 1;
                                            gps_settings.gps_fakedata_enable = 1;
                                        }
                                        else if (*gps_enabled == 3){
                                            gps_settings.gps_enable = 1;
                                            gps_settings.gps_fakedata_enable = 0;
                                        }
                                        gps_settings.gps_sample_period = *gps_sample_period;
                                        LOG_DBG("COMMAND_GPS_START - %d", *gps_enabled);
                                        err = zbus_chan_pub(&GPS_STATE_CHANNEL, &gps_settings, K_SECONDS(1));
                                        if (err) {
                                            LOG_ERR("zbus_chan_pub, error:%d", err);
                                            SEND_FATAL_ERROR();
                                        }
                                        ret = 0x00;  // return 0 for success, its null/no-op, I guess its always a success
                                        prepare_basic_response_simple(msg->messageHandle, ret);
                                        break;
                                case COMMAND_GPS_STOP:
                                        LOG_DBG("COMMAND_GPS_STOP");
                                        int gps_disabled = 0;
                                        err = zbus_chan_pub(&GPS_STATE_CHANNEL, &gps_disabled, K_SECONDS(1));
                                        if (err) {
                                            LOG_ERR("zbus_chan_pub, error:%d", err);
                                            SEND_FATAL_ERROR();
                                        }                                        
                                        ret = 0x00;  // return 0 for success, its null/no-op, I guess its always a success
                                        prepare_basic_response_simple(msg->messageHandle, ret);
                                        break;
                                case COMMAND_SET_PAGE_CYCLE:
                                        LOG_DBG("COMMAND_SET_PAGE_CYCLE");
                                        ret = 0x00;  // return 0 for success, its null/no-op, I guess its always a success
                                        prepare_basic_response_simple(msg->messageHandle, ret);
                                        break;  
                                case COMMAND_SET_AIRPLANE_MODE:
                                        LOG_DBG("COMMAND_SET_AIRPLANE_MODE");
                                        uint8_t* airplane_mode = (uint8_t*)(data + sizeof(message_command_v1_t) + 1);
                                        LOG_DBG("recieved airplane_mode = %d", *airplane_mode);
                                        char response[1024];
                                        int apmode_err;
                                        int ap_mode_zbus_flag;
                                        if (*airplane_mode == 0) {
                                            apmode_err = nrf_modem_at_cmd(response, sizeof(response), "%s", "AT+CFUN=1");
                                            ap_mode_zbus_flag = NETWORK_AIRPLANE_MODE_OFF;
                                        }
                                        else {
                                            apmode_err = nrf_modem_at_cmd(response, sizeof(response), "%s", "AT+CFUN=4");
                                            ap_mode_zbus_flag = NETWORK_AIRPLANE_MODE_ON;
                                        }
                                        if (apmode_err < 0) {
                                                LOG_DBG("error with nrf_modem_at_cmd");
                                                strcpy(response, "ERROR\0");
                                        }

                                        err = zbus_chan_pub(&NETWORK_CHAN, &ap_mode_zbus_flag, K_SECONDS(1));
                                        if (err) {
                                            LOG_ERR("zbus_chan_pub, error:%d", err);
                                            SEND_FATAL_ERROR();
                                        }
                                        ret = 0x00;  // return 0 for success, its null/no-op, I guess its always a success
                                        prepare_basic_response_simple(msg->messageHandle, ret);
                                        break;
                                case COMMAND_SET_DNS:
                                        LOG_DBG("COMMAND_SET_DNS");
                                        char dns_str[64] = {0};
                                        char* dns = (char*)(data + sizeof(message_command_v1_t) + 1);
                                        strncpy(dns_str, dns, MIN(64, (msg->dataLen - 1)));
                                        LOG_INF("setting dns to: (%d) %s", strlen(dns_str), dns_str);
                                        ret = 0;
                                        if (modem_setdnsaddr(dns_str) == 0) {
                                            LOG_INF("DNS address set");
                                            ret = 0;
                                        } else {
                                            LOG_ERR("Failed to set DNS address");
                                            ret = 1;
                                        }
                                        prepare_basic_response_simple(msg->messageHandle, ret);
                                        break;
                                case COMMAND_PING:
                                        LOG_DBG("COMMAND_PING");
                                        ret = 0x00;  
                                        // TODO: implement ping
                                        prepare_basic_response_simple(msg->messageHandle, ret);
                                        break;
                                default:
                                        LOG_DBG("COMMAND_UNKNOWN");
                                        ret = 0x01;  // return 0 for error since we dont do this YET
                                        prepare_basic_response_simple(msg->messageHandle, ret);
                                        break;
                            }
                            break;                 
                    case MESSAGE_TYPE_AT:
                            LOG_DBG("MESSAGE_TYPE_AT");
                            char* atCommand = (char*)(data + sizeof(message_command_v1_t));
                            atCommand[msg->dataLen] = 0; //Terminate the string
                            int at_err;

                            char response[1024];
                            LOG_WRN("recieved atCommand = %s", atCommand);
                            at_err = nrf_modem_at_cmd(response, sizeof(response), "%s", atCommand);
                            LOG_WRN("recieved atCommand response = %s", response);
                            if (at_err < 0) {
                                    LOG_DBG("error with nrf_modem_at_cmd");
                                    strcpy(response, "ERROR\0");
                            }

                            prepare_basic_response(msg->messageHandle, response, strlen(response));
                            break;
                    case MESSAGE_TYPE_NULL:
                            LOG_DBG("MESSAGE_TYPE_NULL");
                            ret = 0x00;  // return 0 for success, its null, I guess its always a success
                            prepare_basic_response_simple(msg->messageHandle, ret);
                            break;
                    case MESSAGE_TYPE_DEVICE_STATUS:
                            //LOG_DBG("MESSAGE_TYPE_DEVICE_STATUS");
                            modem_status_t* status = &last_modem_status;
                            //printStatus(status);
                            print_wr_stats();
                            prepare_response(MESSAGE_TYPE_DEVICE_STATUS, msg->messageHandle, (uint8_t*)status, sizeof(modem_status_t));
                            break;
                    case MESSAGE_TYPE_DEVICE_INFO:
                            LOG_DBG("MESSAGE_TYPE_DEVICE_INFO");
                            modem_info_t info = getModemInfo();
                            prepare_response(MESSAGE_TYPE_DEVICE_INFO, msg->messageHandle, (uint8_t*)&info, sizeof(info));
                            break;
                    case MESSAGE_TYPE_NO_OP:
                            LOG_DBG("MESSAGE_TYPE_NO_OP");
                            break;
                    case MESSAGE_TYPE_CELL_INFO:
                            LOG_DBG("MESSAGE_TYPE_CELL_INFO");
                            cell_info_t *cell_info = getCellInfo();
                            prepare_response(MESSAGE_TYPE_CELL_INFO, msg->messageHandle, (uint8_t*)&cell_info, sizeof(cell_info_t));
                            break;
                    case MESSAGE_TYPE_DEVICE_PING:
                            // not currrently handled
                            break;
                    case MESSAGE_TYPE_FOTA_FROM_HTTPS:
                            LOG_DBG("MESSAGE_TYPE_FOTA_FROM_HTTPS");
                            if (!config_get_lte_connected()) {
                                LOG_ERR("LTE not connected, cannot download");
                                ret = 0x01; // return 1 for error
                                prepare_basic_response_simple(msg->messageHandle, ret);
                                break;
                            }
                            char* url = (char*)(data + sizeof(message_command_v1_t));
                            url[msg->dataLen] = 0; //Terminate the string
                            LOG_DBG("recieved FOTA url = %s", url);
                            download_request_t dl_request;
                            dl_request.download_handle = msg->messageHandle;
                            memset(dl_request.download_url, 0, CONFIG_PURINA_D1_LTE_DOWNLOAD_URL_SIZE_MAX);
                            strncpy(dl_request.download_url, url, strlen(url));
                            err = zbus_chan_pub(&FOTA_DL_CHANNEL, &dl_request, K_SECONDS(1));
                            if (err) {
                                LOG_ERR("zbus_chan_pub, error:%d", err);
                                //SEND_FATAL_ERROR();
                            }
                            //handleFotaDownloadHTTPsMessage(url, CONFIG_PURINA_D1_DOWNLOAD_SECURITY_TAG); //CONFIG_PURINA_D1_DOWNLOAD_SECURITY_TAG, or -1
                            //fotaSendCheckForUpdate();
                            break;
                    case MESSAGE_TYPE_DOWNLOAD_FROM_HTTPS:
                            LOG_DBG("MESSAGE_TYPE_DOWNLOAD_FROM_HTTPS");
                            if (!config_get_lte_connected()) {
                                LOG_ERR("LTE not connected, cannot download");
                                ret = 0x01; // return 1 for error
                                prepare_basic_response_simple(msg->messageHandle, ret);
                                break;
                            }
                            char* dl_url = (char*)(data + sizeof(message_command_v1_t));
                            dl_url[msg->dataLen] = 0; //Terminate the string
                            LOG_DBG("recieved download url = %s", dl_url);
                            if (strlen(dl_url) > CONFIG_PURINA_D1_LTE_DOWNLOAD_URL_SIZE_MAX) {
                                LOG_ERR("dl_url > CONFIG_PURINA_D1_LTE_DOWNLOAD_URL_SIZE_MAX: %d", strlen(dl_url));
                                ret = 0x00;
                                prepare_basic_response_simple(msg->messageHandle, ret);
                                break;
                            }
                            download_request_t my_dl_request;
                            my_dl_request.download_handle = msg->messageHandle;
                            memset(my_dl_request.download_url, 0, CONFIG_PURINA_D1_LTE_DOWNLOAD_URL_SIZE_MAX);
                            strncpy(my_dl_request.download_url, dl_url, strlen(dl_url));
                            err = zbus_chan_pub(&DOWNLOAD_REQUEST_CHANNEL, &my_dl_request, K_SECONDS(1));
                            if (err) {
                                LOG_ERR("zbus_chan_pub, error:%d", err);
                                SEND_FATAL_ERROR();
                            }
                            break;
                    case MESSAGE_TYPE_GET_TIME:
                            LOG_DBG("MESSAGE_TYPE_GET_TIME");
                            char* time;
                            if ((time = getStatusTime()) == NULL) {
                                ret = 0x01; // return 1 for error
                                prepare_basic_response_simple(msg->messageHandle, ret);
                                break;
                            }
                            prepare_response(MESSAGE_TYPE_COMMAND_RESP, msg->messageHandle, time, 32);
                            break;
                    case MESSAGE_TYPE_FW_UPLOAD:
                            LOG_DBG("MESSAGE_TYPE_FW_UPLOAD");
                            firmware_upload_t* my_fw_upload = (firmware_upload_t*)(data + sizeof(message_command_v1_t));
                            uint8_t* fw_data = (uint8_t*)(data + sizeof(message_command_v1_t) + sizeof(firmware_upload_t));
                            uint16_t fw_data_len = my_fw_upload->data_len;
                            //LOG_DBG("recieved fw_data_len = %d", fw_data_len);
                            if (fw_data_len > CONFIG_PURINA_D1_LTE_FOTA_CHUNK_SIZE_MAX) {
                                LOG_ERR("fw_data_len > CONFIG_PURINA_D1_LTE_FOTA_CHUNK_SIZE_MAX: %d", fw_data_len);
                                firmware_upload_t reply = {
                                    .chunk_num = my_fw_upload->chunk_num,
                                    .chunk_total = my_fw_upload->chunk_total,
                                    .crc = my_fw_upload->crc,
                                    .data_len = 0,
                                    .return_code = 0x02
                                };
                                prepare_response(MESSAGE_TYPE_FW_UPLOAD, msg->messageHandle, (uint8_t*)&reply, sizeof(firmware_upload_t));
                                break;
                            }
                            uint8_t* fw_data_ptr = k_malloc(fw_data_len);
                            if (fw_data_ptr == NULL) {
                                LOG_ERR("fw_data_ptr - malloc failed");
                                SEND_FATAL_ERROR();
                            }
                            memcpy(fw_data_ptr, fw_data, fw_data_len);
                            firmware_upload_data_t fw_upload_data;
                            uint32_t crc32 = 0;
                            crc32 = crc32_ieee_update(crc32, fw_data_ptr, fw_data_len);
                            //LOG_DBG("crc32 = 0x%08x", crc32);
                            if (crc32 != my_fw_upload->crc) {
                                LOG_ERR("crc32 != my_fw_upload->crc: 0x%08x != 0x%08x", crc32, my_fw_upload->crc);
                                firmware_upload_t reply = {
                                    .chunk_num = my_fw_upload->chunk_num,
                                    .chunk_total = my_fw_upload->chunk_total,
                                    .crc = my_fw_upload->crc,
                                    .data_len = 0,
                                    .return_code = 0x03
                                };
                                prepare_response(MESSAGE_TYPE_FW_UPLOAD, msg->messageHandle, (uint8_t*)&reply, sizeof(firmware_upload_t));
                                break;
                            }
                            fw_upload_data.data = fw_data_ptr;
                            fw_upload_data.data_len = fw_data_len;
                            fw_upload_data.chunk_num = my_fw_upload->chunk_num;
                            fw_upload_data.chunk_total = my_fw_upload->chunk_total;
                            fw_upload_data.crc = my_fw_upload->crc;
                            fw_upload_data.handle = msg->messageHandle;
                            err = zbus_chan_pub(&FW_UPLOAD_CHANNEL, &fw_upload_data, K_SECONDS(1));
                            if (err) {
                                LOG_ERR("zbus_chan_pub, error:%d", err);
                                SEND_FATAL_ERROR();
                            }
                            break;
                    default:
                            LOG_DBG("MESSAGE_TYPE_UNKNOWN");
                            ret = 0x01; // return 1 for error
                            prepare_basic_response_simple(msg->messageHandle, ret);
                            break;
            }
        }
        else {
            LOG_DBG("message wrong version = %d\n", msg->version);
        }
}


///////////////////////////////
/// 
///     spi_recv_action_work_handler
/// 
void spi_recv_action_work_handler(struct k_work *work) {
    if (work == NULL) {
        return;
    }
    workref_t *wr = CONTAINER_OF(work, workref_t, work);
    incoming_data_item_t *rx_data = (incoming_data_item_t*)wr->reference;

    if (rx_data) {
        nrf5340_recv_callback(rx_data->data, rx_data->len, NULL);
        if (rx_data->data) k_free(rx_data->data);
        if (rx_data) k_free(rx_data);
    }

    if (wr) wr_put(wr);
}



///////////////////////////////
/// 
///     manual_isr_setup
/// 
static void manual_isr_setup()
{
    IRQ_DIRECT_CONNECT(SPIM3_SPIS3_TWIM3_TWIS3_UARTE3_IRQn, 0,
        nrfx_spis_3_irq_handler, 0);
    irq_enable(SPIM3_SPIS3_TWIM3_TWIS3_UARTE3_IRQn);
}

///////////////////////////////
/// 
///     spis_event_handler
/// 
void spis_event_handler(nrfx_spis_evt_t const *event, void *p_context)
{
    if (event->evt_type == NRFX_SPIS_BUFFERS_SET_DONE) {
        //LOG_DBG("spis_event_handler: buffers set");
        
    }
    else if (event->evt_type == NRFX_SPIS_XFER_DONE) {
        if (event->tx_amount != 0) {
            //LOG_DBG("Setting DataReady to 0");
            gpio_pin_set(DataReady.port, DataReady.pin, 0);
            time_last_message_recvd = k_uptime_get();
            // k_sleep(K_USEC(50));
            // gpio_pin_set(DataReady.port, DataReady.pin, 1);
        spis_first_msg_received = true;
        }
        if (event->tx_amount == spis_actual_response_tx_len) {
            //spis_actual_response_tx_done = true;
            spi_buffer_is_generic_or_empty = true;
        }
        else {
            LOG_ERR("spis_event_handler: tx_amount != spis_actual_response_tx_len:  %d != %d", event->tx_amount, spis_actual_response_tx_len);
            spi_buffer_is_generic_or_empty = true;
        }
        // LOG_WRN("spis_event_handler: xfer done - rx(%d) tx(%d)", event->rx_amount, event->tx_amount);

        if (event->rx_amount > CONFIG_PURINA_D1_SPIS_RX_BUFFER_SIZE) {
            LOG_ERR("event->rx_amount > CONFIG_PURINA_D1_SPIS_RX_BUFFER_SIZE: %d", event->rx_amount);
            return;
        }

        if (event->rx_amount > 0) {
            message_command_v1_t *msg = (message_command_v1_t *)m_rx_buf;
            if ((msg->version == 1) && (msg->messageType != MESSAGE_TYPE_NO_OP)) {         
                incoming_data_item_t* fifo_item = k_malloc(sizeof(incoming_data_item_t));
                if (fifo_item == NULL) {
                    LOG_ERR("fifo_item - malloc failed - no room for more incoming data at this time");
                    prepare_basic_response_simple(msg->messageHandle, 1);
                    return;
                }
                fifo_item->data = k_malloc(event->rx_amount);
                if (fifo_item->data == NULL) {
                    LOG_ERR("fifo_item->data - no room for more incoming data at this time");
                    prepare_basic_response_simple(msg->messageHandle, 1);
                    k_free(fifo_item);
                    return;
                }                

                fifo_item->work = wr_get(fifo_item, __LINE__);
                k_work_init(&fifo_item->work->work, spi_recv_action_work_handler);
                memcpy(fifo_item->data, m_rx_buf, event->rx_amount);
                fifo_item->len = event->rx_amount;
                k_work_submit_to_queue(&spi_recv_work_q, &fifo_item->work->work);
            }
        }
        set_next_response();
    }
}


///////////////////////////////
/// 
///     init_spis_interface
/// 
int spis_setup() {
    //LOG_DBG("SPIs Init Started");

    spis_config.orc = 0xfe;
    spis_config.def = 0xcc;
    spis_config.miso_drive = NRF_GPIO_PIN_S0S1; // NRF_GPIO_PIN_H0H1

    nrf_spis_disable(spis.p_reg);

    if (gpio_pin_configure_dt(&DataReady, GPIO_OUTPUT) != 0)
    {
        LOG_ERR("Error: failed to configure %s pin %d\n",
            DataReady.port->name, DataReady.pin);
        return 0;
    }


    nrfx_err_t ret = nrfx_spis_init(&spis, &spis_config, spis_event_handler, NULL);
    if (ret == NRFX_ERROR_INVALID_STATE)
    {
        LOG_DBG("SPIs Init Failed INVALID_STATE (%X)", ret);
    }
    else if (ret != NRFX_SUCCESS) {
        LOG_DBG("SPIs Init Failed (%X)", ret);
        return 0;
    }

    manual_isr_setup();

    copyStaticSystemStatus(&last_modem_status);
    set_next_response();

    LOG_DBG("SPIs Init Complete");
    return 0;
}




void spis_zbus_mgr_listener(const struct zbus_channel *chan)
{



            if (&MQTT_CLOUD_TO_DEV_MESSAGE == chan) {
                LOG_DBG("zbus MQTT_CLOUD_TO_DEV_MESSAGE");
                const inc_mqtt_event_t* mqtt_msg_ptr = zbus_chan_const_msg(chan);
                if (mqtt_msg_ptr->msg_length > 0) {
                    spis_send_mqtt(*mqtt_msg_ptr);
                }
            }
            if (&STATUS_UPDATE == chan) {
                modem_status_t* status;
                zbus_chan_const_msg(chan);
                LOG_DBG("zbus STATUS_UPDATE");
                status = getStatusFromFifo();
                if (status) {
                    //save most recent status to be used for default response
                    copySystemStatus(&last_modem_status, status);
                    prepare_response(MESSAGE_TYPE_DEVICE_STATUS, 255, (uint8_t*)status, sizeof(modem_status_t));
                    k_free(status);
                }
            }
            if (&SPI_MSG_RESPONSE == chan) {
                const spi_msg_response_t *status = zbus_chan_const_msg(chan);
                LOG_DBG("zbus SPI_MSG_RESPONSE: %d %d %d", status->msgHandle, status->msgStatus, status->length);
                if (status->length > 0) {
                    prepare_basic_response(status->msgHandle, status->data, status->length);
                }
                else {
                    int8_t simple_response_data[1];
                    simple_response_data[0] = status->msgStatus;
                    prepare_response(MESSAGE_TYPE_RESPONSE, status->msgHandle, simple_response_data, 1);
                }
            }
            if (&DEBUG_INFO_CHANNEL == chan) {
                const debug_info_t *debug_info = zbus_chan_const_msg(chan);
                uint8_t* debug_mem = k_malloc(debug_info->debug_string_length + sizeof(debug_info_message_t));
                debug_info_message_t debug_info_msg;
                debug_info_msg.debug_level = debug_info->debug_level;
                debug_info_msg.debug_string_length = debug_info->debug_string_length;
                debug_info_msg.error_code = debug_info->error_code;
                memcpy(debug_mem, (uint8_t*)(&debug_info_msg), sizeof(debug_info_message_t));
                memcpy(debug_mem + sizeof(debug_info_message_t), debug_info->debug_string_buffer, debug_info->debug_string_length);
                LOG_DBG("zbus DEBUG_INFO_CHANNEL");
                prepare_response(MESSAGE_TYPE_LTE_DEBUG, 255, (uint8_t*)debug_mem, sizeof(debug_info_message_t) + debug_info->debug_string_length);
                k_free(debug_mem);
            }
            if (&GPS_DATA_CHANNEL == chan) {
                const gps_info_t* gps_data = zbus_chan_const_msg(chan);
                LOG_DBG("zbus GPS_DATA_CHANNEL");
                prepare_response(MESSAGE_TYPE_GPS_DATA, 255, (uint8_t*)gps_data, sizeof(gps_info_t));
            }
            if (&CELL_INFO_CHANNEL == chan) {
                const cell_info_t *cell_data = zbus_chan_const_msg(chan);
                LOG_DBG("zbus CELL_INFO_CHANNEL");
                //LOG_HEXDUMP_WRN(&cell_data, sizeof(cell_info_t), "Cell Info:");
                prepare_response(MESSAGE_TYPE_CELL_INFO, 255, (uint8_t*)cell_data, sizeof(cell_info_t));
            }
        

        //k_sleep(K_MSEC(1));
	
}


static void spis_module_thread_fn(void)
{
	int err;

	err = spis_setup();
	if (err) {
		LOG_ERR("setup, error: %d", err);
	}

    k_work_queue_init(&spi_recv_work_q);
    struct k_work_queue_config spi_recv_work_q_cfg = {
        .name = "spi_recv_work_q",
        .no_yield = 0,
    };
    k_work_queue_start(&spi_recv_work_q, spi_stack_area,
                   K_THREAD_STACK_SIZEOF(spi_stack_area), 3,
                   &spi_recv_work_q_cfg);
    
    spis_initialized = true;
    //LOG_5340_INF("9160 SPIs Module Thread Started");
	while (true) {
        // kick the queue now and then as a backup, almost certainly not needed, need to verify
        spi_recv_action_work_handler(NULL);
        k_sleep(K_MSEC(100));
	}
}
const k_tid_t spis_module_thread;
K_THREAD_DEFINE(spis_module_thread,
        16384,  
        spis_module_thread_fn, NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO-1, 0, 0);

