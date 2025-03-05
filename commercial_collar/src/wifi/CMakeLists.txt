# Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved
# SPDX-License-Identifier: LicenseRef-Proprietary

target_sources(app PRIVATE src/wifi.c src/wifi_spi.c src/wifi_shell.c src/wifi_at.c src/net_mgr.c)
zephyr_library_include_directories(include)
