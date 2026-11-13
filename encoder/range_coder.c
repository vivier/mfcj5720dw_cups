/* Static probability coding of indexed rows and channel-swath framing. */
#include "range_coder.h"
#include "data.h"
#include "protocol.h"
#include "state.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum range_constants {
	RANGE_BYTE_BITS = 8,
	RANGE_TOP_BYTE_SHIFT = 24,
	RANGE_NORMALIZATION_LIMIT = 1U << RANGE_TOP_BYTE_SHIFT,
	RANGE_MAX_LEVELS = 5,
	RANGE_MODEL_SYMBOLS = RANGE_MAX_LEVELS + 1,
	RANGE_MAX_ROWS = 432,
	RANGE_COLOR_CHANNELS = 4,
	RANGE_COUNTS_PER_CHANNEL = 5,
	RANGE_PREFIX_BYTES = 5,
	RANGE_TRAILER_BYTES = 4,
	RANGE_CHUNK_BYTES = 4092,
	RANGE_CHUNK_HEADER_BYTES = 4,
	RANGE_MAX_BYTES_PER_SYMBOL = 3,
};

/* Preserve the numeric layout used by the encoder callbacks. */
struct range_state {
	uint32_t range;
	uint32_t low;
	uint32_t cache_empty;
	uint8_t cached_byte;
	uint8_t reserved_cache[3];
	uint32_t pending_bytes;
	uint32_t output_bytes;
	uint32_t total_frequency;
	uint32_t symbol_counts[RANGE_MODEL_SYMBOLS];
};

struct range_swath {
	uint16_t filled_rows;
	uint8_t reserved_prefix[0x14 - 2];
	uint16_t first_byte;
	uint16_t end_byte;
	uint32_t symbol_counts[RANGE_COLOR_CHANNELS][RANGE_COUNTS_PER_CHANNEL];
	uint8_t reserved_tail[4];
};

static struct range_state *encoder_state(struct range_encoder *encoder)
{
	return (void *)encoder->parameters.state;
}

static void put_u32le(uint8_t *output, uint32_t value)
{
	output[0] = value;
	output[1] = value >> 8;
	output[2] = value >> 16;
	output[3] = value >> 24;
}

uint32_t set_range_encoder_length(struct range_encoder *encoder, uint32_t *length)
{
	encoder->parameters.rows = *length;
	return 1;
}

void reset_range_encoder(struct range_encoder *encoder)
{
	struct range_state *state = encoder_state(encoder);

	state->range = UINT32_MAX;
	state->low = 0;
	state->cached_byte = 0;
	state->pending_bytes = 0;
	state->cache_empty = 1;
	state->output_bytes = 0;
	state->total_frequency = 0;
}

void finish_range_encoder_session(void)
{
}

void encode_probability_interval(struct range_encoder *encoder, intptr_t output_address,
				 int lower_bound, uint32_t upper_bound)
{
	struct range_state *state = encoder_state(encoder);
	uint8_t *output = (void *)output_address;
	uint32_t unit = state->range / state->total_frequency;
	uint32_t low;

	/* Give the last interval the division remainder, as in the original coder. */
	if (upper_bound < state->total_frequency)
		state->range = unit * (upper_bound - (uint32_t)lower_bound);
	else
		state->range -= (uint32_t)lower_bound * unit;
	low = state->low + (uint32_t)lower_bound * unit;
	if (low < state->low) {
		/* Carry into the cached byte, then propagate through deferred bytes. */
		state->cached_byte++;
		while (state->pending_bytes) {
			output[state->output_bytes++] = state->cached_byte;
			state->cached_byte = 0;
			state->pending_bytes--;
		}
	}
	state->low = low;
	while (state->range < RANGE_NORMALIZATION_LIMIT) {
		uint8_t high_byte = state->low >> RANGE_TOP_BYTE_SHIFT;

		if (state->cache_empty == 1) {
			state->cached_byte = high_byte;
			state->cache_empty = 0;
		} else if (high_byte == UINT8_MAX) {
			state->pending_bytes++;
		} else {
			output[state->output_bytes++] = state->cached_byte;
			while (state->pending_bytes) {
				output[state->output_bytes++] = UINT8_MAX;
				state->pending_bytes--;
			}
			state->cached_byte = high_byte;
		}
		state->low <<= RANGE_BYTE_BITS;
		state->range <<= RANGE_BYTE_BITS;
	}
}

