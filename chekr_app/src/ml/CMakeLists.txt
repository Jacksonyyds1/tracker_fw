# SPDX-License-Identifier: Apache-2.0

target_sources(app PRIVATE src/ml.cpp src/ml_test.cpp)
zephyr_library_include_directories(include)

set(LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/libs)
add_library(ml_lib STATIC IMPORTED GLOBAL)
set_target_properties(ml_lib PROPERTIES IMPORTED_LOCATION ${LIB_DIR}/libAPI_NRF5340_v3_1_0.a)
target_link_libraries(app PUBLIC ml_lib)
