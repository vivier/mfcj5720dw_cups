/* Serpentine error diffusion, calibrated dot selection and CMYK row packing. */
#include "diffusion.h"
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

enum diffusion_constants {
	INK_CHANNELS = 4,
	ERROR_ROWS = 3,
	ERROR_PADDING_COLUMNS = 4,
	NOISE_SIDE = 256,
	NOISE_MASK = NOISE_SIDE - 1,
	NOISE_PHASE_MASK = 1023,
	NOISE_MINIMUM = 2,
	NOISE_MAXIMUM = 255,
	PHOTO_NOISE_MAXIMUM = 1535,
	NOISE_SHIFT_MASK = 31,
	DENSITY_SHIFT = 7,
	DENSITY_UNITY = 1 << DENSITY_SHIFT,
	HEAD_TABLES_BUILT = 0x20,
	GRAY_BLACK_ONLY = 1,
	RGB_BYTES = 3,
	RGB_WHITE = 0xffffff,
	CORRECTION_BUFFER_BYTES = 0x16da0,
	RAMP_WIDTH = 200,
	LEFT_MARGIN = 62,
	RIGHT_MARGIN = 58,
	BEST_ROW_PAIRS = 5,
	PROFILE_VARIANT_MASK = 0x0f0f,
	PROFILE_KEEP_PHOTO_NOISE = 0x0608,
};

enum diffusion_state_offset {
	STATE_HEAD_CALIBRATION = 0x614,
	STATE_BAND_INDEX = 0x8a4,
	STATE_BAND_HEIGHT = 0x8a6,
	STATE_FINAL_BAND_HEIGHT = 0x8a8,
	STATE_INK_LIMIT = 0x11ac,
	STATE_PIXEL_SCALE = 0x11b4,
	STATE_SCAN_ROW = 0x11b8,
	STATE_PLANE_STRIDE = 0x11d8,
	STATE_BAND_COUNT = 0x11f2,
	STATE_CORRECTION = 0x12fc,
	STATE_CORRECTION_FIRST_ROW = 0x8133e,
	STATE_COLOR_CORRECTION = 0x81344,
	STATE_COLOR_FIRST_ROW = 0x81348,
	STATE_DENSITY_UPPER = 0x8134c,
	STATE_HALF_RATE_ROWS = 0x81350,
	STATE_RAMP_MODE = 0x817c4,
	STATE_RAMP_POSITION = 0x817cc,
};

enum diffusion_kind {
	DIFFUSION_BINARY,
	DIFFUSION_FOUR_LEVEL,
	DIFFUSION_FIVE_LEVEL,
	DIFFUSION_ADAPTIVE,
	DIFFUSION_PHOTO,
};

enum ramp_mode {
	RAMP_DISABLED,
	RAMP_ZERO,
	RAMP_BODY,
	RAMP_LEADING_EDGE,
	RAMP_TRAILING_EDGE,
};

/* Thresholds and ink amounts are stored in descending dot-level order. */
struct diffusion_channel {
	int16_t ink;
	uint16_t reserved_ink;
	uint8_t packed_dots;
	uint8_t reserved_thresholds[0x18 - 5];
	int16_t thresholds[4];
	int16_t dot_ink[4];
	uint8_t reserved_blank[0x58 - 0x28];
	int16_t blank_threshold;
	int16_t blank_ink;
	uint8_t reserved_tail[0x78 - 0x5c];
};

struct diffusion_parameters {
	struct diffusion_channel channels[INK_CHANNELS];
	uint32_t noise_cursor;
	uint32_t noise_phase;
	uint32_t gray_mode;
	uint32_t error_phase;
	uint32_t first_error;
	uint8_t reserved_column[0x1fc - 0x1f4];
	uint32_t matrix_column;
	uint32_t matrix_row;
	int16_t noise_threshold;
	int16_t noise_offset;
	int16_t noise_floor;
	uint16_t noise_shift;
	uint8_t reserved_head[0x4a6a - 0x20c];
	uint8_t head_flags;
	uint8_t media_class;
	int16_t row_map[40];
	uint32_t row_phase_count;
	int32_t previous_row;
	int32_t older_row;
	uint32_t reserved_calibration;
	uint32_t density;
};

