/* Error-diffusion initialization, row rendering and directional kernels. */
#ifndef BROTHER_ENCODER_DIFFUSION_H
#define BROTHER_ENCODER_DIFFUSION_H

#include <stdint.h>

struct printer_state;
struct raster_band;

uint32_t render_binary_diffusion(struct printer_state *printer, int first_row, uint32_t width,
				 struct raster_band *band, uint32_t row_count, int first_column,
				 int column_count);
uint32_t render_four_level_diffusion(struct printer_state *printer, int first_row, uint32_t width,
				     struct raster_band *band, uint32_t row_count, int first_column,
				     int column_count);
uint32_t render_five_level_diffusion(struct printer_state *printer, int first_row, uint32_t width,
				     struct raster_band *band, uint32_t row_count, int first_column,
				     int column_count);
uint32_t render_fast_binary_diffusion(struct printer_state *printer, int first_row, uint32_t width,
				      struct raster_band *band, uint32_t row_count,
				      int first_column, int column_count);
uint32_t render_adaptive_diffusion(struct printer_state *printer, int first_row, uint32_t width,
				   struct raster_band *band, uint32_t row_count, int first_column,
				   int column_count);
uint32_t render_photo_diffusion(struct printer_state *printer, int first_row, uint32_t width,
				struct raster_band *band, uint32_t row_count, int first_column,
				int column_count);
uint32_t render_best_diffusion(struct printer_state *printer, int first_row, uint32_t width,
			       struct raster_band *band, uint32_t row_count, int first_column,
			       int column_count);
void diffuse_binary_forward(short *halftone);
void diffuse_binary_reverse(short *halftone);
void diffuse_four_level_forward(short *halftone);
void diffuse_four_level_reverse(short *halftone);
void diffuse_five_level_forward(short *halftone);
void diffuse_five_level_reverse(short *halftone);
void diffuse_adaptive_forward(short *halftone);
void diffuse_adaptive_reverse(short *halftone);
void diffuse_photo_forward(short *halftone);
void diffuse_photo_reverse(short *halftone);
void initialize_diffusion_buffers(struct printer_state *printer, uint32_t width,
				  int expanded_width);

#endif
