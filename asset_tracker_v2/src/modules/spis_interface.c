#include <stdlib.h>
#include <zephyr/kernel.h>
#include "nrfx_spis.h"
#include "nrfx_gpiote.h"
#include <zephyr/sys/printk.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/irq.h>
#include <zephyr/logging/log.h>
#include <app_event_manager.h>
#include "events/sensor_module_event.h"
#include "events/location_module_event.h"
#include <math.h>
#include <nrf_modem_at.h>

//#include <zephyr/zbus/zbus.h>
//#include "message_channel.h"

LOG_MODULE_REGISTER(spis, CONFIG_LOCATION_MODULE_LOG_LEVEL); //TODO

extern bool disable_modem;

#define MODULE spis_module

#include "modules_common.h"
#include "events/app_module_event.h"
#include "events/data_module_event.h"
#include "events/sensor_module_event.h"
#include "events/util_module_event.h"

struct spis_msg_data {
	union {
		struct app_module_event app;
		struct data_module_event data;
		struct util_module_event util;
	} module;
};
/* Sensor module super states. */
static enum state_type {
	STATE_INIT,
	STATE_RUNNING,
	STATE_SHUTDOWN
} state;


/* Sensor module message queue. */
#define SPIS_QUEUE_ENTRY_COUNT	10
#define SPIS_QUEUE_BYTE_ALIGNMENT	4

K_MSGQ_DEFINE(msgq_spis, sizeof(struct spis_msg_data),
	      SPIS_QUEUE_ENTRY_COUNT, SPIS_QUEUE_BYTE_ALIGNMENT);

static struct module_data self = {
	.name = "spis",
	.msg_q = &msgq_spis,
	.supports_shutdown = true,
};

typedef enum {
    MESSAGE_TYPE_NO_OP = 0,
    MESSAGE_TYPE_RESPONSE = 1,
    MESSAGE_TYPE_COMMAND = 2,
    MESSAGE_TYPE_AT = 3,
    MESSAGE_TYPE_BATT_LVL = 4,
    MESSAGE_TYPE_JSON = 5,
    MESSAGE_TYPE_SSIDS = 6,
    MESSAGE_TYPE_DEVICE_PING = 7,
    MESSAGE_TYPE_NULL = 0xff
} message_type_t;

typedef enum {
    COMMAND_NO_OP = 0,
    COMMAND_REBOOT = 1,
    COMMAND_START = 2,
    COMMAND_NULL = 0xff
} command_type_t;

typedef struct message_response_v1_t {
    uint8_t version;
    uint8_t messageType;
    uint8_t messageHandle;
    uint16_t dataLen;
} __attribute__((__packed__)) message_response_v1_t ;

message_response_v1_t default_response = {
    .version = 0x01,
    .messageType = MESSAGE_TYPE_NO_OP,
    .messageHandle = 255,
    .dataLen = 0
};



#define SPIS_INSTANCE 3 /**< SPIS instance index. */
static const nrfx_spis_t spis =
    NRFX_SPIS_INSTANCE(SPIS_INSTANCE); /**< SPIS instance. */

#define APP_SPIS_CS_PIN 13
#define APP_SPIS_SCK_PIN 16
#define APP_SPIS_MISO_PIN 14
#define APP_SPIS_MOSI_PIN 15
#define RX_BUF_SIZE 1280
#define TX_BUF_SIZE 768


#define MAX_RESPONSES 3

#define MAX_INCOMING_BUFS MAX_RESPONSES
struct incoming_data_item_t {
    bool in_use;
    uint8_t data[RX_BUF_SIZE];
    uint16_t len;
} __attribute__((__packed__));

struct incoming_data_item_t  incoming_data_item_list[MAX_INCOMING_BUFS];
K_FIFO_DEFINE(incoming_fifo);

