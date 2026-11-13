/* Select the calibrated renderer and configure its ink channels. */
#ifndef BROTHER_ENCODER_HALFTONE_H
#define BROTHER_ENCODER_HALFTONE_H

struct printer_state;

void configure_halftone_channels(struct printer_state *printer);

#endif
