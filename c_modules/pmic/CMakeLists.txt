# SPDX-License-Identifier: Apache-2.0

target_sources(app PRIVATE src/pmic.c src/pmic_leds.c src/pmic_shell.c)
zephyr_library_include_directories(include)
