#include "wifi.h"
#include <zephyr/logging/log.h>

#if (CONFIG_USE_UART_TO_DA16200)
#include "wifi_uart.h"
#else
#include "wifi_spi.h"
#endif

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include "pmic.h"

LOG_MODULE_REGISTER(wifi, LOG_LEVEL_DBG);


const struct device *gpio_p0 = NULL;
const struct device *gpio_p1 = NULL;

int wifi_init()
{
    int ret = 0;

    wifi_power_on();

    gpio_p1 = device_get_binding("gpio@842800");
    if (gpio_p1 == NULL) {
        LOG_ERR("Error: didn't find %s device", "GPIO_1");
        return -1;
    }else {
	    int ret = gpio_pin_configure(gpio_p1, 4, GPIO_OUTPUT_ACTIVE);
	    if (ret != 0) {
	        LOG_ERR("Error %d: failed to configure %s pin %d",
	               ret, "GPIO_1", 4);
	    }else{
	    	LOG_DBG("turned on the WIFI 3v3?_enable");
	    }
	    ret = gpio_pin_configure(gpio_p1, 14, GPIO_OUTPUT_ACTIVE);
	    if (ret != 0) {
	        LOG_ERR("Error %d: failed to configure %s pin %d",
	               ret, "GPIO_1", 14);
	    }else{
	    	LOG_DBG("configured WIFI level shifter");
	    }
        ret = gpio_pin_set(gpio_p1, 14, 1);
	    if (ret != 0) {
	        LOG_ERR("Error %d: failed to turn on level shifter, %s pin %d",
	               ret, "GPIO_1", 14);
	    }else{
	    	LOG_DBG(" WIFI level shifter turned on");
	    }
	    ret = gpio_pin_configure(gpio_p1, 8, GPIO_OUTPUT_ACTIVE);
	    if (ret != 0) {
	        LOG_ERR("Error %d: failed to configure %s pin %d",
	               ret, "GPIO_1", 8);
	    }else{
	    	LOG_DBG("configured WIFI wakeup");
	    }
  #if (CONFIG_USE_UART_TO_DA16200)
  #else
        // set up the gpio that lets the DA tell us when data is ready on SPI
	    ret = gpio_pin_configure(gpio_p1, 7, GPIO_INPUT);
	    if (ret != 0) {
	        LOG_ERR("Error %d: failed to configure %s pin %d",
	               ret, "GPIO_1", 7);
	    }else{
	    	LOG_DBG("configured DA data ready gpio");
	    }
  #endif
 	}

    gpio_p0 = device_get_binding("gpio@842500");
    if (gpio_p0 == NULL) {
        LOG_ERR("Error: didn't find %s device", "GPIO_0");
        return -1;
    }else {
	    int ret = gpio_pin_configure(gpio_p0, 28, GPIO_OUTPUT_ACTIVE);
	    if (ret != 0) {
	        LOG_ERR("Error %d: failed to configure %s pin %d",
	               ret, "GPIO_0", 28);
	    }else{
	    	LOG_DBG("turned on the WIFI power_key");
	    }
	}

#if (CONFIG_USE_UART_TO_DA16200)
    LOG_DBG("Initializing UART to DA16200");
    ret = wifi_uart_init();
#else
    LOG_DBG("Initializing SPI to DA16200");
    ret = wifi_spi_init();
#endif
    LOG_DBG("Wifi Enabled.");
    return ret;
}

int wifi_set_power_key(bool newState)
{
    int ret = 0;
    if (newState) {
        ret = gpio_pin_set(gpio_p0, 28, 1);
    }else{
        ret = gpio_pin_set(gpio_p0, 28, 0);
    }
    return ret;
}


int wifi_set_3v3_enable(bool newState)
{
    int ret = 0;
    if (newState) {
        ret = gpio_pin_set(gpio_p1, 4, 1);
    }else{
        ret = gpio_pin_set(gpio_p1, 4, 0);
    }
    return ret;
}


int wifi_set_wakeup(bool newState)
{
    int ret = 0;
    if (newState) {
        ret = gpio_pin_set(gpio_p1, 8, 1);
    }else{
        ret = gpio_pin_set(gpio_p1, 8, 0);
    }
    return ret;
}


int wifi_send(char *buf)
{
    int ret = 0;
#if (CONFIG_USE_UART_TO_DA16200)
    ret = wifi_uart_send(buf);
#else
    ret = wifi_spi_send(buf);
#endif
    return ret;
}

int wifi_send_timeout(char *buf, k_timeout_t timeout)
{
    int ret = 0;
#if (CONFIG_USE_UART_TO_DA16200)
    ret = wifi_uart_send_timeout(buf, timeout);
#else
    ret = wifi_spi_send_timeout(buf, timeout);
#endif
    return ret;
}


int wifi_recv(wifi_msg_t *msg, k_timeout_t timeout)
{
#if (CONFIG_USE_UART_TO_DA16200)
    return wifi_uart_recv(msg, timeout);
#else 
    return wifi_spi_recv(msg, timeout);
#endif
}

void wifi_msg_free(wifi_msg_t *msg)
{
#if (CONFIG_USE_UART_TO_DA16200)
    wifi_uart_msg_free(msg);
#else 
    wifi_spi_msg_free(msg);
#endif
}

void wifi_set_rx_cb(wifi_on_rx_cb_t cb, void *user_data)
{
#if (CONFIG_USE_UART_TO_DA16200)
    wifi_uart_set_rx_cb(cb, user_data);
#else
    wifi_spi_set_rx_cb(cb, user_data);
#endif
}

//////////////////////////////////////////////////////////
//  wifi_flush_msgs()
//
//  Flush all messages from the receive queue
void wifi_flush_msgs()
{
#if (CONFIG_USE_UART_TO_DA16200)
    wifi_uart_flush_msgs();
#else
    wifi_spi_flush_msgs();
#endif
}

int wifi_power_on()
{
    set_switch_state(PMIC_SWITCH_WIFI, true);
    return 0;
}

int wifi_power_off()
{
    set_switch_state(PMIC_SWITCH_WIFI, false);
    return 0;
}
