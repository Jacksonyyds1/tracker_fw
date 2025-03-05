/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */
#include <stdlib.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/sys/printk.h>
#include <getopt.h>

#include "modem.h"
#include <zephyr/logging/log.h>

int
modem_set_rffe_settings()
{
#define NUM_RFFE_CMDS 8
    const char *rffe[NUM_RFFE_CMDS] = {
        "AT+CFUN=0",
        "AT\%XMIPIRFFEDEV?",
        "AT\%XMIPIRFFEDEV=4,4,102,3,184",
        "AT\%XMIPIRFFECTRL=4,0,1,28,184",
#if 1    //DVT unit
        "AT\%XMIPIRFFECTRL=4,1,1,28,56,5,1,1,4,4,746,2,2,799,1,1,894,2,2,1990,8,8,2200",
#else
        "AT\%XMIPIRFFECTRL=4,1,1,28,56,5,1,1,4,4,746,2,2,787,4,4,824,1,1,894,8,8,2200",
#endif
        "AT\%XMIPIRFFECTRL=4,2,1,28,184",
        "AT\%XMIPIRFFECTRL=4,3,1,28,184",
        "AT+CFUN=0"
    };
    char     resp_buf[200];
    uint16_t resp_len = sizeof(resp_buf);
    int      my_handle;

    for (int i = 0; i < NUM_RFFE_CMDS; i++) {
        my_handle = modem_send_command(MESSAGE_TYPE_AT, (uint8_t *)rffe[i], strlen(rffe[i]), true);
        // k_sleep(K_MSEC(100));
        memset(resp_buf, 0, sizeof(resp_buf));
        if (modem_recv_resp(my_handle, resp_buf, &resp_len, 1000) == 0) {    // 1 sec timeout, thats a looooong time
            // Modem returned valid response
            if (strstr(resp_buf, "OK\r\n")) {
                printf("[%s]..OK...\n", rffe[i]);
            }
            // printf("<<<<{%s}\n",resp_buf);
        } else {
            // Modem response timed out
            printf("Modem response timed out\n");
        }
        k_sleep(K_MSEC(100));
    }
    return 0;
}