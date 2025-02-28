# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

set(DTC_OVERLAY_FILE ../boards/${BOARD}.overlay)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(blinky)

# Enable C++ support
enable_language(CXX)

# Add the libs directory to the include path
target_include_directories(app PRIVATE ${CMAKE_SOURCE_DIR}/libs)

# Link the libedgeimpulse.a library
target_link_libraries(app PRIVATE ${CMAKE_SOURCE_DIR}/libs/libAPI_NRF5340.a)

# Specify C++ source files
target_sources(app PRIVATE src/main.cpp) # Change the file extension to .cpp

# Add any additional C++ source files
# target_sources(app PRIVATE src/other.cpp)
