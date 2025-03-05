#
# Copyright (c) 2020 Nordic Semiconductor
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
#

cmake_minimum_required(VERSION 3.20.0)

list(APPEND CONF_FILE prj.conf)

if (EXISTS prj.local.conf)
  list(APPEND CONF_FILE prj.local.conf)
endif()
if (CONFIG_RELEASE_BUILD)
  message("Doing a RELEASE build")
  list(APPEND CONF_FILE prj.release.conf)
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(purina_d1_lte  VERSION 1.1.12)
zephyr_library_include_directories(src/include)




target_sources(app PRIVATE src/main.c)
target_sources(app PRIVATE src/wi.c)
target_sources(app PRIVATE src/status/status.c)
target_sources(app PRIVATE src/network/network.c)
target_sources(app PRIVATE src/zbus/zbus_msgs.c)
target_sources(app PRIVATE src/zbus/shutdown.c)
target_sources(app PRIVATE src/spi/spis_interface.c)
target_sources(app PRIVATE src/purina_iot/transport.c)
target_sources(app PRIVATE src/purina_iot/fota.c)
target_sources(app PRIVATE src/purina_iot/nrf91_upgrade.c)
add_subdirectory(src/gps)



zephyr_include_directories(src)
zephyr_include_directories(src/include)
zephyr_include_directories(src/spi)
zephyr_include_directories(src/zbus)
zephyr_include_directories(src/network)
zephyr_include_directories(src/status)
zephyr_include_directories(src/purina_iot)
zephyr_include_directories(../c_modules/modem/include)
zephyr_include_directories(src/gps)
target_include_directories(app PRIVATE ${ZEPHYR_BASE}/../nrf/ext/curl/include/)
target_include_directories(app PRIVATE ${ZEPHYR_BASE}/include/zephyr/posix/)
add_definitions(-DAPP_VERSION_MAJOR_CMAKE=${PROJECT_VERSION_MAJOR})
add_definitions(-DAPP_VERSION_MINOR_CMAKE=${PROJECT_VERSION_MINOR})
add_definitions(-DAPP_VERSION_PATCH_CMAKE=${PROJECT_VERSION_PATCH})



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

