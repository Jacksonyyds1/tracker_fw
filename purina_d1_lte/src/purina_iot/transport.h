#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <zephyr/kernel.h>

#include "modem_interface_types.h"  // from c_modules/modem/include so its shared with the 5340


int transport_set_settings(const char *settingsJsonStr);
int transport_set_subscription_topics(const char *topicsJsonStr);
int transport_allow_mqtt_connect(bool allow);
void transport_init(void);
