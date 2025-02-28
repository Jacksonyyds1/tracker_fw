/* Copyright (c) 2024, Nestle Purina Pet Care. All rights reserved */
/*
 * Generated using zcbor version 0.7.0
 * https://github.com/NordicSemiconductor/zcbor
 * Generated with a --default-max-qty of 3
 */

#ifndef ML_TYPES_H__
#define ML_TYPES_H__

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Which value for --default-max-qty this file was created with.
 *
 *  The define is used in the other generated file to do a build-time
 *  compatibility check.
 *
 *  See `zcbor --help` for more information about --default-max-qty
 */
#define DEFAULT_MAX_QTY 3

struct Inference
{
    uint32_t _Inference_activity;
    float    _Inference_reps;
    float    _Inference_probability;
    uint64_t _Inference_start;
    uint64_t _Inference_end;
    float    _Inference_am;
    float    _Inference_gm;
    float    _Inference_as;
    float    _Inference_gs;
};

#ifdef __cplusplus
}
#endif

#endif /* ML_TYPES_H__ */
