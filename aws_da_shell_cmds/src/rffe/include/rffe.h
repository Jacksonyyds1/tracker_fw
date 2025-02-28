#pragma once
#include <stdio.h>
#include <stdint.h>

int RFFE_init();
uint8_t RFFE_write(uint16_t regAddr, uint8_t val);
uint8_t RFFE_read(uint16_t regAddr);