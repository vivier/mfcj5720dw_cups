/* Track ink coverage and decide whether a row needs correction. */
#ifndef BROTHER_ENCODER_ROW_CORRECTION_H
#define BROTHER_ENCODER_ROW_CORRECTION_H

#include <stdbool.h>
#include <stdint.h>

void initialize_row_correction(int *correction, uint16_t column_span, uint16_t row_span,
			       uint64_t edge_chroma_bits, int edge_dark_low, int edge_dark_high,
			       uint64_t interior_chroma_bits, uint64_t interior_dark_bits,
			       int page_rows, int black_threshold, int page_width);
void reset_correction_row(intptr_t correction);
void advance_correction_row(intptr_t correction);
void accumulate_ink_coverage(int *correction, int cyan_magenta, int yellow_black, uint32_t reserved,
			     uint32_t column);
int evaluate_row_correction(int *correction);
bool needs_row_correction(int *correction);

#endif
