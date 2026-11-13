/* Row compression, zero trimming and packed-symbol counting. */
#ifndef BROTHER_ENCODER_COMPRESSION_H
#define BROTHER_ENCODER_COMPRESSION_H

#include <stdint.h>

struct output_swath;
struct raster_pass;

uint32_t append_compressed_row(struct output_swath *swath, intptr_t input, uint32_t length,
			       struct raster_pass *pass);
uint16_t count_leading_zero_bytes(char *input, uint16_t length);
uint16_t trim_trailing_zero_bytes(intptr_t input, uint16_t length);
void count_packed_symbols(intptr_t input, uint16_t length, uint32_t bits_per_symbol, int *counts);

#endif
