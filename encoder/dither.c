/* Convert RGB rows to independently packed CMYK dots using threshold matrices. */
#include "dither.h"
#include "color.h"
#include "data.h"
#include "printhead.h"
#include "protocol.h"
#include "row_correction.h"
#include "settings.h"
#include "state.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum dither_constants {
	INK_CHANNELS = 4,
	MATRIX_SIDE = 128,
	MATRIX_MASK = MATRIX_SIDE - 1,
	THRESHOLDS_PER_CELL = 4,
	MATRIX_CHANNEL_BYTES = MATRIX_SIDE * MATRIX_SIDE * THRESHOLDS_PER_CELL * 2,
	DENSITY_SHIFT = 14,
	DENSITY_SCALE = 1 << DENSITY_SHIFT,
	DENSITY_NORMAL = 1000,
	HALFTONE_DITHER = 1,
	GRAY_BLACK_ONLY = 1,
	HEAD_TABLES_BUILT = 0x20,
	PLANE_STRIDE_BYTES = 0x1190,
	OUTPUT_ROW_BYTES = INK_CHANNELS * PLANE_STRIDE_BYTES,
	CORRECTION_BUFFER_BYTES = 0x16da0,
	RGB_BYTES = 3,
	RGB_WHITE = 0xffffff,
	FAST_ROW_COPIES = 2,
};

enum dither_state_offset {
	STATE_HALFTONE_METHOD = 0x8e6,
	STATE_INK_LIMIT = 0x11ac,
	STATE_PIXEL_SCALE = 0x11b4,
	STATE_CORRECTION = 0x12fc,
	STATE_CORRECTION_FIRST_ROW = 0x8133e,
	STATE_COLOR_CORRECTION = 0x81344,
	STATE_COLOR_FIRST_ROW = 0x81348,
	STATE_DENSITY_UPPER = 0x8134c,
	STATE_HALF_RATE_ROWS = 0x81350,
	STATE_DENSITY_FLAGS = 0x81360,
	STATE_CHANNEL_DENSITIES = 0x81362,
	STATE_HEAD_TABLES_AVAILABLE = 0x817d8,
};

enum ink_channel {
	INK_CYAN,
	INK_MAGENTA,
	INK_YELLOW,
	INK_BLACK,
};

/* Values are the number of packed bits per dot. */
enum dot_format {
	DOT_BINARY = 1,
	DOT_FOUR_LEVEL = 2,
	DOT_FIVE_LEVEL = 4,
};

/*

 * Numeric calibration records retain their original layout. Matrix pointers
 * live separately in halftone_state.channels, in K/C/M/Y order.
 */
struct dither_channel {
	uint8_t reserved[0x5c];
	uint32_t density[THRESHOLDS_PER_CELL];
	uint8_t reserved_tail[0x78 - 0x6c];
};

struct dither_parameters {
	struct dither_channel channels[INK_CHANNELS];
	uint8_t reserved_gray[0x1e8 - 0x1e0];
	uint32_t gray_mode;
	uint8_t reserved_row[0x200 - 0x1ec];
	uint32_t matrix_row;
	uint8_t reserved_head[0x4a6a - 0x204];
	uint8_t head_flags;
	uint8_t media_class;
	int16_t row_map[40];
};

union ink_pixel {
	int16_t channels[INK_CHANNELS];
	uint32_t pairs[2];
};

struct color_cache {
	uint32_t rgb;
	uint32_t interpolation[2];
	int16_t input[INK_CHANNELS];
	union ink_pixel converted;
};

struct dither_context {
	struct printer_state *printer;
	struct printer_buffers *buffers;
	struct dither_parameters *parameters;
	struct color_cache color;
	union ink_pixel filtered;
	uint32_t first_dot;
	uint32_t dot_count;
	uint32_t pixel_scale;
	enum dot_format format;
	bool fast;
	bool black_only;
};

static uint16_t state_word(struct printer_state *printer, size_t offset)
{
	uint16_t value;

	memcpy(&value, printer->numeric_state + offset, sizeof(value));
	return value;
}

static uint32_t state_dword(struct printer_state *printer, size_t offset)
{
	uint32_t value;

	memcpy(&value, printer->numeric_state + offset, sizeof(value));
	return value;
}

void assign_channel_dither_matrices(struct printer_state *printer)
{
	struct printer_buffers *buffers = &printer->buffers;
	struct halftone_channel *channels = halftone_channels(buffers->halftone);
	unsigned int channel;

	for (channel = 0; channel < INK_CHANNELS; channel++)
		channels[channel].dither_matrix =
			buffers->dither_matrix + channel * MATRIX_CHANNEL_BYTES;
}

