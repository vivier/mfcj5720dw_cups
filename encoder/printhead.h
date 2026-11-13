/* Nozzle calibration, per-row thresholds and density correction. */
#ifndef BROTHER_ENCODER_PRINTHEAD_H
#define BROTHER_ENCODER_PRINTHEAD_H

#include <stdbool.h>
#include <stdint.h>

struct printer_state;
struct raster_band;

void load_printhead_parameters(struct printer_state *printer);
void apply_head_density_calibration(struct printer_state *printer);
void prepare_halftone_band(struct printer_state *printer, struct raster_band *band);
void configure_row_correction(struct printer_state *printer);
uint32_t needs_bidirectional_correction(struct printer_state *printer);
uint32_t needs_color_row_correction(struct printer_state *printer);
void load_head_density_calibration(struct printer_state *printer);
uint8_t supports_head_density_calibration(struct printer_state *printer);
bool needs_borderless_density_ramp(struct printer_state *printer);
void select_borderless_density_ramp(struct printer_state *printer, int first_row, int row_count,
				    int enabled);
void decode_head_parameters(struct printer_state *printer, intptr_t parameters);
void set_default_head_parameters(struct printer_state *printer);
void build_diffusion_head_tables(struct printer_state *printer);
void select_diffusion_row_thresholds(struct printer_state *printer, int row);
void select_scaled_row_thresholds(struct printer_state *printer, int row, int row_phase);
void build_dither_head_tables(struct printer_state *printer);
void select_dither_row_thresholds(struct printer_state *printer, int row);
void select_head_density_calibration(struct printer_state *printer);

#endif
