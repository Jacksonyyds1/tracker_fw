#pragma once
#include <stdint.h>

int watchdog_init(void);
void watchdog_feed(void);
void watchdog_force(void); // for testing WDT from shell