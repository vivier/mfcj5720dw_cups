/* Range-coder state, indexed-band compression and swath framing. */
#ifndef BROTHER_ENCODER_RANGE_CODER_H
#define BROTHER_ENCODER_RANGE_CODER_H

#include <stdbool.h>
#include <stdint.h>

struct output_swath;
struct printer_state;
struct range_encoder;
struct range_parameters;
struct raster_pass;

/* compress_indexed_band() reports bytes through output_length; nonzero means success. */
uint32_t set_range_encoder_length(struct range_encoder *encoder, uint32_t *length);
void reset_range_encoder(struct range_encoder *encoder);
void finish_range_encoder_session(void);
void encode_probability_interval(struct range_encoder *encoder, intptr_t output_address,
				 int lower_bound, uint32_t upper_bound);
void flush_range_encoder(struct range_encoder *encoder, intptr_t output_address);
uint32_t compress_indexed_band(struct range_encoder *encoder, intptr_t input_address,
			       uint8_t *output, uint32_t *output_length);
uint32_t get_range_encoder_capacity(struct range_encoder *encoder, uint32_t *capacity);
uint32_t range_encoder_available(void);
bool initialize_range_encoder(struct range_encoder *encoder, struct range_parameters *parameters);
void emit_range_encoded_swath(struct printer_state *printer, struct raster_pass *pass,
			      struct output_swath *swath);

#endif