static nrfx_spis_config_t spis_config =
    NRFX_SPIS_DEFAULT_CONFIG(APP_SPIS_SCK_PIN, APP_SPIS_MOSI_PIN, APP_SPIS_MISO_PIN, APP_SPIS_CS_PIN);
static uint8_t m_tx_buf[TX_BUF_SIZE]; // TX buffer. 
static uint8_t m_rx_buf[RX_BUF_SIZE]; // RX buffer.

int64_t last_ssids_received_ts=0;
int spis_send_response(message_type_t type, uint8_t handle, uint8_t* data, uint16_t dataLen);
int set_spi_output_buffer(uint8_t *tx_buf, size_t len);
void set_next_response();

volatile bool spis_actual_response_tx_done = true;
volatile uint16_t spis_actual_response_tx_len = 0;

message_response_v1_t important_response = {
    .version = 0x01,
    .messageType = MESSAGE_TYPE_NO_OP,
    .messageHandle = 255,
    .dataLen = 0
};
static uint8_t m_important_tx_buf[TX_BUF_SIZE + sizeof(message_response_v1_t)]; // TX buffer. 
///////////////////////////////
/// 
///     prepare_response
/// 
void prepare_response(uint8_t handle, uint8_t* data, uint16_t dataLen) {
    LOG_DBG("dataLen = %d", dataLen);
    spis_actual_response_tx_done = false;
    important_response.version = 0x01;
    important_response.messageType = MESSAGE_TYPE_RESPONSE;
    important_response.messageHandle = handle;
    important_response.dataLen = dataLen;

    //spis_update_response();
    memcpy(m_important_tx_buf, (uint8_t*)(&important_response), sizeof(message_response_v1_t));
    memcpy(m_important_tx_buf + sizeof(message_response_v1_t), data, dataLen);

    spis_actual_response_tx_len = sizeof(message_response_v1_t) + dataLen;
    set_next_response();
}



///////////////////////////////
/// 
///     set_next_response
/// 
void set_next_response() {
    if (spis_actual_response_tx_done) {
        LOG_DBG("set_next_response - default");
        nrfx_spis_buffers_set(&spis, ((uint8_t*)&default_response),
            sizeof(message_response_v1_t), m_rx_buf,
            RX_BUF_SIZE);
        return;
    }
    else {
        LOG_WRN("set_next_response - important");
        nrfx_spis_buffers_set(&spis, ((uint8_t*)m_important_tx_buf),
            spis_actual_response_tx_len, m_rx_buf,
            RX_BUF_SIZE);
        return;        
    }
}



