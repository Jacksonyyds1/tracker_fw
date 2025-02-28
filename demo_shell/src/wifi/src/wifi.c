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


const struct device *en_wifi = NULL;
const struct device *en3v3 = NULL;

int wifi_init()
{
    int ret = 0;

    wifi_power_on();

    en3v3 = device_get_binding("gpio@842800");
    if (en3v3 == NULL) {
        LOG_ERR("Error: didn't find %s device", "GPIO_1");
        return -1;
    }else {
        int ret = gpio_pin_configure(en3v3, 4, GPIO_OUTPUT_ACTIVE);
        if (ret != 0) {
            LOG_ERR("Error %d: failed to configure %s pin %d",
                   ret, "GPIO_1", 4);
        }else{
            LOG_DBG("turned on the WIFI 3v3?_enable");
        }
    }
    

    en_wifi = device_get_binding("gpio@842500");
    if (en_wifi == NULL) {
        LOG_ERR("Error: didn't find %s device", "GPIO_0");
        return -1;
    }else {
        int ret = gpio_pin_configure(en_wifi, 28, GPIO_OUTPUT_ACTIVE);
        if (ret != 0) {
            LOG_ERR("Error %d: failed to configure %s pin %d",
                   ret, "GPIO_0", 28);
        }else{
            LOG_DBG("turned on the WIFI power_key");
        }
    }

#if (CONFIG_USE_UART_TO_DA16200)
    // Initialize UART
    LOG_DBG("Initializing UART to DA16200");
    ret = wifi_uart_init();
#else
    ret = wifi_spi_init();
#endif
    LOG_DBG("Wifi Enabled.");
    return ret;
}

int wifi_set_power_key(bool newState)
{
    int ret = 0;
    if (newState) {
        ret = gpio_pin_set(en_wifi, 28, 1);
    }else{
        ret = gpio_pin_set(en_wifi, 28, 0);
    }
    return ret;
}


int wifi_set_3v3_enable(bool newState)
{
    int ret = 0;
    if (newState) {
        ret = gpio_pin_set(en3v3, 4, 1);
    }else{
        ret = gpio_pin_set(en3v3, 4, 0);
    }
    return ret;
}


int wifi_send(char *buf)
{
    int ret = 0;
#if (CONFIG_USE_UART_TO_DA16200)
    // Initialize UART
    ret = wifi_uart_send(buf);
#else
    ret = wifi_spi_send(buf);
#endif
    return ret;
}


int wifi_recv(char *buf, k_timeout_t timeout)
{
    int ret = 0;
#if (CONFIG_USE_UART_TO_DA16200)
    // Initialize UART
    ret = wifi_uart_recv(buf, timeout);
#else 
    ret = wifi_spi_recv(buf, timeout);
#endif
    return ret;
}


int wifi_recv_line(char *buf, k_timeout_t timeout)
{
    int ret = 0;
    int cnt = 0;
    // printf("BUF SPACE %d\n",cnt);
#if (CONFIG_USE_UART_TO_DA16200)
    // Initialize UART
    while(true){
        ret = wifi_uart_recv(&buf[cnt], timeout);
        // printf("%c[%d]",buf[cnt],ret);
        if( ret ==0){
            if( buf[cnt] == '\r'){
                // printf("[EOL]");
                return 0; // Line ended, let them retry
            }
            cnt++;
            if(cnt >=900){
                printf("OUT OF BUF SPACE %d\n",cnt);
                buf[cnt]=0;
                return 0; //Make the caller retry
            }
        }else {
            // printf("NO data\n");
            return ret;
        }
    }
#else
    ret = wifi_spi_recv(buf, timeout);
    return ret;
#endif
}

void wifi_set_rx_cb(void (*cb)(uint8_t *data, size_t len, void *user_data), void *user_data)
{
#if (CONFIG_USE_UART_TO_DA16200)
    wifi_uart_set_rx_cb(cb, user_data);
#else
    wifi_spi_set_rx_cb(cb, user_data);
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
