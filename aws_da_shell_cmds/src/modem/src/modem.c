#include "modem.h"
#include <zephyr/logging/log.h>

#if (CONFIG_USE_UART_TO_NRF9160)
#include "modem_uart.h"
#else
#include "modem_spi.h"
#endif

#include <zephyr/kernel.h>
#include "pmic.h"

LOG_MODULE_REGISTER(modem, LOG_LEVEL_DBG);


int modem_init()
{
    int ret = 0;
    modem_power_on();

#if (CONFIG_USE_UART_TO_NRF9160)
    // Initialize UART
    LOG_DBG("Initializing UART to 9160");
    ret = modem_uart_init();
#else
    ret = modem_spi_init();
#endif
    return ret;
}


int modem_send(char *buf)
{
    int ret = 0;
#if (CONFIG_USE_UART_TO_NRF9160)
    // Initialize UART
    ret = modem_uart_send(buf);
#else
    ret = modem_spi_send(buf);
#endif
    return ret;
}


int modem_recv(char *buf, k_timeout_t timeout)
{
    int ret = 0;
#if (CONFIG_USE_UART_TO_NRF9160)
    // Initialize UART
    ret = modem_uart_recv(buf, timeout);
#else 
    ret = modem_spi_recv(buf, timeout);
#endif
    return ret;
}

void modem_set_rx_cb(void (*cb)(uint8_t *data, size_t len, void *user_data), void *user_data)
{
#if (CONFIG_USE_UART_TO_NRF9160)
    modem_uart_set_rx_cb(cb, user_data);
#else
    modem_spi_set_rx_cb(cb, user_data);
#endif
}

int modem_power_on() 
{
    set_switch_state(PMIC_SWITCH_VSYS, true);
    return 0;
}

int modem_power_off() 
{
    set_switch_state(PMIC_SWITCH_VSYS, false);
    return 0;
}