void flush_range_encoder(struct range_encoder *encoder, intptr_t output_address)
{
	struct range_state *state = encoder_state(encoder);
	uint8_t *output = (void *)output_address;
	unsigned int byte;

	if (state->cache_empty != 1)
		output[state->output_bytes++] = state->cached_byte;
	while (state->pending_bytes) {
		output[state->output_bytes++] = UINT8_MAX;
		state->pending_bytes--;
	}
	/* Finish with the arithmetic low value, most significant byte first. */
	for (byte = 0; byte < RANGE_TRAILER_BYTES; byte++) {
		output[state->output_bytes++] = state->low >> RANGE_TOP_BYTE_SHIFT;
		state->low <<= RANGE_BYTE_BITS;
	}
}

/*
 * With total frequency <= 2^24, a nonempty interval needs at most three
 * normalization bytes. Include both model tables, one cached byte and trailer.
 * This bound also covers blank rows, each encoded as one special symbol.
 */
static bool encoded_capacity(const struct range_parameters *parameters, uint32_t *capacity)
{
	uint64_t symbols;
	unsigned int bits = parameters->bits_per_symbol;

	*capacity = 0;
	if ((bits != 1 && bits != 2 && bits != 4 && bits != 8) || !parameters->alphabet_size ||
	    parameters->alphabet_size > RANGE_MAX_LEVELS || parameters->rows > RANGE_MAX_ROWS ||
	    !parameters->channels || parameters->channels > RANGE_COLOR_CHANNELS)
		return false;
	symbols = (uint64_t)parameters->rows * parameters->row_bytes * (RANGE_BYTE_BITS / bits);
	if (symbols > RANGE_NORMALIZATION_LIMIT)
		return false;
	if (!parameters->row_bytes)
		symbols = parameters->rows;
	*capacity = RANGE_PREFIX_BYTES + 2 * sizeof(uint32_t) * (parameters->alphabet_size + 1) +
		    RANGE_MAX_BYTES_PER_SYMBOL * symbols + 1 + RANGE_TRAILER_BYTES;
	return true;
}

static unsigned int packed_symbol(uint8_t byte, unsigned int index, unsigned int bits)
{
	return (byte >> (RANGE_BYTE_BITS - (index + 1) * bits)) & ((1U << bits) - 1);
}

