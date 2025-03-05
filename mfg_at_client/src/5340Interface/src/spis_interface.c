#if (!CONFIG_USE_UART_TO_NRF5340)


#include "spis_interface.h"
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/logging/log.h>
#include <stdio.h>


#define MY_SPI_SLAVE  DT_NODELABEL(spi3)


// SPI slave functionality
const struct device *spi_slave_dev;
static struct k_poll_signal spi_slave_done_sig = K_POLL_SIGNAL_INITIALIZER(spi_slave_done_sig);

static const struct spi_config spi_slave_cfg = {
	.operation = SPI_WORD_SET(8) | SPI_TRANSFER_MSB |
				 SPI_MODE_CPOL | SPI_MODE_CPHA | SPI_OP_MODE_SLAVE,
	.frequency = 4000000,
	.slave = 0,
};
static uint8_t slave_tx_buffer[2];
static uint8_t slave_rx_buffer[2];

int init_spis_interface()
{
    spi_slave_dev = DEVICE_DT_GET(MY_SPI_SLAVE);
    if(!device_is_ready(spi_slave_dev)) {
        printk("SPI slave device not ready!\n");
		return -1;
    }
	return 0;
}

static int spi_slave_prepare_response(void)
{
	static uint8_t counter = 0;


	const struct spi_buf s_tx_buf = {
		.buf = slave_tx_buffer,
		.len = sizeof(slave_tx_buffer)
	};
	const struct spi_buf_set s_tx = {
		.buffers = &s_tx_buf,
		.count = 1
	};

	struct spi_buf s_rx_buf = {
		.buf = slave_rx_buffer,
		.len = sizeof(slave_rx_buffer),
	};
	const struct spi_buf_set s_rx = {
		.buffers = &s_rx_buf,
		.count = 1
	};

	// Update the TX buffer with a rolling counter
	slave_tx_buffer[1] = counter++;
	printk("SPI SLAVE TX: 0x%.2x, 0x%.2x\n", slave_tx_buffer[0], slave_tx_buffer[1]);

	// Reset signal
	k_poll_signal_reset(&spi_slave_done_sig);
	
	// Start transaction
	int error = spi_transceive_async(spi_slave_dev, &spi_slave_cfg, &s_tx, &s_rx, &spi_slave_done_sig);
	if(error != 0){
		printk("SPI slave transceive error: %i\n", error);
		return error;
	}
	return 0;
}

static int spi_slave_check_for_message(void)
{
	int signaled, result;
	k_poll_signal_check(&spi_slave_done_sig, &signaled, &result);
	if(signaled != 0){
		return 0;
	}
	else return -1;
}

#else 
int init_spis_interface() { return 0;}

#endif