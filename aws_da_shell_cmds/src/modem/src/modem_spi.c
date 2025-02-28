#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <stdint.h>
#include <string.h>

#if (!CONFIG_USE_UART_TO_NRF9160)


int modem_spi_init(void){
    return 0;
}

int modem_spi_send(char *buf){

    return 0;
}

int modem_spi_recv(char *buf, k_timeout_t timeout) {
    return 0;
}

void modem_spi_set_rx_cb(void (*cb)(uint8_t *data, size_t len, void *user_data), void *user_data){

}

#endif