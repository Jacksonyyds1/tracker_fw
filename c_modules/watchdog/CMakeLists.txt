# Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved
# SPDX-License-Identifier: LicenseRef-Proprietary

target_sources(app PRIVATE src/watchdog.c src/watchdog_shell.c)
zephyr_library_include_directories(include)
