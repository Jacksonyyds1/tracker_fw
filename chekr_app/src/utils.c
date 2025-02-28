#include <stdint.h>
#include <stdbool.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/posix/time.h>
#include <zephyr/sys/byteorder.h>

#include "pmic.h"
#include "utils.h"

#define EN_3V0_PIN (4)
static const struct device *gpio1 = DEVICE_DT_GET(DT_NODELABEL(gpio1));

#define POLYNOMIAL    0xA001
#define INITIAL_VALUE 0xFFFF

LOG_MODULE_REGISTER(utils, LOG_LEVEL_DBG);

uint16_t utils_crc16_modbus(const uint8_t *data, uint16_t length)
{
	uint16_t crc = INITIAL_VALUE;
	uint16_t i, j;

	for (i = 0; i < length; i++) {
		crc ^= data[i];
		for (j = 0; j < 8; j++) {
			bool lsb = crc & 1;
			crc >>= 1;
			if (lsb) {
				crc ^= POLYNOMIAL;
			}
		}
	}

	// crc is always big endian
	crc = sys_cpu_to_be16(crc);
	return crc;
}

uint64_t utils_get_currentmillis(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);

	uint64_t t = (ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
	return t;
}

uint64_t utils_get_currentmicros(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);

	uint64_t t = (ts.tv_sec) * 1000 + ts.tv_nsec / 1000;
	return t;
}

int util_enable_dialog(bool enable)
{
	uint32_t output_state = GPIO_OUTPUT_INACTIVE;
	bool power_state = false;

	if (enable) {
		output_state = GPIO_OUTPUT_ACTIVE;
		power_state = true;
	}



	// 3V rail control
	int ret = gpio_pin_configure(gpio1, EN_3V0_PIN, output_state);
	if (ret != 0) {
		LOG_ERR("Error: failed to set EN_3V0_PIN to %d", output_state);
		return ret;
	} else {
		LOG_DBG("Configured 3V output to 0x%04x", output_state);
	}

	// level shifter and the digital logic line
	set_switch_state(PMIC_SWITCH_WIFI, power_state);
	LOG_DBG("set PMIC DA power to %d", power_state);
	return 0;
}

int util_enable_9160(bool enable)
{
	bool power_state = false;
	if (enable) {
		power_state = true;
	}

	set_switch_state(PMIC_SWITCH_VSYS, power_state);
	LOG_DBG("set PMIC 9160 power to %d", power_state);
	return 0;
}
