/* Input-band geometry, page transformations and halftone dispatch. */
#ifndef BROTHER_ENCODER_RASTER_H
#define BROTHER_ENCODER_RASTER_H

#include <stdbool.h>
#include <stdint.h>

struct job_settings;
struct page_state;
struct printer_state;
struct raster_band;

uint32_t clear_paper_margins(struct job_settings *settings, struct page_state *page);
uint32_t whiten_band_edges(uint8_t *pixels, short top_rows, short bottom_rows, short page_height,
			   int row_bytes);
uint32_t rotate_envelope_band(struct job_settings *settings, struct page_state *page);
uint32_t rotate_landscape_band(struct job_settings *settings, struct page_state *page);
bool allocate_band_workspace(struct page_state *page, uint8_t *pixels);
void free_band_workspace(void);
void prepare_band_descriptor(struct page_state *page);
void render_halftone_band(struct printer_state *printer, struct raster_band *band);
void clear_halftone_planes(struct printer_state *printer);
int find_first_ink_column(struct printer_state *printer, struct raster_band *band);
int find_last_ink_column(struct printer_state *printer, struct raster_band *band);
bool is_plain_normal_raster(struct printer_state *printer);

#endif
