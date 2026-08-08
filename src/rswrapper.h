/**
 * @file src/rswrapper.h
 * @brief Wrappers for nanors vectorization
 * @details This is a drop-in replacement for nanors rs.h
 */
#pragma once

// standard includes
#include <stddef.h>
#include <stdint.h>

#define DATA_SHARDS_MAX 255

typedef struct _reed_solomon reed_solomon;

typedef reed_solomon *(*reed_solomon_new_t)(int data_shards, int parity_shards);
typedef void (*reed_solomon_release_t)(reed_solomon *rs);
typedef int (*reed_solomon_encode_t)(reed_solomon *rs, uint8_t **shards, int nr_shards, int bs);
typedef int (*reed_solomon_decode_t)(reed_solomon *rs, uint8_t **shards, uint8_t *marks, int nr_shards, int bs);

extern reed_solomon_new_t reed_solomon_new_fn;
extern reed_solomon_release_t reed_solomon_release_fn;
extern reed_solomon_encode_t reed_solomon_encode_fn;
extern reed_solomon_decode_t reed_solomon_decode_fn;

#define reed_solomon_new reed_solomon_new_fn
#define reed_solomon_release reed_solomon_release_fn
#define reed_solomon_encode reed_solomon_encode_fn
#define reed_solomon_decode reed_solomon_decode_fn

/**
 * @brief This initializes the RS function pointers to the best vectorized version available.
 * @details The streaming code will directly invoke these function pointers during encoding.
 */
void reed_solomon_init(void);

/**
 * @brief Overwrites the RS parity/generator matrix directly.
 * @details Used by the audio broadcast path to replace the computed matrix with a
 * known-good one matching Nvidia's implementation - see the call site in stream.cpp
 * for why. Kept as an ordinary (non-ISA-variant) function since it's a plain memcpy,
 * not a vectorized operation, and exists specifically so callers never need the full
 * reed_solomon struct layout visible - that stays internal to rswrapper.c.
 */
void reed_solomon_set_matrix(reed_solomon *rs, const uint8_t *matrix, size_t size);