uint32_t compress_indexed_band(struct range_encoder *encoder, intptr_t input_address,
			       uint8_t *output, uint32_t *output_length)
{
	const struct range_parameters *parameters = &encoder->parameters;
	struct range_state *state = encoder_state(encoder);
	const uint8_t *input = (void *)input_address;
	uint32_t histogram[RANGE_MODEL_SYMBOLS] = {0};
	uint32_t cumulative[RANGE_MODEL_SYMBOLS + 1] = {0};
	bool blank_rows[RANGE_MAX_ROWS];
	unsigned int blank_symbol = parameters->alphabet_size;
	unsigned int bits = parameters->bits_per_symbol;
	unsigned int symbols_per_byte, row, column, symbol, index;
	uint32_t capacity;
	size_t row_stride;

	if (!output_length)
		return 0;
	*output_length = 0;
	if (!state || !output || !encoded_capacity(parameters, &capacity) ||
	    (!input && parameters->rows && parameters->row_bytes))
		return 0;
	reset_range_encoder(encoder);
	symbols_per_byte = RANGE_BYTE_BITS / bits;
	row_stride = (size_t)parameters->row_bytes * parameters->channels;
	memset(state->symbol_counts, 0, (blank_symbol + 1) * sizeof(uint32_t));

	/*
	 * The model replaces all-zero rows with an escape symbol; dot counts still
	 * count their actual zero pixels. Data rows contain only ordinary levels.
	 */
	for (row = 0; row < parameters->rows; row++) {
		uint32_t frequencies[RANGE_MODEL_SYMBOLS] = {0};
		bool blank = true;

		for (column = 0; column < parameters->row_bytes; column++) {
			uint8_t byte = input[row * row_stride + column];

			for (index = 0; index < symbols_per_byte; index++) {
				symbol = packed_symbol(byte, index, bits);
				if (symbol >= blank_symbol)
					return 0;
				frequencies[symbol]++;
				if (symbol)
					blank = false;
			}
		}
		blank_rows[row] = blank;
		if (blank)
			histogram[blank_symbol]++;
		for (symbol = 0; symbol <= blank_symbol; symbol++) {
			state->symbol_counts[symbol] += frequencies[symbol];
			if (!blank)
				histogram[symbol] += frequencies[symbol];
		}
	}
	for (symbol = 0; symbol <= blank_symbol; symbol++)
		cumulative[symbol + 1] = cumulative[symbol] + histogram[symbol];
	state->total_frequency = cumulative[blank_symbol + 1];
	output[0] = blank_symbol;
	put_u32le(output + 1, parameters->pixels);
	for (symbol = 0; symbol <= blank_symbol; symbol++) {
		put_u32le(output + RANGE_PREFIX_BYTES + symbol * sizeof(uint32_t),
			  histogram[symbol]);
		put_u32le(output + RANGE_PREFIX_BYTES +
				  (blank_symbol + 1 + symbol) * sizeof(uint32_t),
			  cumulative[symbol + 1]);
	}
	state->output_bytes = RANGE_PREFIX_BYTES + 2 * (blank_symbol + 1) * sizeof(uint32_t);
	for (row = 0; row < parameters->rows; row++) {
		if (blank_rows[row]) {
			encode_probability_interval(encoder, (intptr_t)output,
						    cumulative[blank_symbol],
						    cumulative[blank_symbol + 1]);
			continue;
		}
		for (column = 0; column < parameters->row_bytes; column++) {
			for (index = 0; index < symbols_per_byte; index++) {
				symbol = packed_symbol(input[row * row_stride + column], index,
						       bits);
				encode_probability_interval(encoder, (intptr_t)output,
							    cumulative[symbol],
							    cumulative[symbol + 1]);
			}
		}
	}
	flush_range_encoder(encoder, (intptr_t)output);
	*output_length = state->output_bytes;
	return 1;
}

uint32_t get_range_encoder_capacity(struct range_encoder *encoder, uint32_t *capacity)
{
	return encoded_capacity(&encoder->parameters, capacity);
}

uint32_t range_encoder_available(void) { return 1; }

bool initialize_range_encoder(struct range_encoder *encoder, struct range_parameters *parameters)
{
	struct range_state *state = (void *)parameters->state;

	if (!state)
		return false;
	encoder->parameters = *parameters;
	encoder->parameters.alphabet_size &= UINT8_MAX;
	encoder->capacity = get_range_encoder_capacity;
	encoder->set_length = set_range_encoder_length;
	encoder->compress = compress_indexed_band;
	encoder->available = range_encoder_available;
	encoder->finish = finish_range_encoder_session;
	/* Initialization intentionally leaves output position and counters alone. */
	state->range = UINT32_MAX;
	state->low = 0;
	state->cached_byte = 0;
	state->pending_bytes = 0;
	state->cache_empty = 1;
	return true;
}

static void write_bound(uint8_t command, uint16_t value)
{
	uint8_t bytes[] = {command, value, value >> 8};

	(*lpr_write_callback)(bytes, sizeof(bytes));
}

static void write_channel_header(uint32_t framed_bytes, uint32_t raw_bytes)
{
	uint8_t header[1 + 2 * sizeof(uint32_t)] = {COMMAND_RANGE_CHANNEL};

	put_u32le(header + 1, framed_bytes);
	put_u32le(header + 5, raw_bytes);
	(*lpr_write_callback)(header, sizeof(header));
}

static void write_compressed_channel(const uint8_t *data, uint32_t length, uint32_t raw_bytes)
{
	uint32_t chunks = length / RANGE_CHUNK_BYTES + (length % RANGE_CHUNK_BYTES != 0);

	write_channel_header(length + chunks * RANGE_CHUNK_HEADER_BYTES, raw_bytes);
	while (length) {
		uint32_t bytes = length < RANGE_CHUNK_BYTES ? length : RANGE_CHUNK_BYTES;
		uint8_t header[RANGE_CHUNK_HEADER_BYTES] = {COMMAND_RASTER_DATA, bytes, bytes >> 8,
							    0};

		(*lpr_write_callback)(header, sizeof(header));
		(*lpr_write_callback)(data, bytes);
		data += bytes;
		length -= bytes;
	}
}

