#include "rffe.h"
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/printk.h>
#include <zephyr/logging/log.h>
#include <zephyr/kernel.h>

LOG_MODULE_REGISTER(rffe, LOG_LEVEL_DBG);

#define LOW          0
#define HIGH         1
#define RFFE_PERIOD  2
// inverse of 0x4    2
#define RFFE_ADDRESS 0x2

#define RFFE_WRITE 0x2
#define RFFE_READ  0x3

static const struct gpio_dt_spec vio = GPIO_DT_SPEC_GET(DT_NODELABEL(antennavio), gpios);
static const struct gpio_dt_spec rffe_sda = GPIO_DT_SPEC_GET(DT_NODELABEL(antennasda), gpios);
static const struct gpio_dt_spec rffe_sck = GPIO_DT_SPEC_GET(DT_NODELABEL(antennascl), gpios);

int RFFE_init()
{
	uint8_t ret = 0;
	ret = gpio_pin_configure_dt(&vio, GPIO_OUTPUT);
	if (ret != 0) {
		LOG_ERR("Error %d: failed to configure %s pin %d\n", ret, vio.port->name, vio.pin);
		return -1;
	}
	ret = gpio_pin_configure_dt(&rffe_sda, GPIO_OUTPUT);
	if (ret != 0) {
		LOG_ERR("Error %d: failed to configure %s pin %d\n", ret, rffe_sda.port->name,
			rffe_sda.pin);
		return -1;
	}
	ret = gpio_pin_configure_dt(&rffe_sck, GPIO_OUTPUT);
	if (ret != 0) {
		LOG_ERR("Error %d: failed to configure %s pin %d\n", ret, rffe_sck.port->name,
			rffe_sck.pin);
		return -1;
	}
	gpio_pin_set_dt(&vio, 1);
	gpio_pin_set_dt(&rffe_sda, 0);
	gpio_pin_set_dt(&rffe_sck, 0);

	LOG_DBG("RFFE Setup complete.\n");
	k_busy_wait(100);
	return 0;
}

bool RFFE_Bitbang(uint16_t data, uint8_t dataLen, bool sendParity)
{
	uint8_t parityCount = 0;
	printk("Sending %X bits: \n", dataLen);
	for (uint8_t bit = 0u; bit < dataLen; bit++) {
		// Set SDA before SCK changes
		if (data & (1 << bit)) {
			gpio_pin_set_dt(&rffe_sda, 1);
			printk("1");
			parityCount++;
		} else {
			gpio_pin_set_dt(&rffe_sda, 0);
			printk("0");
		}
		printk("\n");
		gpio_pin_set_dt(&rffe_sck, 1);
		gpio_pin_set_dt(&rffe_sck, 0);
	}
	if (sendParity) {
		if (parityCount & 0x01) {
			gpio_pin_set_dt(&rffe_sda, 0);
		} else {
			gpio_pin_set_dt(&rffe_sda, 1);
		}
		gpio_pin_set_dt(&rffe_sck, 1);
		gpio_pin_set_dt(&rffe_sck, 0);
	}
	return true;
}

uint8_t RFFE_sendHeader(uint8_t cmd, uint8_t regAddr, bool parkBus)
{
	// send start sequence condition
	//   a single low to high and back pulse while sclk is low
	gpio_pin_set_dt(&rffe_sda, 1);
	gpio_pin_set_dt(&rffe_sda, 0);

	// send 4 bit slave ID
	// RFFE_Bitbang(RFFE_ADDRESS, 4, false);

	// send write command b010
	// send 5 bit reg address
	uint8_t data = cmd << 5 | (regAddr & 0x1F);
	// printk("data %X \n", data);
	uint8_t v2 = ((data & 0x01) << 7) | ((data & 0x02) << 5) | ((data & 0x04) << 3) |
		     ((data & 0x08) << 1) | ((data & 0x10) >> 1) | ((data & 0x20) >> 3) |
		     ((data & 0x40) >> 5) | ((data & 0x80) >> 7);
	data = v2;
	// printk("data %X \n", data);

	uint16_t alldata = 0;
	alldata |= (data << 4);
	alldata |= RFFE_ADDRESS;

	RFFE_Bitbang(alldata, 12, true);

	if (parkBus) {
		// send bus park
		gpio_pin_set_dt(&rffe_sda, 0);
		gpio_pin_set_dt(&rffe_sck, 1);

		gpio_pin_configure_dt(&rffe_sda, GPIO_DISCONNECTED);
		gpio_pin_set_dt(&rffe_sck, 0);
	}
	return 0;
}

