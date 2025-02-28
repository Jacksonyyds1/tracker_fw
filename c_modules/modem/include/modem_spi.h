/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */
#pragma once
#include <stddef.h>


int  modem_spi_init(void);
int  modem_spi_recv(char *buf, k_timeout_t timeout);
void modem_spi_set_rx_cb(void (*cb)(uint8_t *data, size_t len, void *user_data), void *user_data);
int  modem_spi_send_command(modem_message_type_t type, uint8_t *data, uint16_t dataLen, bool reply_requested);
int  modem_spi_recv_resp(uint8_t handle, uint8_t *data, uint16_t *dataLen, int timeout);
int  modem_spi_free_reply_data(int handle);
int  send_spi_command(uint8_t cmd);

int                 modem_spi_get_IMEI(uint8_t *buf, uint16_t len);
int                 modem_spi_get_VERSION(uint8_t *buf, uint16_t len);
int                 modem_spi_get_ICCID(uint8_t *buf, uint16_t len);
version_response_t *modem_spi_get_cached_version();
void                send_version_no_reply();
void                send_status_no_reply();
uint32_t            merge_shadow_status(modem_status_t *status);
cell_info_t         modem_spi_getCellInfo();
int                 modem_spi_stop_waiting_for_resp(int handle);
int                 modem_spi_free_reply_data_but_not_handle(int handle);
void                copy_modem_status(modem_status_t *src, modem_status_t *dst);
void                clear_cell_info();