union ink_pixel {
	int16_t channels[INK_CHANNELS];
	uint32_t pairs[2];
};

struct diffusion_context {
	struct printer_state *printer;
	struct printer_buffers *buffers;
	struct diffusion_parameters *parameters;
	struct halftone_channel *channels;
	enum diffusion_kind kind;
	unsigned int bits_per_dot;
	unsigned int pixel_scale;
	unsigned int first_dot;
	unsigned int dot_count;
	unsigned int plane_stride;
	uint32_t previous_rgb;
	uint32_t interpolation[2];
	int16_t input_ink[INK_CHANNELS];
	union ink_pixel converted;
	union ink_pixel filtered;
	uint32_t noise_phase;
	bool paired_rows;
	bool density_ramp;
};

/* Seven retained copies of these schedules contain the same three phases.
 * Each column interleaves three int16_t errors; offsets are in error samples.
 */
static const int previous_row_offsets[ERROR_ROWS] = {1, -2, 1};
static const int older_row_offsets[ERROR_ROWS] = {2, -1, -1};
static const int next_phase_offsets[ERROR_ROWS] = {1, 1, -2};

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

static void set_state_dword(struct printer_state *printer, size_t offset, uint32_t value)
{
	memcpy(printer->numeric_state + offset, &value, sizeof(value));
}

static unsigned int dot_bits(enum diffusion_kind kind)
{
	if (kind == DIFFUSION_BINARY)
		return 1;
	return kind == DIFFUSION_FIVE_LEVEL ? 4 : 2;
}

static int16_t accumulated_error(struct diffusion_parameters *parameters,
				 struct halftone_channel *channel, int direction)
{
	const int16_t *errors = (void *)channel->error_cursor;
	const int16_t *minimum = (void *)lpr_minimum_diffusion_thresholds;
	int16_t ink = channel->values[0];
	int previous = parameters->previous_row;
	int16_t sum, value;

	/* Six neighbors, with 1:1:1:1:2:2 weights. Narrow before the signed shift. */
	sum = errors[previous - ERROR_ROWS] + errors[parameters->older_row] +
	      errors[previous + ERROR_ROWS] + errors[-2 * direction * ERROR_ROWS] +
	      2 * (errors[-direction * ERROR_ROWS] + errors[previous]);
	value = (sum >> 3) + ink;
	parameters->noise_cursor &= NOISE_PHASE_MASK;
	parameters->noise_phase &= NOISE_PHASE_MASK;
	if (value < minimum[ink]) {
		const int16_t *noise =
			(void *)halftone_channels((uintptr_t)parameters)[0].diffusion_noise;

		/* Division truncates toward zero here, unlike the weighted-error shift. */
		value = noise[parameters->noise_phase] / 8 + minimum[ink];
	}
	return value;
}

static int16_t threshold_noise(const struct diffusion_parameters *parameters,
			       const struct halftone_channel *channel, bool photo)
{
	const int16_t *noise = (void *)channel->diffusion_noise;
	unsigned int row = parameters->matrix_row * NOISE_SIDE;
	unsigned int column = parameters->matrix_column;
	int reflected = noise[row + NOISE_MASK - column];
	uint32_t value;
	int maximum = photo ? PHOTO_NOISE_MAXIMUM : NOISE_MAXIMUM;

	if (noise[row + column] < parameters->noise_threshold) {
		value = (uint16_t)((reflected >> 5) + parameters->noise_floor);
	} else {
		value = (reflected >> (parameters->noise_shift & NOISE_SHIFT_MASK)) +
			(uint32_t)(uint16_t)(parameters->noise_offset + parameters->noise_floor);
		if ((int16_t)value < NOISE_MINIMUM)
			value = NOISE_MINIMUM;
	}
	if ((int16_t)value > maximum)
		value = maximum;
	return (uint16_t)(value << 2);
}

