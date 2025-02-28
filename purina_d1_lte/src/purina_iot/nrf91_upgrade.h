#pragma once

int nrf91_start_upgrade();
int nrf91_upgrade_with_mem_chunk(uint8_t *chunk, uint16_t chunk_size);
int nrf91_finish_upgrade();
int nrf91_cancel_upgrade();
