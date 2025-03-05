# Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved
# SPDX-License-Identifier: LicenseRef-Proprietary

target_sources(app PRIVATE src/d1_json.c src/d1_json_shell.c)
zephyr_library_include_directories(include)
