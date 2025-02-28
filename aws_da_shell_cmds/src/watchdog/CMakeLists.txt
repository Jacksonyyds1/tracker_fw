# SPDX-License-Identifier: Apache-2.0

target_sources(app PRIVATE src/watchdog.c src/watchdog_shell.c)
zephyr_library_include_directories(include)
