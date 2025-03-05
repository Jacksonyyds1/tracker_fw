#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <zephyr/kernel.h>
#include "modem_interface_types.h"

int gps_init();
gps_info_t* getGPSFromFifo();