///////////////////////////////
/// 
///     nrf5340_recv_callback
/// 
void nrf5340_recv_callback(uint8_t *data, size_t len, void *user_data) {
    //LOG_DBG("len = %d", len);
    uint8_t simple_response_data[1];

        if (len < sizeof(message_response_v1_t)) {
                //LOG_DBG("message wrong len = %d/%d\n", len, sizeof(message_response_v1_t));
                return;
        }

        message_response_v1_t *msg = (message_response_v1_t *)data;
        if (msg->version == 0x01) {
            // LOG_DBG("nrf5340_recv_callback  dataLen = %d\n", msg->dataLen);
            // LOG_DBG("nrf5340_recv_callback  spi interrupt data recvd size = %d\n",spis_rx_buff_recvd_len);
            // LOG_DBG("nrf5340_recv_callback  spi interrupt data sent size = %d\n",spis_tx_buff_sent_len);
            // LOG_DBG("nrf5340_recv_callback  handle = %d\n", msg->messageHandle);
            // LOG_DBG("nrf5340_recv_callback  msg data size = %d\n", msg->dataLen);

            switch (msg->messageType) {
                    case MESSAGE_TYPE_JSON:
                            LOG_DBG("MESSAGE_TYPE_JSON");
                            char* ssid_string_ptr = (data + sizeof(message_response_v1_t));
                            ssid_string_ptr[msg->dataLen] = 0; //Terminate the string

                            simple_response_data[0] = 0x00;  // return 0 for successs
                            prepare_response(msg->messageHandle, simple_response_data, 1);
                            break;
                    case MESSAGE_TYPE_RESPONSE:
                            LOG_DBG("MESSAGE_TYPE_RESPONSE, that's odd");
                            simple_response_data[0] = 0x01;  // return 1 for error
                            prepare_response(msg->messageHandle, simple_response_data, 1);
                            break;
                    case MESSAGE_TYPE_COMMAND:
                            LOG_DBG("MESSAGE_TYPE_COMMAND");

                            // get specific command
                            command_type_t modem_command = *((command_type_t*)(data + sizeof(message_response_v1_t)));
                            switch (modem_command) {
                                case COMMAND_NO_OP:
                                        LOG_DBG("COMMAND_NO_OP");
                                        simple_response_data[0] = 0x00;  // return 0 for success, its null/no-op, I guess its always a success
                                        prepare_response(msg->messageHandle, simple_response_data, 1);
                                        break;
                                case COMMAND_REBOOT:
                                        LOG_DBG("COMMAND_REBOOT");
                                        simple_response_data[0] = 0x00;  // return 0 for success, its null/no-op, I guess its always a success
                                        prepare_response(msg->messageHandle, simple_response_data, 1);
                                        break;
                                case COMMAND_START:
                                        LOG_DBG("COMMAND_START");
                                        disable_modem = false;
                                        simple_response_data[0] = 0x00;  // return 0 for success, its null/no-op, I guess its always a success
                                        prepare_response(msg->messageHandle, simple_response_data, 1);
                                        break;
                                case COMMAND_NULL:
                                        LOG_DBG("COMMAND_NULL");
                                        simple_response_data[0] = 0x00;  // return 0 for success, its null/no-op, I guess its always a success
                                        prepare_response(msg->messageHandle, simple_response_data, 1);
                                        break;
                                default:
                                        LOG_DBG("COMMAND_UNKNOWN");
                                        simple_response_data[0] = 0x01;  // return 0 for error since we dont do this YET
                                        prepare_response(msg->messageHandle, simple_response_data, 1);
                                        break;
                            }
                            break;
                    case MESSAGE_TYPE_AT:
                            LOG_DBG("MESSAGE_TYPE_AT");
                            char* atCommand = (char*)(data + sizeof(message_response_v1_t));
                            atCommand[msg->dataLen] = 0; //Terminate the string
                            int err;
                            //char command[64];
                            //memcpy(command, atCommand, msg->dataLen);
                            //command[(msg->dataLen)] = 0;

                            char response[1024];
                            LOG_WRN("recieved atCommand = {%d}", strlen(atCommand));
                            err = nrf_modem_at_cmd(response, sizeof(response), "%s", atCommand);
                            LOG_WRN("recieved atCommand response = [%d]", strlen(response));
                            if (err < 0) {
                                    LOG_DBG("error with nrf_modem_at_cmd");
                                    strcpy(response, "ERROR\0");
                            }

                            prepare_response(msg->messageHandle, response, strlen(response));
                            break;
                    case MESSAGE_TYPE_NULL:
                            LOG_DBG("MESSAGE_TYPE_NULL");
                            simple_response_data[0] = 0x00;  // return 0 for success, its null, I guess its always a success
                            prepare_response(msg->messageHandle, simple_response_data, 1);
                            break;
                    case MESSAGE_TYPE_BATT_LVL:
                            LOG_DBG("MESSAGE_TYPE_BATT_LVL");
                            if (msg->dataLen != sizeof(float)) {
                                LOG_DBG("MESSAGE_TYPE_BATT_LVL - wrong dataLen (expects float (%d bytes))= %d", sizeof(float), msg->dataLen);
                                simple_response_data[0] = 0x01;  // return 1 for error
                                prepare_response(msg->messageHandle, simple_response_data, 1);
                                break;
                            }
                            uint8_t *batt_level = ((uint8_t*)(data + sizeof(message_response_v1_t)));
                            LOG_DBG("4 bytes received");
                            struct _temp_float{
                                union {
                                    float value;
                                    uint8_t _raw[4];
                                };
                            } temp_float;
                            for( int i =0 ; i < msg->dataLen ; i++){
                                LOG_DBG("[%02x] ",batt_level[i]);
                                temp_float._raw[i] = batt_level[i];
                            }
                            LOG_DBG("\n");
                            LOG_DBG("FLOAT is %f\n",temp_float.value);
                            int newBatt = (fabs(temp_float.value));
                            LOG_DBG("  new batt_level = %d", newBatt);


                            struct sensor_module_event *sensor_module_event = new_sensor_module_event();

                            __ASSERT(sensor_module_event, "Not enough heap left to allocate event");

                            sensor_module_event->data.bat.timestamp = k_uptime_get();
                            sensor_module_event->data.bat.battery_level = newBatt;
                            sensor_module_event->type = SENSOR_EVT_FUEL_GAUGE_READY;
                            APP_EVENT_SUBMIT(sensor_module_event);
                            simple_response_data[0] = 0x00;  // return 0 for success
                            prepare_response(msg->messageHandle, simple_response_data, 1);
                            break;
                    case MESSAGE_TYPE_NO_OP:
                            LOG_DBG("MESSAGE_TYPE_NO_OP");
                            //simple_response_data[0] = 0x00;  // return 0 for success, its null/no-op, I guess its always a success
                            //prepare_response(msg->messageHandle, simple_response_data, 0);
                            break;
                    case MESSAGE_TYPE_SSIDS:
                            LOG_DBG("MESSAGE_TYPE_SSIDS");
                            last_ssids_received_ts = k_uptime_get();
                            uint8_t* ssidList = data + sizeof(message_response_v1_t);
	                        struct location_module_event *evt = new_location_module_event();
                            evt->data.cloud_location.neighbor_cells_valid = false;

                            evt->data.cloud_location.wifi_access_points_valid = true;
                            evt->data.cloud_location.wifi_access_points.cnt = 0;
                            while(sscanf(ssidList, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx,%hhd\n",
                                    &evt->data.cloud_location.wifi_access_points.ap_info[evt->data.cloud_location.wifi_access_points.cnt].mac[0],
                                    &evt->data.cloud_location.wifi_access_points.ap_info[evt->data.cloud_location.wifi_access_points.cnt].mac[1],
                                    &evt->data.cloud_location.wifi_access_points.ap_info[evt->data.cloud_location.wifi_access_points.cnt].mac[2],
                                    &evt->data.cloud_location.wifi_access_points.ap_info[evt->data.cloud_location.wifi_access_points.cnt].mac[3],
                                    &evt->data.cloud_location.wifi_access_points.ap_info[evt->data.cloud_location.wifi_access_points.cnt].mac[4],
                                    &evt->data.cloud_location.wifi_access_points.ap_info[evt->data.cloud_location.wifi_access_points.cnt].mac[5],
                                    &evt->data.cloud_location.wifi_access_points.ap_info[evt->data.cloud_location.wifi_access_points.cnt].rssi) == 7) {
                                LOG_DBG("  ssid = %hhx:%hhx:%hhx:%hhx:%hhx:%hhx,%hhd\n",
                                    evt->data.cloud_location.wifi_access_points.ap_info[evt->data.cloud_location.wifi_access_points.cnt].mac[0],
                                    evt->data.cloud_location.wifi_access_points.ap_info[evt->data.cloud_location.wifi_access_points.cnt].mac[1],
                                    evt->data.cloud_location.wifi_access_points.ap_info[evt->data.cloud_location.wifi_access_points.cnt].mac[2],
                                    evt->data.cloud_location.wifi_access_points.ap_info[evt->data.cloud_location.wifi_access_points.cnt].mac[3],
                                    evt->data.cloud_location.wifi_access_points.ap_info[evt->data.cloud_location.wifi_access_points.cnt].mac[4],
                                    evt->data.cloud_location.wifi_access_points.ap_info[evt->data.cloud_location.wifi_access_points.cnt].mac[5],
                                    evt->data.cloud_location.wifi_access_points.ap_info[evt->data.cloud_location.wifi_access_points.cnt].rssi);
                                evt->data.cloud_location.wifi_access_points.ap_info[evt->data.cloud_location.wifi_access_points.cnt].mac_length = 6;
                                evt->data.cloud_location.wifi_access_points.cnt++;
                                ssidList = strchr(ssidList, '\n');
                                if (ssidList == NULL) {
                                    break;
                                }
                                ssidList++;
                            }

                            evt->type = LOCATION_MODULE_EVT_CLOUD_LOCATION_DATA_READY;
                            evt->data.cloud_location.timestamp = k_uptime_get();
                            APP_EVENT_SUBMIT(evt);
                            simple_response_data[0] = 0x00;  // return 0 for success
                            prepare_response(msg->messageHandle, simple_response_data, 1);
                            break;
                    case MESSAGE_TYPE_DEVICE_PING:
                        // not currrently handled
                    default:
                            LOG_DBG("MESSAGE_TYPE_UNKNOWN");
                            simple_response_data[0] = 0x01; // return 1 for error
                            prepare_response(msg->messageHandle, simple_response_data, 1);
                            break;
            }
        }
        else {
            LOG_DBG("message wrong version = %d\n", msg->version);
        }
}

