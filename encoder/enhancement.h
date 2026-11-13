/* Image statistics, tone curves and RGB enhancement. */
#ifndef BROTHER_ENCODER_ENHANCEMENT_H
#define BROTHER_ENCODER_ENHANCEMENT_H

#include <stdint.h>

struct printer_state;
struct raster_band;

uint32_t enhance_row_region(struct printer_state *printer, uint32_t first_row, uint32_t width,
			    struct raster_band *band, uint32_t row_count, uint32_t first_column,
			    uint32_t column_count);
void collect_band_histogram(struct printer_state *printer, int first_row, uint32_t width,
			    struct raster_band *band, int row_count, int first_column,
			    int column_count);
void build_enhancement_curves(struct printer_state *printer);
void apply_row_enhancement(struct printer_state *printer, int first_row, uint32_t width,
			   struct raster_band *band, int row_count, int first_column,
			   int column_count);
void initialize_color_enhancement(struct printer_state *printer);
void configure_color_enhancement(struct printer_state *printer);

#endif
