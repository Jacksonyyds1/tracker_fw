# SPDX-License-Identifier: Apache-2.0

target_sources(app PRIVATE src/5340_interface.c src/spis_interface.c src/uart_interface.c)
zephyr_library_include_directories(include)
