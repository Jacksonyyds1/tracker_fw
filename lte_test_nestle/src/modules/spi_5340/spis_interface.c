

#include <stdlib.h>
#include <zephyr/kernel.h>
#include "nrfx_spis.h"
#include "nrfx_gpiote.h"
#include <zephyr/sys/printk.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/irq.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>
#include "message_channel.h"

LOG_MODULE_REGISTER(spis, CONFIG_MQTT_SAMPLE_TRIGGER_LOG_LEVEL);


typedef enum {
    MESSAGE_TYPE_NO_OP = 0,
    MESSAGE_TYPE_RESPONSE = 1,
    MESSAGE_TYPE_COMMAND = 2,
    MESSAGE_TYPE_AT = 3,
    MESSAGE_TYPE_BATT_LVL = 4,
    MESSAGE_TYPE_JSON = 5,
    MESSAGE_TYPE_NULL = 0xff
} message_type_t;

typedef struct message_response_v1_t {
    uint8_t version;
    uint8_t messageType;
    uint8_t messageHandle;
    uint16_t dataLen;
} __attribute__((__packed__)) message_response_v1_t ;


#define SPIS_INSTANCE 3 /**< SPIS instance index. */
static const nrfx_spis_t spis =
    NRFX_SPIS_INSTANCE(SPIS_INSTANCE); /**< SPIS instance. */

// DT_PROP(DT_NODELABEL(i2c1), clock_frequency)
// PINCTRL_DT_STATE_PINS_DEFINE(DT_PATH(zephyr_user), uart0_alt_default);
// pinctrl_lookup_state(const struct pinctrl_dev_config *config, uint8_t id, const struct pinctrl_state **state)¶
// PINCTRL_DT_DEV_CONFIG_GET

//const struct pinctrl_dev_config* spis_pinctrls = PINCTRL_DT_DEV_CONFIG_GET(DEVICE_DT_GET(DT_NODELABEL(spi3)));

#define APP_SPIS_CS_PIN 13
#define APP_SPIS_SCK_PIN 16
#define APP_SPIS_MISO_PIN 14
#define APP_SPIS_MOSI_PIN 15

#define RX_BUF_SIZE 2048
#define TX_BUF_SIZE 128

volatile uint16_t spis_rx_buff_recvd_len = 0;
volatile uint16_t spis_tx_buff_sent_len = 0;


static nrfx_spis_config_t spis_config =
    NRFX_SPIS_DEFAULT_CONFIG(APP_SPIS_SCK_PIN, APP_SPIS_MOSI_PIN, APP_SPIS_MISO_PIN, APP_SPIS_CS_PIN);
static uint8_t m_tx_buf[TX_BUF_SIZE]; // TX buffer. 
static uint8_t m_rx_buf[RX_BUF_SIZE]; // RX buffer.


int spis_send_response(message_type_t type, uint8_t handle, uint8_t* data, uint16_t dataLen);