static unsigned int select_dot(struct diffusion_parameters *parameters,
			       struct halftone_channel *channel, enum diffusion_kind kind,
			       int16_t noise, int16_t *error)
{
	struct diffusion_channel *values = (void *)channel->values;
	bool photo = kind == DIFFUSION_ADAPTIVE || kind == DIFFUSION_PHOTO;
	int level, lowest = kind == DIFFUSION_BINARY ? 3 : 1;
	int highest = kind == DIFFUSION_FIVE_LEVEL ? 4 : 3;

	if (!values->ink) {
		*error = (int16_t)(*error + 1);
		if (*error >= values->blank_threshold + noise)
			*error = (int16_t)(*error - values->blank_ink);
		return 0;
	}
	for (level = highest; level >= lowest; level--) {
		int threshold = values->thresholds[4 - level] + noise;

		if (photo && level == 2)
			threshold = (int32_t)(((uint32_t)(int32_t)values->thresholds[2] *
						       parameters->density >>
					       DENSITY_SHIFT) +
					      (uint32_t)(int32_t)noise);
		if (*error >= threshold) {
			*error = (int16_t)(*error - values->dot_ink[4 - level]);
			return kind == DIFFUSION_BINARY ? 1 : level;
		}
	}
	if (kind != DIFFUSION_PHOTO) {
		const int16_t *spots = (void *)channel->photo_noise;
		unsigned int cell = parameters->matrix_row * NOISE_SIDE + parameters->matrix_column;
		int cutoff = kind == DIFFUSION_ADAPTIVE ? 1 : 2;
		int negative_ink = -values->blank_ink;

		/* Sparse low-density dots carry negative error into subsequent pixels. */
		if (spots[cell] < cutoff) {
			if (*error < negative_ink / 4) {
				*error = (int16_t)(*error - values->blank_ink);
				return 1;
			}
			if (*error < negative_ink / 16) {
				*error = (int16_t)(*error - values->dot_ink[3]);
				return 1;
			}
		}
	}
	return 0;
}

static void diffuse_pixel(short *halftone, enum diffusion_kind kind, int direction)
{
	struct diffusion_parameters *parameters = (void *)halftone;
	struct halftone_channel *channels = halftone_channels((uintptr_t)halftone);
	unsigned int bits = dot_bits(kind), index;
	bool photo = kind == DIFFUSION_ADAPTIVE || kind == DIFFUSION_PHOTO;

	for (index = 0; index < INK_CHANNELS; index++) {
		struct halftone_channel *channel = &channels[index];
		struct diffusion_channel *values = (void *)channel->values;
		int16_t *errors = (void *)channel->error_cursor;
		int16_t error = accumulated_error(parameters, channel, direction);
		int16_t noise = threshold_noise(parameters, channel, photo);
		unsigned int dot = select_dot(parameters, channel, kind, noise, &error);

		if (direction > 0)
			values->packed_dots = values->packed_dots << bits | dot;
		else
			values->packed_dots = values->packed_dots >> bits | dot << (8 - bits);
		if (values->ink && error > values->ink)
			error = values->ink;
		*errors = error;
		channel->error_cursor = (uintptr_t)(errors + direction * ERROR_ROWS);
	}
}

void diffuse_binary_forward(short *halftone)
{
	diffuse_pixel(halftone, DIFFUSION_BINARY, 1);
}

void diffuse_binary_reverse(short *halftone)
{
	diffuse_pixel(halftone, DIFFUSION_BINARY, -1);
}

void diffuse_four_level_forward(short *halftone)
{
	diffuse_pixel(halftone, DIFFUSION_FOUR_LEVEL, 1);
}

void diffuse_four_level_reverse(short *halftone)
{
	diffuse_pixel(halftone, DIFFUSION_FOUR_LEVEL, -1);
}

void diffuse_five_level_forward(short *halftone)
{
	diffuse_pixel(halftone, DIFFUSION_FIVE_LEVEL, 1);
}

void diffuse_five_level_reverse(short *halftone)
{
	diffuse_pixel(halftone, DIFFUSION_FIVE_LEVEL, -1);
}

void diffuse_adaptive_forward(short *halftone)
{
	diffuse_pixel(halftone, DIFFUSION_ADAPTIVE, 1);
}

