# Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved
# SPDX-License-Identifier: LicenseRef-Proprietary

target_sources(app PRIVATE src/ble.c src/tracker_service.c)
zephyr_library_include_directories(include)