//////////////////////////////
/// 
///     spi_get_next_incoming_buffer
/// 
int spi_get_next_incoming_buffer() {
    for (int i=0;i<MAX_INCOMING_BUFS;i++) {
        if (incoming_data_item_list[i].in_use == false) {
            LOG_DBG("spi_get_next_incoming_buffer: found available incoming_data_item_list[%d]", i);
            incoming_data_item_list[i].in_use = true;
            return i;
        }
    }
    return -1;
}

///////////////////////////////
/// 
///     spi_recv_action_work_handler
/// 
void spi_recv_action_work_handler(struct k_work *work) {
    while (k_fifo_is_empty(&incoming_fifo) == false) {
        struct incoming_data_item_t *rx_data = k_fifo_get(&incoming_fifo, K_NO_WAIT);
        if (rx_data) {
            nrf5340_recv_callback(rx_data->data, rx_data->len, NULL);
            rx_data->in_use = false;
        }
        else {
            LOG_ERR("spi_recv_action_work_handler: no data in fifo");
        }
    }
}
/* Register the work handler */
K_WORK_DEFINE(spi_recv_action_work, spi_recv_action_work_handler);


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
        // LOG_DBG("spis_event_handler: buffers set");
    }
    else if (event->evt_type == NRFX_SPIS_XFER_DONE) {
        if (event->tx_amount == spis_actual_response_tx_len) {
            spis_actual_response_tx_done = true;
        }
        // LOG_WRN("spis_event_handler: xfer done - rx(%d) tx(%d)", event->rx_amount, event->tx_amount);

        message_response_v1_t *msg = (message_response_v1_t *)m_rx_buf;
        if ((msg->version == 1) && (msg->messageType != MESSAGE_TYPE_NO_OP)) {         
            int buf_indx = spi_get_next_incoming_buffer();
            if (buf_indx == -1) {
                LOG_ERR("spis_event_handler: no available incoming_data_item_list");
                return;
            }
            struct incoming_data_item_t* fifo_item = &incoming_data_item_list[buf_indx];
            memcpy(fifo_item->data, m_rx_buf, event->rx_amount);
            fifo_item->len = event->rx_amount;
            k_fifo_alloc_put(&incoming_fifo, fifo_item);
            k_work_submit(&spi_recv_action_work);
        }
        set_next_response(); 
    }   
}