void diffuse_adaptive_reverse(short *halftone)
{
	diffuse_pixel(halftone, DIFFUSION_ADAPTIVE, -1);
}

void diffuse_photo_forward(short *halftone)
{
	diffuse_pixel(halftone, DIFFUSION_PHOTO, 1);
}

void diffuse_photo_reverse(short *halftone)
{
	diffuse_pixel(halftone, DIFFUSION_PHOTO, -1);
}

static void convert_pixel(struct diffusion_context *context, const uint8_t *pixel)
{
	uint32_t rgb = pixel[0] | (uint32_t)pixel[1] << 8 | (uint32_t)pixel[2] << 16;

	if (rgb == context->previous_rgb)
		return;
	interpolate_rgb_to_ink(context->interpolation, (uint8_t *)&rgb, context->input_ink,
			       context->printer);
	if (context->parameters->gray_mode == GRAY_BLACK_ONLY) {
		const uint16_t *curve = (void *)context->buffers->black_curve;

		memset(&context->converted, 0, sizeof(context->converted));
		context->converted.channels[3] = curve[(uint8_t)context->input_ink[3]];
	} else {
		apply_ink_limit(context->input_ink, (uint16_t *)context->converted.channels,
				context->printer);
	}
	context->previous_rgb = rgb;
}

static void prepare_ink(struct diffusion_context *context, unsigned int column, bool limit)
{
	struct printer_state *printer = context->printer;
	unsigned int channel;

	context->filtered = context->converted;
	if (context->kind == DIFFUSION_FIVE_LEVEL &&
	    (int32_t)state_dword(printer, STATE_HALF_RATE_ROWS) > 0) {
		union ink_pixel *previous = (void *)context->buffers->correction_previous;
		union ink_pixel *current = (void *)context->buffers->correction_current;

		for (channel = 0; channel < INK_CHANNELS; channel++) {
			uint32_t sum = (uint32_t)(int32_t)context->converted.channels[channel] +
				       (uint16_t)previous[column].channels[channel] +
				       2U * (uint16_t)current[column].channels[channel];

			context->filtered.channels[channel] = sum / 4;
		}
		previous[column] = context->converted;
	}
	if (limit)
		apply_row_ink_limit(context->filtered.channels,
				    state_dword(printer, STATE_COLOR_CORRECTION),
				    state_dword(printer, STATE_COLOR_FIRST_ROW),
				    state_word(printer, STATE_DENSITY_UPPER),
				    state_word(printer, STATE_INK_LIMIT));
	for (channel = 0; channel < INK_CHANNELS; channel++)
		context->parameters->channels[(channel + 1) % INK_CHANNELS].ink =
			context->filtered.channels[channel];
}

static uint32_t density_at_column(const struct diffusion_context *context, uint32_t column)
{
	struct printer_state *printer = context->printer;
	uint32_t mode = state_dword(printer, STATE_RAMP_MODE);
	uint32_t width =
		state_dword(printer, STATE_IMAGE_END_X) - state_dword(printer, STATE_IMAGE_START_X);
	uint32_t scale = context->pixel_scale;
	uint32_t left = LEFT_MARGIN * scale, right = (width - RIGHT_MARGIN) * scale;
	uint32_t left_inner = (LEFT_MARGIN + RAMP_WIDTH) * scale;
	uint32_t right_inner = (width - RIGHT_MARGIN - RAMP_WIDTH) * scale;
	uint32_t density = DENSITY_UNITY;

	if (mode == RAMP_ZERO)
		return 0;
	if (mode != RAMP_BODY && mode != RAMP_LEADING_EDGE && mode != RAMP_TRAILING_EDGE)
		return density;
	if (column < left || column > right)
		return 0;
	if (column < left_inner)
		density = (column - left) * DENSITY_UNITY / (scale * RAMP_WIDTH);
	else if (column > right_inner)
		density = (right - column) * DENSITY_UNITY / (scale * RAMP_WIDTH);
	if (mode == RAMP_LEADING_EDGE || mode == RAMP_TRAILING_EDGE) {
		uint32_t edge =
			(state_dword(printer, STATE_RAMP_POSITION) << DENSITY_SHIFT) / RAMP_WIDTH;

		if (density > edge)
			density = edge;
	}
	return density > DENSITY_UNITY ? DENSITY_UNITY : density;
}

