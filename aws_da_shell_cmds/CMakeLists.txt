# Copyright (c) 2023 Nordic Semiconductor ASA
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

set(DTC_OVERLAY_FILE ./nrf5340dk_nrf5340_cpuapp_mfg.overlay)

get_filename_component(APPLICATION_PROJECT_DIR
    ${CMAKE_CURRENT_LIST_DIR}/..
    ABSOLUTE
)


list(APPEND BOARD_ROOT ${APPLICATION_PROJECT_DIR})
list(APPEND DTS_ROOT ${APPLICATION_PROJECT_DIR})
message(ADDING BOARD_ROOT="${BOARD_ROOT}")
message(ADDING DTS_ROOT="${DTS_ROOT}")



find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(wifi_aws VERSION 0.0.1)
zephyr_library_include_directories(src/include)
target_sources(app PRIVATE src/main.c src/board.c src/shell.c)
add_subdirectory(src/pmic)
add_subdirectory(src/rffe)
add_subdirectory(src/modem)
add_subdirectory(src/wifi)
add_subdirectory(src/watchdog)

add_definitions(-DAPP_VERSION_MAJOR=${PROJECT_VERSION_MAJOR})
add_definitions(-DAPP_VERSION_MINOR=${PROJECT_VERSION_MINOR})
add_definitions(-DAPP_VERSION_PATCH=${PROJECT_VERSION_PATCH})

# Get the current Git commit hash
execute_process(
  COMMAND git rev-parse --short=6 HEAD
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  OUTPUT_VARIABLE GIT_HASH
  OUTPUT_STRIP_TRAILING_WHITESPACE
)

# Check if the repository is dirty
execute_process(
  COMMAND git status --porcelain
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  OUTPUT_VARIABLE GIT_DIRTY_STATUS
  OUTPUT_STRIP_TRAILING_WHITESPACE
)

# Append '-dirty' to the version string if the repository is dirty
if(NOT GIT_DIRTY_STATUS STREQUAL "")
  set(GIT_HASH "${GIT_HASH}-dirty")
endif()

add_definitions(-DGIT_HASH_CMAKE=\"${GIT_HASH}\")

# Get the build machine name
execute_process(
  COMMAND hostname
  OUTPUT_VARIABLE BUILD_MACHINE
  OUTPUT_STRIP_TRAILING_WHITESPACE
)

# Get the build date
string(TIMESTAMP BUILD_DATE "%Y-%m-%d %H:%M:%S")

# Pass the variables to the code
add_definitions(-DBUILD_MACHINE_CMAKE=\"${BUILD_MACHINE}\")
add_definitions(-DBUILD_DATE_CMAKE=\"${BUILD_DATE}\")