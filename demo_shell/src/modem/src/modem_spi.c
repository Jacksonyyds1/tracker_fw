//#include <zephyr.h>
#include "modem.h"
#include "nrf.h"
#include <nrfx_spim.h>
#include <nrfx_gpiote.h>
#include <drivers/nrfx_errors.h>
//#include <zephyr/printk.h>
#include <string.h>
#include <zephyr/kernel.h>
#include "nrfx_spis.h"
#include "nrfx_gpiote.h"
#include <zephyr/sys/printk.h>
#include <zephyr/irq.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/gpio.h>

#if (!CONFIG_USE_UART_TO_NRF9160)

static const struct gpio_dt_spec spi4cs = GPIO_DT_SPEC_GET(DT_NODELABEL(spi4cs), gpios);


LOG_MODULE_REGISTER(spi_modem, LOG_LEVEL_DBG);

#define SPI_INSTANCE 4
static const nrfx_spim_t spim = NRFX_SPIM_INSTANCE(SPI_INSTANCE);

#define APP_SPIM_CS_PIN (2)
#define APP_SPIM_SCK_PIN (8)
#define APP_SPIM_MISO_PIN (10)
#define APP_SPIM_MOSI_PIN (9)

#define NRFX_CUSTOM_ERROR_CODES 0 //used in nrfx_errors.h

static nrfx_spim_config_t spim_config =
    NRFX_SPIM_DEFAULT_CONFIG(APP_SPIM_SCK_PIN, APP_SPIM_MOSI_PIN, APP_SPIM_MISO_PIN, APP_SPIM_CS_PIN);

#define SPIM_RX_BUFF_SIZE 2048
static uint8_t m_rx_buf[SPIM_RX_BUFF_SIZE];

static volatile bool spim_xfer_done; /**< Flag used to indicate that SPIM instance completed the transfer. */
static uint8_t* spim_rx_buff_ptr; // pointer to rx buffer
static uint8_t* spim_rx_buff; // pointer to rx buffer
static volatile uint16_t spim_rx_reply_bytes_remaining; // number of bytes remaining to be read
typedef void (*spim_on_rx_cb_t)(uint8_t *data, size_t len, void *user_data);
spim_on_rx_cb_t spim_on_rx_cb = NULL;
void* spim_on_rx_cb_userData = NULL;
static uint8_t* spim_tx_buff; // pointer to tx buffer

///////////////////////////////
/// 
///     manual_isr_setup 
/// 
static void manual_isr_setup()
{
    IRQ_DIRECT_CONNECT(SPIM4_IRQn, 0,
               nrfx_spim_4_irq_handler, 0);
    irq_enable(SPIM4_IRQn);
}


///////////////////////////////
/// 
///     spim_recv_action_work_handler
/// 
void spim_recv_action_work_handler(struct k_work *work) {
    //LOG_DBG("entry spim_recv_action_work_handler");
    message_command_v1_t* cmd = (message_command_v1_t*)m_rx_buf;

    uint16_t dataLen = cmd->dataLen;  //(m_rx_buf[3] << 8) + (m_rx_buf[4]) + 6;
    // LOG_DBG("spim_recv_action_work_handler: ");
    // LOG_DBG("messageType = %d", cmd->messageType);
    // LOG_DBG("messageHandle = %d", cmd->messageHandle);
    // LOG_DBG("dataLen = %d", dataLen); 
    // LOG_DBG("data = ");
    // for (int i = 6; i < (dataLen-1); i++) {
    //     if ((m_rx_buf[i])+1 != (m_rx_buf[i+1])) {
    //         printk("data invalid at %d  0x%02X 0x%02X 0x%02X", i, m_rx_buf[i-1], m_rx_buf[i], m_rx_buf[i+1]);
    //     }
    // }

    if (spim_on_rx_cb) {
        printk("calling spim_on_rx_cb\n");
        spim_on_rx_cb(m_rx_buf, dataLen, spim_on_rx_cb_userData);
    }
}
/* Register the work handler */
K_WORK_DEFINE(spim_recv_action_work, spim_recv_action_work_handler);


