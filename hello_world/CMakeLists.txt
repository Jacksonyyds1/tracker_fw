# Copyright (c) 2023 Nordic Semiconductor ASA
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

#set(SHIELD npm1300_ek)
set(DTC_OVERLAY_FILE ../boards/${BOARD}.overlay)

get_filename_component(APPLICATION_PROJECT_DIR
    ${CMAKE_CURRENT_LIST_DIR}/..
    ABSOLUTE
)


list(APPEND BOARD_ROOT ${APPLICATION_PROJECT_DIR})
list(APPEND DTS_ROOT ${APPLICATION_PROJECT_DIR})
message(ADDING BOARD_ROOT="${BOARD_ROOT}")
message(ADDING DTS_ROOT="${DTS_ROOT}")



find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(hello_world)
target_sources(app PRIVATE src/main.c)
