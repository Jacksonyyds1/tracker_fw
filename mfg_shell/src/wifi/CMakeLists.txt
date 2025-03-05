# SPDX-License-Identifier: Apache-2.0

target_sources(app PRIVATE src/wifi.c src/wifi_uart.c src/wifi_spi.c src/wifi_shell.c)
zephyr_library_include_directories(include)
