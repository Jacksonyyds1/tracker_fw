# Copyright (c) 2023 Nordic Semiconductor ASA
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

set(DTC_OVERLAY_FILE ./nrf9160dk_nrf9160_ns_mfg.overlay)

get_filename_component(APPLICATION_PROJECT_DIR
    ${CMAKE_CURRENT_LIST_DIR}/..
    ABSOLUTE
)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

list(APPEND BOARD_ROOT ${APPLICATION_PROJECT_DIR})
list(APPEND DTS_ROOT ${APPLICATION_PROJECT_DIR})
message(ADDING BOARD_ROOT="${BOARD_ROOT}")
message(ADDING DTS_ROOT="${DTS_ROOT}")



find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(mfg_at_client VERSION 0.0.1)
target_sources(app PRIVATE src/main.c)
add_subdirectory(src/5340Interface)

add_definitions(-DAPP_VERSION_MAJOR=${PROJECT_VERSION_MAJOR})
add_definitions(-DAPP_VERSION_MINOR=${PROJECT_VERSION_MINOR})
add_definitions(-DAPP_VERSION_PATCH=${PROJECT_VERSION_PATCH})


