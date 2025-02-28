/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */
#include "d1_json.h"

// #include <zephyr/logging/log_ctrl.h>
// #include <zephyr/logging/log.h>
// #include <zephyr/shell/shell.h>
// #include <stdlib.h>
// #include <stdio.h>
// #include <zephyr/kernel.h>
// #include <string.h>

// void do_test1(const struct shell *sh, size_t argc, char **argv)
// {
//     wifi_arr_t* wifi_top = k_malloc(sizeof(wifi_arr_t));
//     if (!wifi_top) { shell_print(sh, "allocation failed\n"); }

//     wifi_top->count = 0;

//     strcpy(wifi_top->wifi[0].ssid,"SSID1");
//     wifi_top->wifi[0].rssi = -45;
//     wifi_top->wifi[0].channel = 1;
//     wifi_top->count++;

//     strcpy(wifi_top->wifi[1].ssid,"SSID2");
//     wifi_top->wifi[1].rssi = -35;
//     wifi_top->wifi[1].channel = 1;
//     wifi_top->count++;

//     strcpy(wifi_top->wifi[2].ssid,"SSID3");
//     wifi_top->wifi[2].rssi = -25;
//     wifi_top->wifi[2].channel = 3;
//     wifi_top->count++;

//     char* retStr = json_wifi_data(wifi_top);
//     shell_print(sh, "JSON = %s\n", retStr);
//     k_free(wifi_top);
//     //k_free(retStr);  // should k_free this, its important, but since this is just a test I can
//     avoid it
//                     // shell_print doesnt print immidiately and if you k_free it now, it crashes.
// }

// SHELL_STATIC_SUBCMD_SET_CREATE(
//     sub_json,
//     SHELL_CMD(test1, NULL, "reset the modem", do_test1),
//     SHELL_SUBCMD_SET_END
// );

// SHELL_CMD_REGISTER(json, &sub_json, "JSON test commands", NULL);
