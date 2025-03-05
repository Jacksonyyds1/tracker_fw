# Copyright (c) 2021 Nordic Semiconductor ASA
# SPDX-License-Identifier: Apache-2.0
#
# This CMake file is picked by the Zephyr build system because it is defined
# as the module CMake entry point (see zephyr/module.yml).

zephyr_include_directories(include)

get_filename_component(APPLICATION_PROJECT_DIR
    ${CMAKE_CURRENT_LIST_DIR}/../../..
    ABSOLUTE
)

list(APPEND BOARD_ROOT ${APPLICATION_PROJECT_DIR})
list(APPEND DTS_ROOT ${APPLICATION_PROJECT_DIR})
message(BOARD_ROOT="${BOARD_ROOT}")
message(DTS_ROOT="${DTS_ROOT}")
