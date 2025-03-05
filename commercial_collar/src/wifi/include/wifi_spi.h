/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */
/****************************************************************
 *  wifi_spi.h
 *  SPDX-License-Identifier: LicenseRef-Proprietary
 *
 * The wifi code is layered as follows:
 *     wifi_spi.c or wifi_uart.c - Talked to hardware to transfer data to and from the DA1200
 *     wifi.c - HW independent layer to send adn receive messages to and from the DA1200
 *     wifi_at.c - AT command layer to send and receive AT commands to and from the DA1200
 *     net_mgr.c - The network (wifi and lte) api layer, manages the state of the da and
 *                 lte chip and network communication state machine and publishes zbus
 *                 message when revevent stuff happens
 *     wifi_shell.c - A collection of shell commands to test and debug during development
 */
#pragma once
#include <stddef.h>
#include "wifi.h"

int  wifi_spi_init(void);
void wifi_spi_msg_free(wifi_msg_t *msg);
void wifi_spi_flush_msgs();
int  wifi_spi_recv(wifi_msg_t *msg, k_timeout_t timeout);

//////////////////////////////////////////////////////////
// wifi_spi_send
//
// Write an AT or ESC command to the DA16200.
//
//  @param data - pointer to buffer containing the AT command
//
//  @return - 0 on success,
//           -errno on error
int wifi_spi_send(char *data);

//////////////////////////////////////////////////////////
//	Requeue a message that was recv'd
//
// @param msg  - a wifi_msg_t that was obtained
//               from wifi_spi_recv()
//
// @note caller can call this instead of wifi_msg_free()
void wifi_spi_requeue(wifi_msg_t *msg);
void wifi_spi_set_print_txrx(bool print);
int  wifi_spi_msg_cnt();

//////////////////////////////////////////////////////////
// wifi_spi_add_tx_rx_cb
//
// Add a callback to be called when a message is received
//
// @param cb - callback to call when a message is received
// @param user_data - user data to pass to the callback
//
// @return - On success a >0 id to be used when removing the
//           callback, -1 on error
// @note the callback should return true if it consumed the
//       message and false if it didn't
int wifi_spi_add_tx_rx_cb(wifi_tx_rx_cb_t cb, void *user_data);

//////////////////////////////////////////////////////////
// wifi_spi_rem_tx_rx_cb
//
// Remove a callback that was added with wifi_spi_add_tx_rx_cb
//
// @param id - id returned from wifi_spi_add_tx_rx_cb
void wifi_spi_rem_tx_rx_cb(int id);
