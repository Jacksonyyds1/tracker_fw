/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint64_t utils_get_currentmillis(void);
int      utils_set_currentmillis(uint64_t currentmillis);
uint64_t utils_get_currentmicros(void);
int      utils_load_setting(const char *name, void *dest, size_t len);
uint64_t utils_get_utc(void);

#ifdef __cplusplus
}
#endif
