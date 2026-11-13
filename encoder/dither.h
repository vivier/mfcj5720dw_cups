/* Threshold-matrix selection and packed-dot rendering. */
#ifndef BROTHER_ENCODER_DITHER_H
#define BROTHER_ENCODER_DITHER_H

#include <stdint.h>

struct printer_state;
struct raster_band;

void assign_channel_dither_matrices(struct printer_state *printer);
void select_dither_matrix(struct printer_state *printer);
uint32_t set_dither_density_factors(struct printer_state *printer);
uint32_t render_five_level_dither(struct printer_state *printer, int first_row, uint32_t width,
				  struct raster_band *band, uint32_t row_count, int first_column,
				  int column_count);
uint32_t render_four_level_dither(struct printer_state *printer, int first_row, uint32_t width,
				  struct raster_band *band, uint32_t row_count, int first_column,
				  int column_count);
uint32_t render_fast_four_level_dither(struct printer_state *printer, int first_row, uint32_t width,
				       struct raster_band *band, uint32_t row_count,
				       int first_column, int column_count);
uint32_t render_binary_dither(struct printer_state *printer, int first_row, uint32_t width,
			      struct raster_band *band, uint32_t row_count, int first_column,
			      int column_count);
uint32_t render_fast_binary_dither(struct printer_state *printer, int first_row, uint32_t width,
				   struct raster_band *band, uint32_t row_count, int first_column,
				   int column_count);

#endif
