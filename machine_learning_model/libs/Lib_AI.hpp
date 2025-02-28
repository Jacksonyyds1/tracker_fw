#ifndef LIB_API_H
#define LIB_API_H

#include <stdint.h>
#include <stddef.h>
#include "baseLib.hpp"

/** Functions ----------------------------------------------------------------*/
/**
 * @brief Initializes the library with the necessary parameters and function pointers.
 * @param inputBufferStart Pointer to the start of the input buffer.
 * @param inputBufferSize Size of the input buffer.
 * @param outputBufferStart Pointer to the start of the output buffer.
 * @param outputBufferSize Size of the output buffer.
 * @param sequenceArrayInputBuffer Pointer to the sequence array for the input buffer.
 * @param sequenceArrayOutputBuffer Pointer to the sequence array for the output buffer.
 * @param sleepFunction Function pointer to the sleep function.
 * @param inputLockFunction Function pointer to the input buffer lock function.
 * @param inputReleaseLockFunction Function pointer to the input buffer release lock function.
 * @param outputLockFunction Function pointer to the output buffer lock function.
 * @param outputReleaseLockFunction Function pointer to the output buffer release lock function.
 * @param jhcInferCallInterval Interval for Joint Health monitoring ML-based inference model (in seconds).
 * @param repInferCallInterval Interval for repetition check model (in seconds).
 * @param pet_size Size of the pet.
 */
void initializeLibrary(
        IMUData* inputBufferStart,
        size_t inputBufferSize,
        InferResult* outputBufferStart,
        size_t outputBufferSize,
        uint64_t* sequenceArrayInputBuffer,
        uint64_t* sequenceArrayOutputBuffer,
        void (*sleep)(),
        void (*inputLockFunction)(),
        void (*inputReleaseLockFunction)(),
        void (*outputLockFunction)(),
        void (*outputReleaseLockFunction)(),
        int jhcInferCallInterval, // Interval for Joint Health monitoring ML-based inference model (in seconds).
        int repInferCallInterval, // Interval for repetition check model (in seconds).
        PetSize pet_size); // Size of the pet.

/**
 * @brief Starts the continuous inference process by running an infinite loop.
 *        Calls the `infer()` function and sleeps after each iteration.
 *        This function is intended to be executed in a separate thread or as the main loop.
 */
void startInferring();

#endif
