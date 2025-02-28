/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */
/*
 *
 * SPDX-License-Identifier: LicenseRef-Proprietary
 */

#include <zephyr/logging/log.h>
#include "self_check.h"

LOG_MODULE_REGISTER(self_check, LOG_LEVEL_DBG);

self_check_enum_t
self_check(void)
{
    // TODO: perform some checks here
    return SELF_CHECK_PASS;
}