uint8_t RFFE_write(uint16_t regAddr, uint8_t val)
{
	// send start sequence condition
	// //   a single low to high and back pulse while sclk is low
	gpio_pin_set_dt(&rffe_sda, 1);
	gpio_pin_set_dt(&rffe_sda, 0);

	// // send 4 bit slave ID
	// RFFE_Bitbang(RFFE_ADDRESS, 4, false);

	// RFFE_sendHeader(RFFE_READ, regAddr, false);
	//  send 4 bit slave ID
	// RFFE_Bitbang(RFFE_ADDRESS, 4, false);

	// send write command b010
	// send 5 bit reg address
	uint8_t data = RFFE_WRITE << 5 | (regAddr & 0x1F);
	uint8_t v2 = ((data & 0x01) << 7) | ((data & 0x02) << 5) | ((data & 0x04) << 3) |
		     ((data & 0x08) << 1) | ((data & 0x10) >> 1) | ((data & 0x20) >> 3) |
		     ((data & 0x40) >> 5) | ((data & 0x80) >> 7);
	data = v2;
	uint16_t alldata = 0;
	alldata |= (data << 4);
	alldata |= RFFE_ADDRESS;

	RFFE_Bitbang(alldata, 12, true);

	// // send write command b010
	// // send 5 bit reg address
	// uint8_t dataLen = 0x1;
	// uint8_t cmd = 0x0;  //extended write
	// uint8_t data = dataLen;
	// uint8_t v2 =
	// 	((data & 0x01) << 7) |
	// 	((data & 0x02) << 5) |
	// 	((data & 0x04) << 3) |
	// 	((data & 0x08) << 1) |
	// 	((data & 0x10) >> 1) |
	// 	((data & 0x20) >> 3) |
	// 	((data & 0x40) >> 5) |
	// 	((data & 0x80) >> 7);
	// data = v2;
	// RFFE_Bitbang(data, 8, true);

	// uint8_t addr = (regAddr & 0x1F);
	// v2 =
	// 	((addr & 0x01) << 7) |
	// 	((addr & 0x02) << 5) |
	// 	((addr & 0x04) << 3) |
	// 	((addr & 0x08) << 1) |
	// 	((addr & 0x10) >> 1) |
	// 	((addr & 0x20) >> 3) |
	// 	((addr & 0x40) >> 5) |
	// 	((addr & 0x80) >> 7);
	// addr = v2;
	// RFFE_Bitbang(addr, 8, true);

	// v2 =
	// 	((data & 0x01) << 7) |
	// 	((data & 0x02) << 5) |
	// 	((data & 0x04) << 3) |
	// 	((data & 0x08) << 1) |
	// 	((data & 0x10) >> 1) |
	// 	((data & 0x20) >> 3) |
	// 	((data & 0x40) >> 5) |
	// 	((data & 0x80) >> 7);
	// data = v2;
	// RFFE_Bitbang(data, 8, true);

	// // send bus park
	// gpio_pin_set_dt(&rffe_sck, 1);
	// gpio_pin_configure_dt(&rffe_sda, GPIO_DISCONNECTED);
	// gpio_pin_set_dt(&rffe_sck, 0);

	// RFFE_sendHeader(RFFE_WRITE, regAddr, false);

	// // send data
	v2 = ((val & 0x01) << 7) | ((val & 0x02) << 5) | ((val & 0x04) << 3) | ((val & 0x08) << 1) |
	     ((val & 0x10) >> 1) | ((val & 0x20) >> 3) | ((val & 0x40) >> 5) | ((val & 0x80) >> 7);
	val = v2;
	RFFE_Bitbang(val, 8, true);

	// // send bus park
	gpio_pin_set_dt(&rffe_sck, 1);
	gpio_pin_set_dt(&rffe_sda, 0);
	gpio_pin_configure_dt(&rffe_sda, GPIO_DISCONNECTED);
	gpio_pin_set_dt(&rffe_sck, 0);

	k_busy_wait(RFFE_PERIOD);

	gpio_pin_configure_dt(&rffe_sda, GPIO_OUTPUT);
	gpio_pin_set_dt(&rffe_sda, 0);
	gpio_pin_set_dt(&rffe_sck, 0);
	k_busy_wait(100);

	return 0;
}

