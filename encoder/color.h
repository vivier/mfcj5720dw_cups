/* BCM profiles, transfer curves, RGB-to-ink conversion and ink limits. */
#ifndef BROTHER_ENCODER_COLOR_H
#define BROTHER_ENCODER_COLOR_H

#include <stdint.h>

struct printer_state;

int load_bcm_file(struct printer_state *printer, char *path);
uint32_t load_bcm_table(struct printer_state *printer, int table_index);
uintptr_t select_color_tables(struct printer_state *printer, short color_mode);
void build_rgb_adjustment_tables(struct printer_state *printer);
void update_color_tables(struct printer_state *printer);
void choose_color_table_tags(struct printer_state *printer, short color_mode);
void select_fallback_color_tables(struct printer_state *printer, short color_mode);
uint32_t interpolate_scaled_10bit_curve(uint16_t count, intptr_t table, uint16_t input,
					uint16_t *output, float scale);
uint32_t interpolate_scaled_8bit_curve(short count, intptr_t table, uint16_t input,
				       uint16_t *output, float scale);
uint32_t interpolate_10bit_curve(uint16_t count, intptr_t table, uint16_t input, uint16_t *output);
uint32_t interpolate_8bit_curve(short count, intptr_t table, uint16_t input, uint16_t *output);
uint32_t interpolate_signed_10bit_curve(uint16_t count, intptr_t table, uint16_t input,
					uint16_t *output);
void build_ink_transfer_curves(struct printer_state *printer);
uint16_t *apply_ink_limit(short *input, uint16_t *output, struct printer_state *printer);
uint32_t *interpolate_rgb_to_ink(uint32_t *result, uint8_t *rgb, short *channels,
				 struct printer_state *printer);
void apply_row_ink_limit(short *channels, uint32_t reserved, uint32_t packed_limit,
			 uint16_t lower_threshold, uint16_t upper_threshold);

#endif