void emit_range_encoded_swath(struct printer_state *printer, struct raster_pass *pass,
			      struct output_swath *swath)
{
	struct range_swath *numeric = (void *)swath->numeric_state;
	struct range_state state = {0};
	struct range_encoder encoder;
	unsigned int channels =
		(pass->mode & RASTER_CHANNEL_MASK) == RASTER_MONOCHROME ? 1 : RANGE_COLOR_CHANNELS;
	struct range_parameters parameters = {
		.state = (void *)&state,
		.pixels = pass->encoded_pixels,
		.rows = pass->nozzle_rows,
		.bits_per_symbol = pass->bits_per_symbol,
		.channels = channels,
		.alphabet_size = pass->quantization_levels & UINT8_MAX,
		.row_bytes = pass->padded_row_bytes,
	};
	uint32_t offsets[RANGE_COLOR_CHANNELS], lengths[RANGE_COLOR_CHANNELS];
	uint32_t capacity, offset = 0;
	uint32_t total_rows = channels * parameters.rows;
	uint16_t first_symbol, end_symbol;
	uint8_t *encoded;
	unsigned int channel, row, symbols_per_byte;
	size_t missing_bytes, raw_bytes;

	/* Swath storage must already reserve room for the complete nozzle rows. */
	if (!parameters.row_bytes || !encoded_capacity(&parameters, &capacity) ||
	    numeric->filled_rows > total_rows || swath->cursor < swath->start ||
	    swath->cursor - swath->start != (size_t)numeric->filled_rows * parameters.row_bytes)
		return;
	missing_bytes = (size_t)(total_rows - numeric->filled_rows) * parameters.row_bytes;
	if (missing_bytes) {
		memset((void *)swath->cursor, 0, missing_bytes);
		swath->cursor += missing_bytes;
	}
	encoded = malloc((size_t)capacity * channels);
	if (!encoded)
		return;
	initialize_range_encoder(&encoder, &parameters);
	for (channel = 0; channel < channels; channel++) {
		offsets[channel] = offset;
		if (!compress_indexed_band(&encoder, swath->start + channel * parameters.row_bytes,
					   encoded + offset, &lengths[channel]))
			goto out;
		offset += lengths[channel];
		memcpy(numeric->symbol_counts[channel], state.symbol_counts,
		       sizeof(numeric->symbol_counts[channel]));
	}
	if (pass->mirrored) {
		uint16_t first = numeric->first_byte;

		numeric->first_byte = pass->output_bytes - numeric->end_byte;
		numeric->end_byte = pass->output_bytes - first;
	}
	if (pass->bits_per_symbol == 8) {
		first_symbol = 0;
		end_symbol = pass->padded_row_bytes;
	} else {
		symbols_per_byte = RANGE_BYTE_BITS / pass->bits_per_symbol;
		first_symbol = numeric->first_byte * symbols_per_byte;
		end_symbol = numeric->end_byte * symbols_per_byte;
	}
	write_bound(COMMAND_RANGE_START, first_symbol);
	write_bound(COMMAND_RANGE_END, end_symbol);
	emit_ink_dot_counts(printer, swath);
	raw_bytes = (size_t)parameters.rows * parameters.row_bytes;
	for (channel = 0; channel < channels; channel++) {
		if (pass->compression == ROW_CODEC_RANGE_CODER) {
			write_compressed_channel(encoded + offsets[channel], lengths[channel],
						 raw_bytes);
		} else {
			write_channel_header(raw_bytes, raw_bytes);
			for (row = 0; row < parameters.rows; row++)
				(*lpr_write_callback)((const uint8_t *)swath->start +
							      ((size_t)row * channels + channel) *
								      parameters.row_bytes,
						      parameters.row_bytes);
		}
	}
out:
	free(encoded);
}