static void initialize_error_cursors(struct diffusion_context *context)
{
	struct diffusion_parameters *parameters = context->parameters;
	uintptr_t bases[INK_CHANNELS] = {
		context->buffers->black_errors,
		context->buffers->cyan_errors,
		context->buffers->magenta_errors,
		context->buffers->yellow_errors,
	};
	uint32_t offset;
	unsigned int channel;

	parameters->density = DENSITY_UNITY;
	parameters->first_error = context->first_dot * ERROR_ROWS + 2 * ERROR_ROWS;
	if (parameters->error_phase == 1)
		parameters->first_error += 2;
	else if (parameters->error_phase == 2)
		parameters->first_error++;
	offset = parameters->first_error;
	if (state_dword(context->printer, STATE_SCAN_ROW) & 1)
		offset += context->dot_count * ERROR_ROWS - ERROR_ROWS;
	for (channel = 0; channel < INK_CHANNELS; channel++)
		context->channels[channel].error_cursor = bases[channel] + offset * sizeof(int16_t);
}

static void begin_error_row(struct diffusion_context *context)
{
	struct diffusion_parameters *parameters = context->parameters;

	parameters->previous_row = previous_row_offsets[parameters->error_phase];
	parameters->older_row = older_row_offsets[parameters->error_phase];
}

static void advance_noise_phase(struct diffusion_context *context, unsigned int row)
{
	const int16_t *noise = (void *)context->channels[0].photo_noise;

	context->noise_phase = (context->noise_phase + NOISE_MASK + noise[row & NOISE_PHASE_MASK]) &
			       NOISE_PHASE_MASK;
	context->parameters->noise_phase = context->noise_phase;
}

static void finish_error_row(struct diffusion_context *context, int direction)
{
	struct diffusion_parameters *parameters = context->parameters;
	int advance = parameters->older_row - direction * ERROR_ROWS;
	unsigned int channel;

	for (channel = 0; channel < INK_CHANNELS; channel++) {
		int16_t *errors = (void *)context->channels[channel].error_cursor;

		context->channels[channel].error_cursor = (uintptr_t)(errors + advance);
	}
	parameters->error_phase += next_phase_offsets[parameters->error_phase];
	parameters->matrix_row = (parameters->matrix_row + 1) & NOISE_MASK;
}

static uint8_t write_dot_bytes(struct diffusion_context *context, uint8_t *output,
			       unsigned int shift, int direction)
{
	uint8_t mask = 0;
	unsigned int channel;

	for (channel = 0; channel < INK_CHANNELS; channel++) {
		uint8_t *packed =
			&context->parameters->channels[(channel + 1) % INK_CHANNELS].packed_dots;

		*packed = direction > 0 ? *packed << shift : *packed >> shift;
		output[channel * context->plane_stride] = *packed;
		mask |= *packed;
	}
	return mask;
}

