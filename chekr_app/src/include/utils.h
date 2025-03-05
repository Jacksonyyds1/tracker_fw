#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint16_t utils_crc16_modbus(const uint8_t *data, uint16_t length);
uint64_t utils_get_currentmillis(void);
uint64_t utils_get_currentmicros(void);
int util_enable_dialog(bool enable);
int util_enable_9160(bool enable);

#ifdef __cplusplus
}
#endif