///////////////////////////////
/// 
///     set_spi_output_buffer
/// 
int set_spi_output_buffer(uint8_t *tx_buf, size_t len) {

    if (len > TX_BUF_SIZE) {
        return -1;
    }
    memcpy(m_tx_buf, tx_buf, len);

    if (NRFX_SUCCESS != nrfx_spis_buffers_set(&spis, m_tx_buf,
                len, m_rx_buf,
                RX_BUF_SIZE)) {
            LOG_DBG("SPIs buff set Failed -len(%d)\n", len);
            return -1;
        }

    return 0;
}


///////////////////////////////
/// 
///     spis_send_response
/// 
int spis_send_response(message_type_t type, uint8_t handle, uint8_t* data, uint16_t dataLen){

    return 0;
}

static char *state2str(enum state_type new_state)
{
	switch (new_state) {
	case STATE_INIT:
		return "STATE_INIT";
	case STATE_RUNNING:
		return "STATE_RUNNING";
	case STATE_SHUTDOWN:
		return "STATE_SHUTDOWN";
	default:
		return "Unknown";
	}
}

static void state_set(enum state_type new_state)
{
	if (new_state == state) {
		LOG_DBG("State: %s", state2str(state));
		return;
	}

	LOG_DBG("State transition %s --> %s",
		state2str(state),
		state2str(new_state));

	state = new_state;
}