static uint8_t render_row(struct diffusion_context *context, uint8_t **input_row, int input_stride,
			  uint8_t *output_row, unsigned int first_column, int direction, bool limit)
{
	struct diffusion_parameters *parameters = context->parameters;
	unsigned int dots_per_byte = 8 / context->bits_per_dot;
	unsigned int column =
		direction > 0 ? context->first_dot : context->first_dot + context->dot_count - 1;
	unsigned int packed = (direction > 0 ? column : ~column) % dots_per_byte;
	uint8_t *output = output_row + column / dots_per_byte;
	unsigned int phases = context->kind == DIFFUSION_FIVE_LEVEL ? 2 : 1;
	unsigned int phase, dot;
	uint8_t mask = 0;

	parameters->matrix_column = column & NOISE_MASK;
	for (phase = 0; phase < phases; phase++) {
		if (phases == 2 &&
		    (context->printer->numeric_state[STATE_IMAGE_ORIENTATION] &
		     ORIENTATION_REVERSED) &&
		    (int32_t)state_dword(context->printer, STATE_HALF_RATE_ROWS) > 0)
			*input_row += phase ? -2 * input_stride : input_stride;
		for (dot = 0; dot < context->dot_count; dot++) {
			unsigned int source = dot / context->pixel_scale;
			unsigned int input_column =
				direction > 0
					? first_column + source
					: first_column + context->dot_count / context->pixel_scale -
						  source - 1;
			unsigned int output_column =
				direction > 0 ? context->first_dot + dot
					      : context->first_dot + context->dot_count - dot - 1;

			if (context->density_ramp)
				parameters->density = density_at_column(context, output_column);
			if (dot % context->pixel_scale == 0) {
				convert_pixel(context, *input_row + input_column * RGB_BYTES);
				if (phases == 2 && !phase) {
					union ink_pixel *current =
						(void *)context->buffers->correction_current;

					current[input_column] = context->converted;
					/* Preserve the first phase: repeated subpixels still
					 * diffuse the preceding ink values when horizontal scale
					 * exceeds one.
					 */
					continue;
				}
				prepare_ink(context, input_column, limit);
			}
			if (!context->paired_rows)
				accumulate_ink_coverage(
				    (int *)(context->printer->numeric_state + STATE_CORRECTION),
				    context->filtered.pairs[0], context->filtered.pairs[1], dot,
				    output_column);
			diffuse_pixel((short *)parameters, context->kind, direction);
			if (++packed == dots_per_byte) {
				mask |= write_dot_bytes(context, output, 0, direction);
				output += direction;
				packed = 0;
			}
			parameters->matrix_column =
				(parameters->matrix_column + direction) & NOISE_MASK;
		}
		if (phases == 2)
			*input_row += input_stride;
	}
	if (packed)
		mask |= write_dot_bytes(context, output,
					(dots_per_byte - packed) * context->bits_per_dot,
					direction);
	return mask;
}

static uint32_t page_row(struct printer_state *printer, int first_row)
{
	uint32_t band = state_word(printer, STATE_BAND_INDEX);
	uint32_t height = state_word(printer, STATE_BAND_HEIGHT);
	uint32_t end;

	if (!(printer->numeric_state[STATE_IMAGE_ORIENTATION] & ORIENTATION_REVERSED))
		return band * height + first_row;
	end = band == (uint32_t)(state_word(printer, STATE_BAND_COUNT) - 1)
		      ? band * height + state_word(printer, STATE_FINAL_BAND_HEIGHT)
		      : (band + 1) * height;
	return end - first_row - 1;
}

static bool row_calibration_enabled(struct diffusion_context *context)
{
	return context->printer->numeric_state[STATE_HEAD_CALIBRATION] == 1 &&
	       (context->parameters->head_flags & HEAD_TABLES_BUILT);
}

static bool prepare_row(struct diffusion_context *context, unsigned int row, int direction,
			uint32_t first_page_row)
{
	struct diffusion_parameters *parameters = context->parameters;
	struct printer_state *printer = context->printer;
	bool limit = false;

	begin_error_row(context);
	if (context->paired_rows && direction > 0)
		advance_noise_phase(context, row);
	if (row_calibration_enabled(context)) {
		if (context->paired_rows) {
			unsigned int reverse = direction < 0;

			select_scaled_row_thresholds(
				printer, (row * 2 + reverse) * parameters->row_phase_count,
				((row + first_page_row) * 2 + reverse) *
					parameters->row_phase_count);
		} else {
			select_diffusion_row_thresholds(printer, row);
		}
	}
	if (!context->paired_rows) {
		if (state_dword(printer, STATE_COLOR_CORRECTION))
			limit = parameters->row_map[row] ==
				(int16_t)state_word(printer, STATE_COLOR_FIRST_ROW);
		if (state_dword(printer, STATE_CORRECTION) &&
		    parameters->row_map[row] ==
			    (int16_t)state_word(printer, STATE_CORRECTION_FIRST_ROW))
			reset_correction_row((intptr_t)(printer->numeric_state + STATE_CORRECTION));
	}
	if (!context->paired_rows && direction > 0)
		advance_noise_phase(context, row);
	for (unsigned int channel = 0; channel < INK_CHANNELS; channel++)
		parameters->channels[channel].packed_dots = 0;
	return limit;
}

