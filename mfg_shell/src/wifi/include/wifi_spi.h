#pragma once
#include <stddef.h>
#include "wifi.h"

int wifi_spi_init(void);
void wifi_spi_msg_free(wifi_msg_t *msg);
void wifi_spi_flush_msgs();
int wifi_spi_recv(wifi_msg_t *msg, k_timeout_t timeout);
//////////////////////////////////////////////////////////
// wifi_spi_send
//
// Write an AT or ESC command to the DA16200.
//
//  @param data - pointer to buffer containing the AT command
//
//  @return - 0 on success, -1 on error or timeout
int wifi_spi_send(char *buf);
//////////////////////////////////////////////////////////
// wifi_spi_send_timeout
//
// Write an AT or ESC command to the DA16200.
//
//  @param data - pointer to buffer containing the AT command
//  @param timeout - timeout for the write
//
//  @return - 0 on success, -1 on error or timeout
int wifi_spi_send_timeout(char *data, k_timeout_t timeout);
void wifi_spi_set_rx_cb(wifi_on_rx_cb_t cb, void *user_data);
//////////////////////////////////////////////////////////
//	Requeue a message that was recv'd
//
// @param msg  - a wifi_msg_t that was obtained
//               from wifi_spi_recv()
//
// @note caller can call this instead of wifi_msg_free()
void wifi_spi_requeue(wifi_msg_t *msg);
void wifi_spi_set_print_txrx(bool print);
int wifi_spi_msg_cnt();