void select_dither_matrix(struct printer_state *printer)
{
	struct printer_buffers *buffers = &printer->buffers;
	uint16_t depth = state_word(printer, STATE_RASTER_MODE) & RASTER_DEPTH_MASK;

	buffers->dither_matrix = depth == RASTER_FOUR_LEVEL_DOTS
					 ? (uintptr_t)lpr_dither_matrix_bank
					 : (uintptr_t)lpr_photo_dither_matrix_bank;
}

uint32_t set_dither_density_factors(struct printer_state *printer)
{
	struct dither_parameters *parameters = (void *)printer->buffers.halftone;
	uint32_t scale = DENSITY_SCALE;
	unsigned int channel, level;

	if (printer->numeric_state[STATE_HALFTONE_METHOD] != HALFTONE_DITHER)
		return scale;

	for (channel = 0; channel < INK_CHANNELS; channel++) {
		if (printer->numeric_state[STATE_DENSITY_FLAGS])
			scale = DENSITY_NORMAL * DENSITY_SCALE /
				state_word(printer,
					   STATE_CHANNEL_DENSITIES + channel * sizeof(uint16_t));
		for (level = 0; level < THRESHOLDS_PER_CELL; level++)
			parameters->channels[channel].density[level] = scale;
	}
	return scale;
}

static void convert_pixel(struct dither_context *context, const uint8_t *pixel)
{
	struct color_cache *cache = &context->color;
	uint32_t rgb = pixel[0] | (uint32_t)pixel[1] << 8 | (uint32_t)pixel[2] << 16;
	const uint16_t *black_curve = (void *)context->buffers->black_curve;

	/* The initial white pixel has zero ink without invoking color conversion. */
	if (rgb == cache->rgb)
		return;
	interpolate_rgb_to_ink(cache->interpolation, (uint8_t *)&rgb, cache->input,
			       context->printer);
	if (context->black_only) {
		memset(&cache->converted, 0, sizeof(cache->converted));
		cache->converted.channels[INK_BLACK] =
			black_curve[(uint8_t)cache->input[INK_BLACK]];
	} else {
		apply_ink_limit(cache->input, (uint16_t *)cache->converted.channels,
				context->printer);
	}
	cache->rgb = rgb;
}

static void select_row_thresholds(struct dither_context *context, unsigned int row)
{
	if (state_dword(context->printer, STATE_HEAD_TABLES_AVAILABLE) &&
	    (context->parameters->head_flags & HEAD_TABLES_BUILT))
		select_dither_row_thresholds(context->printer, row);
}

static bool begin_corrected_row(struct dither_context *context, unsigned int row)
{
	struct printer_state *printer = context->printer;
	bool limit_ink = false;

	if (state_dword(printer, STATE_COLOR_CORRECTION))
		limit_ink = context->parameters->row_map[row] ==
			    (int16_t)state_word(printer, STATE_COLOR_FIRST_ROW);

	if (state_dword(printer, STATE_CORRECTION)) {
		if (context->parameters->row_map[row] ==
		    (int16_t)state_word(printer, STATE_CORRECTION_FIRST_ROW))
			reset_correction_row((intptr_t)(printer->numeric_state + STATE_CORRECTION));
		advance_correction_row((intptr_t)(printer->numeric_state + STATE_CORRECTION));
	}
	return limit_ink;
}

static void filter_ink_rows(struct dither_context *context, unsigned int dot)
{
	union ink_pixel *previous = (void *)context->buffers->correction_previous;
	union ink_pixel *current = (void *)context->buffers->correction_current;
	union ink_pixel *converted = &context->color.converted;
	unsigned int channel;

	if (context->format == DOT_FIVE_LEVEL) {
		/*
		 * Five-level dots use the previous/current/next rows with 1:2:1 weights.
		 * Preserve the legacy magenta rounding for signed intermediate values.
		 */
		for (channel = 0; channel < INK_CHANNELS; channel++) {
			uint32_t sum = (uint32_t)(int32_t)converted->channels[channel] +
				       (uint16_t)previous[dot].channels[channel] +
				       2U * (uint16_t)current[dot].channels[channel];

			context->filtered.channels[channel] =
				channel == INK_MAGENTA ? (uint32_t)((int32_t)sum / 4) : sum / 4;
		}
	}
	previous[dot] = *converted;
}

