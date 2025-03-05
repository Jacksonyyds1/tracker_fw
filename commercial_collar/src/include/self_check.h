/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */
#pragma once
#include <stdbool.h>

typedef enum
{
    SELF_CHECK_PASS        = 0,
    SELF_CHECK_IMU_FAILURE = -1,

    // TODO: add more failure codes here

} self_check_enum_t;

self_check_enum_t self_check(void);