void nrf5340_recv_callback(uint8_t *data, size_t len, void *user_data) {
        // this is executing in a work queue

        // for (int i = 0; i < len; i++) {
        //  if (data[i] != 0xff) {
        //      if (data[i] == 0x7f) { printk("%x (%d)", data[i], i); }
        //      else { printk("%x ", data[i]); }
        //  }
                
        // }
        // printk("\n");

        if (len < sizeof(message_response_v1_t)) {
                //printk("nrf5340_recv_callback: message wrong len = %d/%d\n", len, sizeof(message_response_v1_t));
                return;
        }

        message_response_v1_t *msg = (message_response_v1_t *)data;
        if (msg->version == 0x01) {
            printk("nrf5340_recv_callback  dataLen = %d\n", msg->dataLen);
            printk("nrf5340_recv_callback  spi interrupt data recvd size = %d\n",spis_rx_buff_recvd_len);
            printk("nrf5340_recv_callback  spi interrupt data sent size = %d\n",spis_tx_buff_sent_len);
            
            printk("nrf5340_recv_callback  handle = %d\n", msg->messageHandle);
            

            switch (msg->messageType) {
                    case MESSAGE_TYPE_JSON:
                            printk("nrf5340_recv_callback: MESSAGE_TYPE_JSON\n");
                            printk("JSON = %s\n", (char*)(data + sizeof(message_response_v1_t)));
                            break;
                    case MESSAGE_TYPE_RESPONSE:
                            printk("nrf5340_recv_callback: MESSAGE_TYPE_RESPONSE, that's odd\n");
                            break;
                    case MESSAGE_TYPE_COMMAND:
                            printk("nrf5340_recv_callback: MESSAGE_TYPE_COMMAND\n");
                            break;
                    case MESSAGE_TYPE_AT:
                            printk("nrf5340_recv_callback: MESSAGE_TYPE_AT\n");
                            break;
                    case MESSAGE_TYPE_NULL:
                            printk("nrf5340_recv_callback: MESSAGE_TYPE_NULL\n");
                            break;
                    case MESSAGE_TYPE_BATT_LVL:
                            printk("nrf5340_recv_callback: MESSAGE_TYPE_BATT_LVL\n");
                            float batt_level = *((float*)(data + sizeof(message_response_v1_t)));
                            printk("  batt_level = %f\n", batt_level);
                            int err;

                            err = zbus_chan_pub(&TRIGGER_CHAN, &batt_level, K_SECONDS(1));
                            if (err) {
                                printk("zbus_chan_pub, error: %d", err);
                                SEND_FATAL_ERROR();
                            }
                            break;
                    case MESSAGE_TYPE_NO_OP:
                            printk("nrf5340_recv_callback: MESSAGE_TYPE_NO_OP\n");
                            break;
                    default:
                            printk("nrf5340_recv_callback: MESSAGE_TYPE_UNKNOWN\n");
                            break;
            }
        }
        else {
            printk("nrf5340_recv_callback: message wrong version = %d\n", msg->version);
        }


// prepare the response
        uint8_t spi_output_buffer[TX_BUF_SIZE];
        uint16_t buff_len = TX_BUF_SIZE;
        //memset(spi_output_buffer, 0, buff_len);

// TODO - gen random ret data, random len to test the 5340 recv side
        buff_len = (rand() % (TX_BUF_SIZE-sizeof(message_response_v1_t)));
        for(int i=0;i<buff_len;i++){
                spi_output_buffer[i] = (i & 0xff);
        }
        //spi_output_buffer[buff_len] = 0;

// send response
        spis_send_response(MESSAGE_TYPE_RESPONSE, msg->messageHandle, spi_output_buffer, buff_len);

}



///////////////////////////////
/// 
///     spi_recv_action_work_handler
/// 
void spi_recv_action_work_handler(struct k_work *work) {

    nrf5340_recv_callback(m_rx_buf, spis_rx_buff_recvd_len, NULL);
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
        //printk("spis_event_handler: buffers set\n");
    }
    else if (event->evt_type == NRFX_SPIS_XFER_DONE) {
        spis_rx_buff_recvd_len = event->rx_amount;
        spis_tx_buff_sent_len = event->tx_amount;
        k_work_submit(&spi_recv_action_work);
        
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
            printk("SPIs buff set Failed -len(%d)\n", len);
            return -1;
        }

    return 0;
}


///////////////////////////////
/// 
///     spis_send_response
/// 
int spis_send_response(message_type_t type, uint8_t handle, uint8_t* data, uint16_t dataLen){

    uint8_t* buff = malloc(dataLen + sizeof(message_response_v1_t));

    buff[0] = 0x01;  // version
    buff[1] = type;
    buff[2] = handle;
    buff[3] = dataLen & 0xff;
    buff[4] = dataLen >> 8;

    memcpy(buff + sizeof(message_response_v1_t), data, dataLen);

    set_spi_output_buffer(buff, dataLen + sizeof(message_response_v1_t));
    free(buff);

    return 0;
}

///////////////////////////////
/// 
///     init_spis_interface
/// 
int spis_task() {

    spis_config.orc = 0xfe;
    spis_config.def = 0xcc;
    spis_config.miso_drive = NRF_GPIO_PIN_S0S1; //NRF_GPIO_PIN_S0S1; // NRF_GPIO_PIN_H0S1  NRF_GPIO_PIN_H0H1
    if (NRFX_SUCCESS !=
        nrfx_spis_init(&spis, &spis_config, spis_event_handler, NULL)) {
        printk("SPIs Init Failed");
        return 0;
    }

    manual_isr_setup();

    uint8_t firstBuff[1] = {0xff};
    // fill out the first buffer with a dummy byte
    spis_send_response(MESSAGE_TYPE_NULL, 0, firstBuff, 1);

    printk("SPIs Init Done\n");
    return 1;
}




K_THREAD_DEFINE(spis_task_id,
        CONFIG_MQTT_SAMPLE_5340_SPI_THREAD_STACK_SIZE,
        spis_task, NULL, NULL, NULL, 3, 0, 0);
