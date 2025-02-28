#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include "modem_interface_types.h"  // from c_modules/modem/include so its shared with the 5340



modem_status_t* getSystemStatus(void);
modem_info_t getModemInfo(void);
void config_set_lte_connected(bool state);
void config_set_lte_enabled(bool state);
void config_set_lte_working(bool state);
void config_set_fota_in_progress(bool state);
void config_set_mqtt_connected(bool state);
char* getStatusTime();
bool config_get_lte_connected();
bool config_get_lte_working();
char* getModemFWVersion();
void config_set_mqtt_initialized(bool state);
bool config_get_mqtt_initialized();
void setFOTAState(uint32_t state, uint8_t percentage);
void clearOneTimeStatuses();
void config_set_powered_off(bool state);
void config_set_mqtt_enabled(bool state);
void config_set_gpsEnabled(bool state);
bool config_get_fota_in_progress();
modem_status_t* getStatusFromFifo();
void copySystemStatus(modem_status_t *dst_status, modem_status_t *src_status);
void copyStaticSystemStatus(modem_status_t *status);
void printStatus(modem_status_t* status);
cell_info_t* getCellInfo();
void printCellInfo(cell_info_t* info, char* prefix);