static uint32_t render_diffusion(struct printer_state *printer, int first_row,
				 struct raster_band *band, uint32_t row_count, int first_column,
				 int column_count, enum diffusion_kind kind, unsigned int row_pairs,
				 bool density_ramp)
{
	struct printer_buffers *buffers = &printer->buffers;
	struct diffusion_parameters *parameters = (void *)buffers->halftone;
	uint32_t scale = state_dword(printer, STATE_PIXEL_SCALE);
	struct diffusion_context context = {
		.printer = printer,
		.buffers = buffers,
		.parameters = parameters,
		.channels = halftone_channels(buffers->halftone),
		.kind = kind,
		.bits_per_dot = dot_bits(kind),
		.pixel_scale = scale,
		.first_dot = first_column * scale,
		.dot_count = column_count * scale,
		.plane_stride = state_dword(printer, STATE_PLANE_STRIDE),
		.previous_rgb = RGB_WHITE,
		.noise_phase = parameters->noise_phase,
		.paired_rows = row_pairs != 0,
		.density_ramp = density_ramp,
	};
	uint8_t *input = band->pixels + first_row * band->row_bytes;
	uint8_t *output = (void *)buffers->raster_planes;
	uint32_t first_page_row = row_pairs ? page_row(printer, first_row) : 0;
	uint32_t count, row, ink_mask = 0;

	initialize_error_cursors(&context);
	if (row_pairs == BEST_ROW_PAIRS && (uint32_t)band->height < (uint32_t)first_row + row_count)
		row_count = band->height - first_row;
	count = row_pairs		       ? row_count * row_pairs
		: kind == DIFFUSION_FIVE_LEVEL ? row_count / 2
					       : row_count;
	for (row = 0; row < count; row++) {
		unsigned int pass, passes = row_pairs ? 2 : 1;

		for (pass = 0; pass < passes; pass++) {
			int direction =
				row_pairs ? (pass ? -1 : 1)
					  : (state_dword(printer, STATE_SCAN_ROW) & 1 ? -1 : 1);
			bool limit = prepare_row(&context, row, direction, first_page_row);

			ink_mask |= render_row(&context, &input, band->row_bytes, output,
					       first_column, direction, limit);
			finish_error_row(&context, direction);
			output += INK_CHANNELS * context.plane_stride;
		}
		if (row_pairs) {
			if (row % row_pairs == row_pairs - 1)
				input += band->row_bytes;
		} else {
			if (kind != DIFFUSION_FIVE_LEVEL)
				input += band->row_bytes;
			set_state_dword(printer, STATE_SCAN_ROW,
					state_dword(printer, STATE_SCAN_ROW) + 1);
		}
	}
	if (kind == DIFFUSION_FIVE_LEVEL &&
	    (int32_t)state_dword(printer, STATE_HALF_RATE_ROWS) > 0) {
		uint8_t *previous = (void *)buffers->correction_previous;
		size_t end = (first_column + column_count) * sizeof(union ink_pixel);

		memset(previous, 0, first_column * sizeof(union ink_pixel));
		memset(previous + end, 0, CORRECTION_BUFFER_BYTES - end);
	}
	parameters->noise_phase = context.noise_phase;
	return ink_mask;
}

uint32_t render_binary_diffusion(struct printer_state *printer, int first_row, uint32_t width,
				 struct raster_band *band, uint32_t row_count, int first_column,
				 int column_count)
{
	(void)width;
	return render_diffusion(printer, first_row, band, row_count, first_column, column_count,
				DIFFUSION_BINARY, 0, false);
}

uint32_t render_four_level_diffusion(struct printer_state *printer, int first_row, uint32_t width,
				     struct raster_band *band, uint32_t row_count, int first_column,
				     int column_count)
{
	(void)width;
	return render_diffusion(printer, first_row, band, row_count, first_column, column_count,
				DIFFUSION_FOUR_LEVEL, 0, false);
}