///////////////////////////////
/// 
///     init_spis_interface
/// 
int spis_setup() {
    LOG_DBG("SPIs Init Started");
    
    spis_config.orc = 0xfe;
    spis_config.def = 0xcc;
    spis_config.miso_drive = NRF_GPIO_PIN_S0S1; //NRF_GPIO_PIN_S0S1; // NRF_GPIO_PIN_H0S1  NRF_GPIO_PIN_H0H1

    nrf_spis_disable(spis.p_reg);

    nrfx_err_t ret = nrfx_spis_init(&spis, &spis_config, spis_event_handler, NULL);
    if (ret == NRFX_ERROR_INVALID_STATE)
    {
        LOG_DBG("SPIs Init Failed INVALID_STATE (%X)", ret);
        //return 0;
    }
    else if (ret != NRFX_SUCCESS) {
        LOG_DBG("SPIs Init Failed (%X)", ret);
        return 0;
    }

    manual_isr_setup();


    set_next_response();

    LOG_DBG("SPIs Init Done\n");
    state_set(STATE_RUNNING);
    return 0;
}




static void spis_module_thread_fn(void)
{
	int err;
	struct spis_msg_data msg = { 0 };

	self.thread_id = k_current_get();

	err = module_start(&self);
	if (err) {
		LOG_ERR("Failed starting module, error: %d", err);
		SEND_ERROR(sensor, SENSOR_EVT_ERROR, err);
	}

	state_set(STATE_INIT);

	err = spis_setup();
	if (err) {
		LOG_ERR("setup, error: %d", err);
		//SEND_ERROR(sensor, SENSOR_EVT_ERROR, err);
	}

	while (true) {
		module_get_next_msg(&self, &msg);

		switch (state) {
		case STATE_INIT:
			//on_state_init(&msg);
			break;
		case STATE_RUNNING:
			//on_state_running(&msg);
			break;
		case STATE_SHUTDOWN:
			/* The shutdown state has no transition. */
			break;
		default:
			LOG_ERR("Unknown state.");
			break;
		}

		//on_all_states(&msg);
	}
}

const k_tid_t spis_module_thread;
K_THREAD_DEFINE(spis_module_thread,
        1024,
        spis_module_thread_fn, NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO-1, 0, 0);
#if 0
APP_EVENT_LISTENER(MODULE, app_event_handler);
APP_EVENT_SUBSCRIBE(MODULE, app_module_event);
APP_EVENT_SUBSCRIBE(MODULE, data_module_event);
APP_EVENT_SUBSCRIBE(MODULE, util_module_event);
#endif
