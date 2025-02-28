/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */
/*
 * Generated using zcbor version 0.7.0
 * https://github.com/NordicSemiconductor/zcbor
 * Generated with a --default-max-qty of 3
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "zcbor_encode.h"
#include "ml_encode.h"

#if DEFAULT_MAX_QTY != 3
#error "The type file was generated with a different default_max_qty than this file"
#endif

static bool encode_Inference(zcbor_state_t *state, const struct Inference *input);

static bool encode_Inference(zcbor_state_t *state, const struct Inference *input)
{
    zcbor_print("%s\r\n", __func__);

    bool tmp_result =
        (((zcbor_list_start_encode(state, 9)
           && (((((((*input)._Inference_activity <= UINT8_MAX)) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))
                 && (zcbor_uint32_encode(state, (&(*input)._Inference_activity))))
                && ((zcbor_float32_encode(state, (&(*input)._Inference_reps))))
                && ((zcbor_float32_encode(state, (&(*input)._Inference_probability))))
                && (((((((*input)._Inference_start <= UINT64_MAX)) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false)))
                     || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))
                    && (zcbor_uint64_encode(state, (&(*input)._Inference_start))))
                && (((((((*input)._Inference_end <= UINT64_MAX)) || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false)))
                     || (zcbor_error(state, ZCBOR_ERR_WRONG_RANGE), false))
                    && (zcbor_uint64_encode(state, (&(*input)._Inference_end))))
                && ((zcbor_float32_encode(state, (&(*input)._Inference_am))))
                && ((zcbor_float32_encode(state, (&(*input)._Inference_gm))))
                && ((zcbor_float32_encode(state, (&(*input)._Inference_as))))
                && ((zcbor_float32_encode(state, (&(*input)._Inference_gs)))))
               || (zcbor_list_map_end_force_encode(state), false))
           && zcbor_list_end_encode(state, 9))));

    if (!tmp_result)
        zcbor_trace();

    return tmp_result;
}

int cbor_encode_Inference(uint8_t *payload, size_t payload_len, const struct Inference *input, size_t *payload_len_out)
{
    zcbor_state_t states[3];

    zcbor_new_state(states, sizeof(states) / sizeof(zcbor_state_t), payload, payload_len, 1);

    bool ret = encode_Inference(states, input);

    if (ret && (payload_len_out != NULL)) {
        *payload_len_out = MIN(payload_len, (size_t)states[0].payload - (size_t)payload);
    }

    if (!ret) {
        int err = zcbor_pop_error(states);

        zcbor_print("Return error: %d\r\n", err);
        return (err == ZCBOR_SUCCESS) ? ZCBOR_ERR_UNKNOWN : err;
    }
    return ZCBOR_SUCCESS;
}
