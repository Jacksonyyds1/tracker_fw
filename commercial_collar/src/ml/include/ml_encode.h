/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */
/*
 * Generated using zcbor version 0.7.0
 * https://github.com/NordicSemiconductor/zcbor
 * Generated with a --default-max-qty of 3
 */

#ifndef ML_ENCODE_H__
#define ML_ENCODE_H__

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "ml_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#if DEFAULT_MAX_QTY != 3
#error "The type file was generated with a different default_max_qty than this file"
#endif

int cbor_encode_Inference(uint8_t *payload, size_t payload_len, const struct Inference *input, size_t *payload_len_out);

#ifdef __cplusplus
}
#endif

#endif /* ML_ENCODE_H__ */
