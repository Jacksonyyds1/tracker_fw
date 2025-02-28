# SPDX-License-Identifier: Apache-2.0

target_sources(app PRIVATE src/modem.c src/modem_spi.c src/modem_shell.c src/modem_init.c src/gps.c)
zephyr_library_include_directories(include)