uint8_t RFFE_write2(uint16_t regAddr, uint8_t val)
{
	// send start sequence condition
	// //   a single low to high and back pulse while sclk is low
	gpio_pin_set_dt(&rffe_sda, 1);
	gpio_pin_set_dt(&rffe_sda, 0);

	// // send 4 bit slave ID
	RFFE_Bitbang(RFFE_ADDRESS, 4, false);

	// RFFE_sendHeader(RFFE_READ, regAddr, false);
	//  send 4 bit slave ID
	// RFFE_Bitbang(RFFE_ADDRESS, 4, false);

	uint8_t data = 1;
	uint8_t v2 = ((data & 0x01) << 7) | ((data & 0x02) << 5) | ((data & 0x04) << 3) |
		     ((data & 0x08) << 1) | ((data & 0x10) >> 1) | ((data & 0x20) >> 3) |
		     ((data & 0x40) >> 5) | ((data & 0x80) >> 7);
	data = v2;
	RFFE_Bitbang(data, 8, true);

	// // send write command b010
	// // send 5 bit reg address
	// uint8_t dataLen = 0x1;
	// uint8_t cmd = 0x0;  //extended write
	data = regAddr;
	v2 = ((data & 0x01) << 7) | ((data & 0x02) << 5) | ((data & 0x04) << 3) |
	     ((data & 0x08) << 1) | ((data & 0x10) >> 1) | ((data & 0x20) >> 3) |
	     ((data & 0x40) >> 5) | ((data & 0x80) >> 7);
	data = v2;
	RFFE_Bitbang(data, 8, true);

	// uint8_t addr = (regAddr & 0x1F);
	// v2 =
	// 	((addr & 0x01) << 7) |
	// 	((addr & 0x02) << 5) |
	// 	((addr & 0x04) << 3) |
	// 	((addr & 0x08) << 1) |
	// 	((addr & 0x10) >> 1) |
	// 	((addr & 0x20) >> 3) |
	// 	((addr & 0x40) >> 5) |
	// 	((addr & 0x80) >> 7);
	// addr = v2;
	// RFFE_Bitbang(addr, 8, true);

	v2 = ((val & 0x01) << 7) | ((val & 0x02) << 5) | ((val & 0x04) << 3) | ((val & 0x08) << 1) |
	     ((val & 0x10) >> 1) | ((val & 0x20) >> 3) | ((val & 0x40) >> 5) | ((val & 0x80) >> 7);
	val = v2;
	RFFE_Bitbang(val, 8, true);

	// // send bus park
	gpio_pin_set_dt(&rffe_sck, 1);
	gpio_pin_set_dt(&rffe_sda, 0);
	gpio_pin_configure_dt(&rffe_sda, GPIO_DISCONNECTED);
	gpio_pin_set_dt(&rffe_sck, 0);

	// RFFE_sendHeader(RFFE_WRITE, regAddr, false);

	// // // send data
	//  v2 =
	// ((val & 0x01) << 7) |
	// ((val & 0x02) << 5) |
	// ((val & 0x04) << 3) |
	// ((val & 0x08) << 1) |
	// ((val & 0x10) >> 1) |
	// ((val & 0x20) >> 3) |
	// ((val & 0x40) >> 5) |
	// ((val & 0x80) >> 7);
	// val = v2;
	// RFFE_Bitbang(val, 8, true);

	// // // send bus park
	// gpio_pin_set_dt(&rffe_sck, 1);
	// gpio_pin_configure_dt(&rffe_sda, GPIO_DISCONNECTED);
	// gpio_pin_set_dt(&rffe_sck, 0);

	// k_busy_wait(RFFE_PERIOD);

	gpio_pin_configure_dt(&rffe_sda, GPIO_OUTPUT);
	gpio_pin_set_dt(&rffe_sda, 0);
	gpio_pin_set_dt(&rffe_sck, 0);
	k_busy_wait(100);

	return 0;
}

