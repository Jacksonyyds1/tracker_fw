# Copyright (c) 2023 Culvert Engineering Inc
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

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

project(imu_test VERSION 0.0.2)
target_sources(app PRIVATE src/main.c src/board.c src/shell.c)
add_subdirectory(src/imu)
add_subdirectory(src/storage)
add_subdirectory(src/ble)

add_definitions(-DAPP_VERSION_MAJOR=${PROJECT_VERSION_MAJOR})
add_definitions(-DAPP_VERSION_MINOR=${PROJECT_VERSION_MINOR})
add_definitions(-DAPP_VERSION_PATCH=${PROJECT_VERSION_PATCH})
