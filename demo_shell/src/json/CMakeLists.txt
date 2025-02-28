# SPDX-License-Identifier: Apache-2.0

target_sources(app PRIVATE src/d1_json.c src/d1_json_shell.c)
zephyr_library_include_directories(include)
