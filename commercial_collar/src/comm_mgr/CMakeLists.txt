# SPDX-License-Identifier: Apache-2.0

target_sources(app PRIVATE src/radioMgr.c src/commMgr.c src/fota.c src/fota_shell.c src/log_telemetry.c)
zephyr_library_include_directories(include)
