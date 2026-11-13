/* JPEG band compression and encoder cleanup. */
#ifndef BROTHER_ENCODER_JPEG_H
#define BROTHER_ENCODER_JPEG_H

#include <stdint.h>

struct printer_state;
struct raster_band;

uint32_t render_jpeg_band(struct printer_state *printer, struct raster_band *band);
void free_jpeg_encoder(struct printer_state *printer);

#endif
