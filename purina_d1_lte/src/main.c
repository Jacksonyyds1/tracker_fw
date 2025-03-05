/*
 * Copyright (c) 2020 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */
#include "app_version.h"
#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <hal/nrf_gpio.h>
#include <zephyr/sys/iterable_sections.h>
#include <zephyr/shell/shell.h>
#include <stdio.h>
#include <stdlib.h>
#include <hw_id.h>
#include <zephyr/pm/device.h>
#include "transport.h"
#include "zbus_msgs.h"
#include "status.h"
#include <zephyr/sys/heap_listener.h>

#include <hal/nrf_uarte.h>
#include "wi.h"

/* Register log module */
LOG_MODULE_REGISTER(purina_d1_lte, CONFIG_PURINA_D1_LTE_LOG_LEVEL);

#define CONSOLE DT_CHOSEN(zephyr_console)

bool disable_modem = true;

#ifdef CONFIG_SYS_HEAP_LISTENER

extern struct k_heap _system_heap;

#define N_HEAP_BLKS 256
struct heap_usage {
	void *ptr;
	size_t bytes;
} heap_usage[N_HEAP_BLKS];

void on_heap_alloc(uintptr_t heap_id, void *mem, size_t bytes)
{
	for (int i = 0; i < N_HEAP_BLKS; i++) {
		if (heap_usage[i].ptr == NULL) {
			heap_usage[i].ptr = mem;
			heap_usage[i].bytes = bytes;
			return;
		}
	}
	LOG_INF("Too many allocated blocks to track! alloc %zu at %p", bytes, mem);
}
HEAP_LISTENER_ALLOC_DEFINE(alloc_listener, HEAP_ID_FROM_POINTER(&_system_heap.heap), on_heap_alloc);

void on_heap_free(uintptr_t heap_id, void *mem, size_t bytes)
{
	for (int i = 0; i < N_HEAP_BLKS; i++) {
		if (heap_usage[i].ptr == mem) {
			heap_usage[i].ptr = NULL;
			heap_usage[i].bytes = 0;
			return;
		}
	}
	LOG_INF("Freeing unallocated mem @ %p", mem);
}
HEAP_LISTENER_FREE_DEFINE(free_listener, HEAP_ID_FROM_POINTER(&_system_heap.heap), on_heap_free);

void do_print_heap(const struct shell *sh, int argc, char **argv)
{
	for (int i = 0; i < N_HEAP_BLKS; i++) {
		if (heap_usage[i].ptr) {
			shell_print(sh, "Alloc %zu @ %p", heap_usage[i].bytes, heap_usage[i].ptr);
		}
	}
}
 SHELL_CMD_REGISTER(heap, NULL, "Print heap blocks in use", do_print_heap);
#endif


int main(void)
{
	LOG_INF("---- Nestle D1 LTE -  version %d.%d.%d (%s)", APP_VERSION_MAJOR,
                APP_VERSION_MINOR, APP_VERSION_PATCH, GIT_HASH);
    LOG_INF("---- Built on %s by %s\r\n\r\n", DBUILD_DATE, DBUILD_MACHINE);

	wr_init();

	/* Mark image as working to avoid reverting to the former image after a reboot. */
#if defined(CONFIG_BOOTLOADER_MCUBOOT)
    // TODO do all the self checks before you call this
	boot_write_img_confirmed();
#endif

#ifdef CONFIG_SYS_HEAP_LISTENER
	heap_listener_register(&alloc_listener);
	heap_listener_register(&free_listener);
#endif


	// nrf_gpio_cfg_input(DT_GPIO_PIN(DT_NODELABEL(wake), gpios), NRF_GPIO_PIN_PULLUP);
	// nrf_gpio_cfg_sense_set(DT_GPIO_PIN(DT_NODELABEL(wake), gpios), NRF_GPIO_PIN_SENSE_LOW);

	transport_init();

	return 0;
}


// TODOs
// X get/update RSSI periodically
// - get/update list of towers? (https://developer.nordicsemi.com/nRF_Connect_SDK/doc/1.6.1/nrf/samples/nrf9160/multicell_location/README.html#nrf9160-multicell-location)
// - gps position/num-sats
// X add - get version num to spi commands
// - handle message chunking
// X change subscribe topics to be passed from 5340
// X cleanup subscribe topics so its dynamic
// X can I call modem_info_init earlier?
// X lte connection attempts just stop after a while
// X in mqtt over the spi, currently 5340 is putting \0 at the end of both topic and message, working but ugly.
// X something evil lurks in getStatusTime in status.c
// - add a download from url -> send to 5340 function
// X force update of any status info to keep it fresh. and/or timestamp each of the items (gps, time, etc)
// X add send_5k for nick
// X combine the 3 places reboot/shutdown are called into one function
// X remove all device ID references and take one passed from 5340
// X remove all host/url/port and other connections from the code and take them from 5340
// X create telemetry module on 5340, takes in data/events and sends periodically

// at%XCOEX0=1,1,1570,1580     GNSS coexistance

// nrf9160 send_at AT+CFUN=0
// nrf9160 send_at AT\%XMIPIRFFEDEV?
// nrf9160 send_at AT\%XMIPIRFFEDEV=4,4,102,3,184
// nrf9160 send_at AT\%XMIPIRFFECTRL=4,0,1,28,184
// nrf9160 send_at AT\%XMIPIRFFECTRL=4,1,1,28,56,5,1,1,4,4,746,2,2,787,4,4,824,1,1,894,8,8,2200
// nrf9160 send_at AT\%XMIPIRFFECTRL=4,2,1,28,184
// nrf9160 send_at AT\%XMIPIRFFECTRL=4,3,1,28,184
// nrf9160 send_at AT+CFUN=0