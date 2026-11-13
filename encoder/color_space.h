/* In-place color-space conversions and chroma adjustment. */
#ifndef BROTHER_ENCODER_COLOR_SPACE_H
#define BROTHER_ENCODER_COLOR_SPACE_H

#include <stdint.h>

void adjust_rgb_chroma(intptr_t color);
void rgb_to_lch(intptr_t color);
void lch_to_rgb(intptr_t color);
void xyz_to_lch(intptr_t color);
void lch_to_xyz(intptr_t color);
void xyz_to_lab(intptr_t color);
void lab_to_xyz(intptr_t color);
long double lab_transfer(double value);
long double inverse_lab_transfer(double value);

#endif
