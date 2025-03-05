# SPDX-License-Identifier: Apache-2.0

target_sources(app PRIVATE 
                           src/ble.c
                           src/chekr.c
                           src/chekr_dash.c
                           src/chekr_record.c
                           )

zephyr_library_include_directories(include)
 