static void correct_pixel(struct dither_context *context, unsigned int dot, bool limit_ink)
{
	struct printer_state *printer = context->printer;

	context->filtered = context->color.converted;
	if ((int32_t)state_dword(printer, STATE_HALF_RATE_ROWS) > 0)
		filter_ink_rows(context, context->first_dot + dot);
	if (limit_ink)
		apply_row_ink_limit(context->filtered.channels,
				    state_dword(printer, STATE_COLOR_CORRECTION),
				    state_dword(printer, STATE_COLOR_FIRST_ROW),
				    state_word(printer, STATE_DENSITY_UPPER),
				    state_word(printer, STATE_INK_LIMIT));
	accumulate_ink_coverage((int *)(printer->numeric_state + STATE_CORRECTION),
				context->filtered.pairs[0], context->filtered.pairs[1], dot,
				context->first_dot + dot);
}

static uint8_t quantize_channel(struct dither_context *context, unsigned int channel,
				unsigned int cell)
{
	/* Ink pixels and output planes are C/M/Y/K; calibration records are K/C/M/Y. */
	unsigned int matrix_channel = (channel + 1) % INK_CHANNELS;
	struct halftone_channel *channels = halftone_channels(context->buffers->halftone);
	const uint16_t *thresholds = (void *)channels[matrix_channel].dither_matrix;
	const uint32_t *density = context->parameters->channels[matrix_channel].density;
	int value = context->filtered.channels[channel];
	unsigned int level;

	/* Fast modes compare zero ink with the matrix too, including zero thresholds. */
	if (!context->fast && !value)
		return 0;
	thresholds += cell * THRESHOLDS_PER_CELL;
	if (context->format == DOT_BINARY)
		return value >= (int)(thresholds[2] * density[2] >> DENSITY_SHIFT);

	for (level = context->format == DOT_FIVE_LEVEL ? 4 : 3; level; level--) {
		/* Keep the 32-bit product before removing the fixed-point fraction. */
		uint32_t threshold = thresholds[level - 1] * density[level - 1] >> DENSITY_SHIFT;

		if (value >= (int)threshold)
			return level;
	}
	return 0;
}

static uint8_t write_packed_dots(uint8_t *output, const uint8_t packed[INK_CHANNELS],
				 unsigned int shift)
{
	uint8_t ink_mask = 0;
	unsigned int channel;

	for (channel = 0; channel < INK_CHANNELS; channel++) {
		uint8_t value = packed[channel] << shift;

		output[channel * PLANE_STRIDE_BYTES] = value;
		ink_mask |= value;
	}
	return ink_mask;
}

static uint8_t render_output_row(struct dither_context *context, uint8_t **input_row,
				 int input_stride, uint8_t *output, unsigned int input_rows,
				 bool limit_ink)
{
	unsigned int dots_per_byte = 8 / context->format;
	unsigned int packed_dots = context->first_dot % dots_per_byte;
	uint8_t packed[INK_CHANNELS] = {0};
	uint8_t ink_mask = 0;
	unsigned int phase, dot, channel;

	for (phase = 0; phase < input_rows; phase++) {
		const uint8_t *pixel;

		/* Reversed half-rate bands retain the original overlapping row traversal. */
		if (!context->fast &&
		    (context->printer->numeric_state[STATE_IMAGE_ORIENTATION] &
		     ORIENTATION_REVERSED) &&
		    (int32_t)state_dword(context->printer, STATE_HALF_RATE_ROWS) > 0)
			*input_row += phase ? -2 * input_stride : input_stride;
		pixel = *input_row;
		for (dot = 0; dot < context->dot_count; dot++) {
			unsigned int column = context->first_dot + dot;
			unsigned int cell;

			convert_pixel(context, pixel + dot / context->pixel_scale * RGB_BYTES);
			if (input_rows == 2 && !phase) {
				union ink_pixel *current =
					(void *)context->buffers->correction_current;

				/*

				 * Binary/four-level modes store the last filtered pixel here;
				 * five-level mode stores the newly converted row for averaging.
				 */
				current[column] = context->format == DOT_FIVE_LEVEL
							  ? context->color.converted
							  : context->filtered;
				continue;
			}
			if (context->fast)
				context->filtered = context->color.converted;
			else
				correct_pixel(context, dot, limit_ink);
			cell = context->parameters->matrix_row * MATRIX_SIDE +
			       (column & MATRIX_MASK);
			for (channel = 0; channel < INK_CHANNELS; channel++)
				packed[channel] = packed[channel] << context->format |
						  quantize_channel(context, channel, cell);
			if (++packed_dots == dots_per_byte) {
				ink_mask |= write_packed_dots(output++, packed, 0);
				packed_dots = 0;
			}
		}
		if (!context->fast)
			*input_row += input_stride;
	}
	if (packed_dots)
		ink_mask |= write_packed_dots(output, packed,
					      (dots_per_byte - packed_dots) * context->format);
	context->parameters->matrix_row = (context->parameters->matrix_row + 1) & MATRIX_MASK;
	return ink_mask;
}