uint32_t render_five_level_diffusion(struct printer_state *printer, int first_row, uint32_t width,
				     struct raster_band *band, uint32_t row_count, int first_column,
				     int column_count)
{
	(void)width;
	return render_diffusion(printer, first_row, band, row_count, first_column, column_count,
				DIFFUSION_FIVE_LEVEL, 0, false);
}

uint32_t render_fast_binary_diffusion(struct printer_state *printer, int first_row, uint32_t width,
				      struct raster_band *band, uint32_t row_count,
				      int first_column, int column_count)
{
	(void)width;
	return render_diffusion(printer, first_row, band, row_count, first_column, column_count,
				DIFFUSION_BINARY, 1, false);
}

uint32_t render_adaptive_diffusion(struct printer_state *printer, int first_row, uint32_t width,
				   struct raster_band *band, uint32_t row_count, int first_column,
				   int column_count)
{
	(void)width;
	return render_diffusion(printer, first_row, band, row_count, first_column, column_count,
				DIFFUSION_ADAPTIVE, 1, true);
}

uint32_t render_photo_diffusion(struct printer_state *printer, int first_row, uint32_t width,
				struct raster_band *band, uint32_t row_count, int first_column,
				int column_count)
{
	(void)width;
	return render_diffusion(printer, first_row, band, row_count, first_column, column_count,
				DIFFUSION_PHOTO, 2, false);
}

uint32_t render_best_diffusion(struct printer_state *printer, int first_row, uint32_t width,
			       struct raster_band *band, uint32_t row_count, int first_column,
			       int column_count)
{
	(void)width;
	return render_diffusion(printer, first_row, band, row_count, first_column, column_count,
				DIFFUSION_PHOTO, 5, true);
}

static void fill_channel_noise(struct halftone_channel *channels, const int16_t *source, bool photo)
{
	unsigned int channel, row, column;

	/* K is unmodified; C mirrors columns, M mirrors rows, Y mirrors both. */
	for (channel = 0; channel < INK_CHANNELS; channel++) {
		int16_t *destination = (void *)(photo ? channels[channel].photo_noise
						      : channels[channel].diffusion_noise);

		for (row = 0; row < NOISE_SIDE; row++)
			for (column = 0; column < NOISE_SIDE; column++) {
				unsigned int source_row = channel & 2 ? NOISE_MASK - row : row;
				unsigned int source_column =
					channel & 1 ? NOISE_MASK - column : column;

				destination[row * NOISE_SIDE + column] =
					source[source_row * NOISE_SIDE + source_column];
			}
	}
}

void initialize_diffusion_buffers(struct printer_state *printer, uint32_t width, int expanded_width)
{
	struct printer_buffers *buffers = &printer->buffers;
	struct diffusion_parameters *parameters = (void *)buffers->halftone;
	struct halftone_channel *channels = halftone_channels(buffers->halftone);
	int16_t *errors[INK_CHANNELS] = {
		(void *)buffers->black_errors,
		(void *)buffers->cyan_errors,
		(void *)buffers->magenta_errors,
		(void *)buffers->yellow_errors,
	};
	const uint16_t *initial[INK_CHANNELS] = {
		lpr_black_initial_errors,
		lpr_cyan_initial_errors,
		lpr_magenta_initial_errors,
		lpr_yellow_initial_errors,
	};
	int count = (expanded_width + ERROR_PADDING_COLUMNS) * ERROR_ROWS;
	unsigned int channel;
	int index;

	(void)width;
	for (index = 0; index < count; index++)
		for (channel = 0; channel < INK_CHANNELS; channel++)
			errors[channel][index] = initial[channel][index & NOISE_MASK];
	parameters->error_phase = 0;
	parameters->noise_cursor = 0;
	if ((state_word(printer, STATE_RASTER_MODE) & PROFILE_VARIANT_MASK) !=
	    PROFILE_KEEP_PHOTO_NOISE)
		fill_channel_noise(channels, (void *)lpr_photo_noise_forward, true);
	fill_channel_noise(channels, (void *)lpr_diffusion_noise_forward, false);
	set_state_dword(printer, STATE_SCAN_ROW, 0);
}
