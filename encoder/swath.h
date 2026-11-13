/* Pack rendered rows into printhead passes and schedule paper feed. */
#ifndef BROTHER_ENCODER_SWATH_H
#define BROTHER_ENCODER_SWATH_H

#include <stdint.h>

struct output_swath;
struct printer_state;
struct raster_band;
struct raster_pass;

void process_raster_pass(struct printer_state *printer, int row_count, struct raster_band *band,
			 uint32_t planes);
void configure_raster_pass(struct printer_state *printer, struct raster_band *band, uint32_t planes,
			   struct raster_pass *pass);
void buffer_interlaced_row(struct printer_state *printer, struct raster_pass *pass);
void buffer_sequential_row(struct printer_state *printer, struct raster_pass *pass);
uint32_t is_last_swath(struct printer_state *printer, struct raster_pass *pass);
uint32_t mask_randomized_pass(struct printer_state *printer, struct raster_pass *pass,
			      uint8_t *input, uint16_t length, intptr_t channel);
uint8_t mask_edge_pass(struct printer_state *printer, const struct output_swath *reserved_swath,
		       struct raster_pass *pass, uint8_t *input, uint16_t length);
uint32_t use_edge_pass_mask(struct printer_state *printer, struct raster_pass *pass);
void flush_current_swath(struct printer_state *printer, struct raster_pass *pass);
void pad_final_swath(struct printer_state *printer, struct raster_pass *pass);
void flush_remaining_swaths(struct printer_state *printer, struct raster_pass *pass);
uint32_t swath_has_printable_data(const struct output_swath *swath);
void reset_swath_buffer(struct output_swath *swath);
void select_swath_layout(struct printer_state *printer, struct raster_pass *pass);
uint16_t get_swath_count(struct printer_state *printer);
uint16_t get_first_swath_offset(struct printer_state *printer);
void build_row_schedule(struct printer_state *printer, intptr_t output_address, int row_count);
uint8_t choose_interlace_flags(struct printer_state *printer);
uint32_t uses_legacy_interlace(struct printer_state *printer);
uint8_t paper_allows_single_pass(struct printer_state *printer);
void initialize_swath_buffers(struct printer_state *printer);

#endif