static uint32_t render_dither(struct printer_state *printer, int first_row,
			      struct raster_band *band, uint32_t row_count, int first_column,
			      int column_count, enum dot_format format, bool fast)
{
	struct printer_buffers *buffers = &printer->buffers;
	struct dither_parameters *parameters = (void *)buffers->halftone;
	uint32_t scale = state_dword(printer, STATE_PIXEL_SCALE);
	struct dither_context context = {
		.printer = printer,
		.buffers = buffers,
		.parameters = parameters,
		.color.rgb = RGB_WHITE,
		.first_dot = first_column * scale,
		.dot_count = column_count * scale,
		.pixel_scale = scale,
		.format = format,
		.fast = fast,
		.black_only = parameters->gray_mode == GRAY_BLACK_ONLY,
	};
	unsigned int input_rows = !fast && state_dword(printer, STATE_HALF_RATE_ROWS) ? 2 : 1;
	unsigned int output_rows = fast ? row_count * FAST_ROW_COPIES : row_count / input_rows;
	uint8_t *input = band->pixels + first_row * band->row_bytes + first_column * RGB_BYTES;
	uint8_t *output = (uint8_t *)buffers->raster_planes + context.first_dot / (8 / format);
	uint32_t ink_mask = 0;
	unsigned int row;

	for (row = 0; row < output_rows; row++) {
		bool limit_ink = false;

		select_row_thresholds(&context, fast ? row / FAST_ROW_COPIES : row);
		if (!fast)
			limit_ink = begin_corrected_row(&context, row);
		ink_mask |= render_output_row(&context, &input, band->row_bytes, output, input_rows,
					      limit_ink);
		output += OUTPUT_ROW_BYTES;
		if (fast && row % FAST_ROW_COPIES == FAST_ROW_COPIES - 1)
			input += band->row_bytes;
	}
	if (!fast && state_dword(printer, STATE_HALF_RATE_ROWS)) {
		/* The retained correction margins use source columns, before scaling. */
		uint8_t *previous = (void *)buffers->correction_previous;
		size_t first = first_column * sizeof(union ink_pixel);
		size_t end = (first_column + column_count) * sizeof(union ink_pixel);

		memset(previous, 0, first);
		memset(previous + end, 0, CORRECTION_BUFFER_BYTES - end);
	}
	return ink_mask;
}

uint32_t render_five_level_dither(struct printer_state *printer, int first_row, uint32_t width,
				  struct raster_band *band, uint32_t row_count, int first_column,
				  int column_count)
{
	(void)width;
	return render_dither(printer, first_row, band, row_count, first_column, column_count,
			     DOT_FIVE_LEVEL, false);
}

uint32_t render_four_level_dither(struct printer_state *printer, int first_row, uint32_t width,
				  struct raster_band *band, uint32_t row_count, int first_column,
				  int column_count)
{
	(void)width;
	return render_dither(printer, first_row, band, row_count, first_column, column_count,
			     DOT_FOUR_LEVEL, false);
}

uint32_t render_fast_four_level_dither(struct printer_state *printer, int first_row, uint32_t width,
				       struct raster_band *band, uint32_t row_count,
				       int first_column, int column_count)
{
	(void)width;
	return render_dither(printer, first_row, band, row_count, first_column, column_count,
			     DOT_FOUR_LEVEL, true);
}

uint32_t render_binary_dither(struct printer_state *printer, int first_row, uint32_t width,
			      struct raster_band *band, uint32_t row_count, int first_column,
			      int column_count)
{
	(void)width;
	return render_dither(printer, first_row, band, row_count, first_column, column_count,
			     DOT_BINARY, false);
}

uint32_t render_fast_binary_dither(struct printer_state *printer, int first_row, uint32_t width,
				   struct raster_band *band, uint32_t row_count, int first_column,
				   int column_count)
{
	(void)width;
	return render_dither(printer, first_row, band, row_count, first_column, column_count,
			     DOT_BINARY, true);
}
