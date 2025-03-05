/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */
#pragma once

#define CONFIG_WDT_NPM1300_TIMEOUT_IN_MS (60000)

int  watchdog_init();
void watchdog_kick();
void watchdog_disable();