///////////////////////////////
/// 
///     spim_event_handler
/// 
void spim_event_handler(nrfx_spim_evt_t const *p_event, void *p_context)
{
    //printk("IRQ: %d\n", p_event->type);
    if (p_event->type == NRFX_SPIM_EVENT_DONE) {
        spim_xfer_done = true;
        if (spim_tx_buff) { free(spim_tx_buff); spim_tx_buff = NULL; }
        printk("Transfer completed\n");
    }
        gpio_pin_set_dt(&spi4cs, 1);
        if (m_rx_buf[0] != 0 || m_rx_buf[0] != 0xff)
        {
            

            if (spim_rx_reply_bytes_remaining == 0) {
                // we have the whole response
                //kick off a work object to process the callback
                k_work_submit(&spim_recv_action_work);
            }
            
        }

    
}
///////////////////////////////
/// 
///     modem_spi_set_rx_cb
/// 
void modem_spi_set_rx_cb(void (*cb)(uint8_t *data, size_t len, void *user_data), void *user_data){
    spim_on_rx_cb = cb;
    spim_on_rx_cb_userData = user_data;
}
void spi_rx_app_cb(uint8_t *data, size_t len, void *user_data)
{
    LOG_ERR("Got %d bytes back\n",len);

}
///////////////////////////////
/// 
///     modem_spi_init
/// 
int modem_spi_init(void){
    LOG_DBG("SPIM setup");
    spim_rx_reply_bytes_remaining = 0;
    spim_rx_buff = NULL;
    spim_rx_buff_ptr = NULL;

    int ret = gpio_pin_configure_dt(&spi4cs, GPIO_OUTPUT);
    gpio_pin_set_dt(&spi4cs, 1);

    if (ret != 0)
    {
        LOG_ERR("Error %d: failed to configure %s pin %d\n",
               ret, spi4cs.port->name, spi4cs.pin);
        return -1;
    }
    

    spim_config.frequency = NRFX_MHZ_TO_HZ(4);
    //spim_config.use_hw_ss = true;
    //spim_config.ss_duration = 255;
    //spim_config.miso_pull = NRF_GPIO_PIN_PULLDOWN; // NRF_GPIO_PIN_NOPULL;  // NRF_GPIO_PIN_PULLDOWN NRF_GPIO_PIN_PULLUP
    if (NRFX_SUCCESS !=
        nrfx_spim_init(&spim, &spim_config, spim_event_handler, NULL)) {
        LOG_ERR("Init Failed\n");
        return 0;
    }

    modem_spi_set_rx_cb(spi_rx_app_cb,NULL);
    manual_isr_setup();

    return 0;
}


///////////////////////////////
/// 
///     modem_spi_send
/// 
int modem_spi_send(uint8_t *buf, uint16_t len, uint8_t *buf2){
    memset(m_rx_buf, 0, SPIM_RX_BUFF_SIZE);

    printk("modem_spi_send: len = %d\n", len);

    gpio_pin_set_dt(&spi4cs, 0);
    k_sleep(K_USEC(20));

    spim_xfer_done = false;
    nrfx_spim_xfer_desc_t xfer_desc = NRFX_SPIM_XFER_TRX(buf, len, m_rx_buf, SPIM_RX_BUFF_SIZE);

    // LOG_DBG("SPIM sending (%d):", len);
    // for (int i = 0; i < len; i++) {
    //     LOG_DBG("%c ", buf[i]);
    // }

    nrfx_err_t err_code = nrfx_spim_xfer(&spim, &xfer_desc, 0);

    if (err_code == NRFX_ERROR_BUSY) {
        LOG_ERR("SPI busy\n");
    }
    else if (err_code != NRFX_SUCCESS){
        LOG_ERR("Error code = %d\n", err_code);
        return err_code;
    }

    if (buf2 != NULL) {
        memcpy(buf2, m_rx_buf, len);
    }
    else {
        //LOG_DBG("SPIM received:");
        // for (int i = 0; i < len; i++) {
        //     LOG_DBG("%x ", m_rx_buf[i]);
        // }
    }
    return 0;
}


///////////////////////////////
/// 
///     modem_spi_recv
/// 
int modem_spi_recv(char *buf, k_timeout_t timeout) {
    return 0;
}


///////////////////////////////
/// 
///     modem_spi_send_command
/// 
int modem_spi_send_command(modem_message_type_t type, uint8_t handle, uint8_t* data, uint16_t dataLen){
    uint8_t ret = 0;

    uint8_t* spim_tx_buff = malloc(dataLen + sizeof(message_command_v1_t));
    message_command_v1_t* cmd = (message_command_v1_t*)spim_tx_buff;

    cmd->version = 0x01;
    cmd->messageType = type;
    cmd->messageHandle = handle;
    cmd->dataLen = dataLen;

    for (int i = 0; i < dataLen; i++) {
        spim_tx_buff[i + sizeof(message_command_v1_t)] = data[i];
    }
    printk("data len is %d\n",dataLen);
    ret = modem_spi_send(spim_tx_buff, dataLen + sizeof(message_command_v1_t), NULL);
    //free(spim_tx_buff);
    
    return 0;
}

#endif