uint8_t RFFE_read(uint16_t regAddr)
{
	uint8_t out = 0;

	RFFE_sendHeader(RFFE_READ, regAddr, true);
	gpio_pin_configure_dt(&rffe_sda, GPIO_INPUT);

	// start reading data
	for (uint8_t i = 0; i < 8; i++) {
		gpio_pin_set_dt(&rffe_sck, 1);
		gpio_pin_set_dt(&rffe_sck, 0);
		out |= gpio_pin_get_dt(&rffe_sda) << (7 - i);
	}
	gpio_pin_set_dt(&rffe_sck, 1); // clock 2 more times for the client to sent pol and bus park
	gpio_pin_set_dt(&rffe_sck, 0);
	gpio_pin_set_dt(&rffe_sck, 1);
	gpio_pin_set_dt(&rffe_sck, 0);
	k_busy_wait(RFFE_PERIOD);

	gpio_pin_configure_dt(&rffe_sda, GPIO_OUTPUT);
	gpio_pin_set_dt(&rffe_sda, 0);
	gpio_pin_set_dt(&rffe_sck, 0);
	k_busy_wait(100);
	return out;
}

uint8_t RFFE_read2(uint8_t regAddr)
{
	uint8_t out = 0;
	uint8_t read_len = 2;

	gpio_pin_set_dt(&rffe_sda, 1);
	gpio_pin_set_dt(&rffe_sda, 0);

	// RFFE_sendHeader(RFFE_READ, regAddr, true);
	// gpio_pin_configure_dt(&rffe_sda, GPIO_INPUT);
	RFFE_Bitbang(RFFE_ADDRESS, 4, false);

	uint8_t data = 0x20 & read_len;
	uint8_t v2 = ((data & 0x01) << 7) | ((data & 0x02) << 5) | ((data & 0x04) << 3) |
		     ((data & 0x08) << 1) | ((data & 0x10) >> 1) | ((data & 0x20) >> 3) |
		     ((data & 0x40) >> 5) | ((data & 0x80) >> 7);
	data = v2;
	RFFE_Bitbang(data, 8, true);

	data = regAddr;
	v2 = ((data & 0x01) << 7) | ((data & 0x02) << 5) | ((data & 0x04) << 3) |
	     ((data & 0x08) << 1) | ((data & 0x10) >> 1) | ((data & 0x20) >> 3) |
	     ((data & 0x40) >> 5) | ((data & 0x80) >> 7);
	data = v2;
	RFFE_Bitbang(data, 8, true);

	gpio_pin_set_dt(&rffe_sda, 0);

	gpio_pin_set_dt(&rffe_sck, 1);
	gpio_pin_set_dt(&rffe_sda, 0);
	gpio_pin_configure_dt(&rffe_sda, GPIO_DISCONNECTED);
	gpio_pin_set_dt(&rffe_sck, 0);
	gpio_pin_configure_dt(&rffe_sda, GPIO_INPUT);

	// start reading data
	for (uint8_t j = 0; j < read_len; j++) {
		for (uint8_t i = 0; i < 8; i++) {
			gpio_pin_set_dt(&rffe_sck, 1);
			gpio_pin_set_dt(&rffe_sck, 0);
			out |= gpio_pin_get_dt(&rffe_sda) << (7 - i);
		}
		gpio_pin_set_dt(&rffe_sck, 1); // clock 1 more times for the client to sent pol
		gpio_pin_set_dt(&rffe_sck, 0);
	}
	gpio_pin_set_dt(&rffe_sck, 1); // and bus park
	gpio_pin_set_dt(&rffe_sck, 0);
	k_busy_wait(RFFE_PERIOD);

	gpio_pin_set_dt(&rffe_sda, 0);
	gpio_pin_configure_dt(&rffe_sda, GPIO_OUTPUT);
	gpio_pin_set_dt(&rffe_sda, 0);
	gpio_pin_set_dt(&rffe_sck, 0);
	k_busy_wait(100);
	return out;
}