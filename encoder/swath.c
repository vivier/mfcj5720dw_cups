/* Schedule printhead passes and pack their color planes into output swaths. */
#include "swath.h"
#include "compression.h"
#include "data.h"
#include "output.h"
#include "protocol.h"
#include "range_coder.h"
#include "row_correction.h"
#include "settings.h"
#include "state.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum swath_constants {
	LAYOUT_PROFILE_MASK = 0x0030,
	LAYOUT_INTERLACE_MASK = 0x7f,
	INTERLACE_ROTATING_SWATHS = 0x20,
	INTERLACE_LEGACY = 0x10,
	INTERLACE_SINGLE_PASS = 0x30,
	INTERLACE_SMALL_PAPER = 0x01,
	INTERLACE_PLAIN = 0x28,
	INTERLACE_PLAIN_BORDERLESS = 0x2a,
	INTERLACE_BORDERLESS = 0xa0,
	FEED_UNITS_PER_INCH = 24000,
	FEED_UNITS_PER_ROW = 80,
	FEED_EDGE_QUANTUM = 20,
	BORDERLESS_LEADING_EDGE = 0xf2f1,
	BORDERLESS_TRAILING_EDGE = 0x1d15,
	NORMAL_LEADING_EDGE = 0xe255,
	NORMAL_TRAILING_EDGE = 0x0c79,
	PLANE_STRIDE_BYTES = 0x1190,
	ROW_PADDING_BYTES = 32,
	MAX_BATCH_ROWS = 40,
	SHORT_BLANK_SWATH_BYTES = 0x690,
	BAND_FIRST = 1,
	BAND_LAST = 2,
	FEED_BUFFERED = 0x01,
	FEED_COMPLETION_SENT = 0x20,
	TWO_PASS_PHASE_MASK = 1,
	FOUR_PASS_PHASE_MASK = 3,
	RANDOM_PHASE_MASK = 0x1f,
	NOISE_TABLE_MASK = 0x3ff,
	NOISE_RANGE = 0x1ff,
	NOISE_CENTER = 255,
	NOISE_PHASES = 1024,
	NOISE_CHANNEL_PHASE = 256,
	FIVE_PASS_PATTERNS = 72,
	FIVE_PASS_COUNT = 5,
	FIVE_DOT_LEVELS = 5,
	RASTER_PROFILE_0105 = 0x105,
	RASTER_FAMILY_0300 = 0x300,
	MULTILEVEL_PLANE_FLAG = 2,
	CUSTOM_PAPER_FIRST = 400,
	CUSTOM_PAPER_COUNT = 16,
};

enum swath_state_offset {
	STATE_QUALITY = 0x00f8,
	STATE_ALIGNMENT_FLAGS = 0x081a,
	STATE_REQUESTED_MEDIA = 0x0858,
	STATE_BORDERLESS = 0x0889,
	STATE_FEED_OFFSET_MULTIPLIER = 0x08a4,
	STATE_FIRST_INK_COLUMN = 0x08ca,
	STATE_INK_COLUMNS = 0x08cc,
	STATE_FEED_POSITION = 0x08ec,
	STATE_BAND_PHASE = 0x1160,
	STATE_PASS_INDEX = 0x1162,
	STATE_TRAILING_PASSES = 0x1163,
	STATE_ROW_INDEX = 0x1166,
	STATE_MASK_PHASE = 0x11f0,
	STATE_CORRECTION = 0x12fc,
	STATE_CORRECTION_ROWS = 0x8133c,
	STATE_HALF_RATE_ROWS = 0x81350,
	STATE_DUPLEX_CONTROL = 0x81810,
};

struct swath_rows {
	uint16_t filled;
	uint16_t reserved_count;
	uint8_t reserved_prefix[8];
	uint8_t reserved_flag;
	uint8_t reserved_byte;
	uint16_t leading_blanks;
	uint32_t skipped_lines;
	uint16_t first_ink;
	uint16_t last_ink;
	uint32_t symbols[COLOR_CHANNELS][COUNTERS_PER_CHANNEL];
	int16_t padding_rows;
	uint16_t reserved_tail;
};

static uint8_t state_byte(struct printer_state *printer, size_t offset)
{
	return printer->numeric_state[offset];
}

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

static void store_byte(struct printer_state *printer, size_t offset, uint8_t value)
{
	printer->numeric_state[offset] = value;
}

static void store_word(struct printer_state *printer, size_t offset, uint16_t value)
{
	memcpy(printer->numeric_state + offset, &value, sizeof(value));
}

static void store_dword(struct printer_state *printer, size_t offset, uint32_t value)
{
	memcpy(printer->numeric_state + offset, &value, sizeof(value));
}

static struct swath_rows *swath_rows(struct output_swath *swath)
{
	return (void *)swath->numeric_state;
}

static bool four_channels(uint16_t mode)
{
	uint16_t channels = mode & RASTER_CHANNEL_MASK;

	return channels == 0 || channels == RASTER_CHANNEL_MASK;
}

static unsigned int channel_count(uint16_t mode)
{
	return four_channels(mode) ? COLOR_CHANNELS : 1;
}

static bool photo_media(uint32_t media)
{
	switch (media) {
	case MEDIA_INKJET:
	case MEDIA_OTHER_PHOTO:
	case MEDIA_VARIANT_17:
	case MEDIA_BROTHER_BP71:
	case MEDIA_HAGAKI_REVERSE:
	case MEDIA_BROTHER_BP60:
		return true;
	default:
		return false;
	}
}

static bool small_photo_paper(uint16_t paper)
{
	switch (paper) {
	case PAPER_ID_PHOTO_L:
	case PAPER_ID_HAGAKI:
	case PAPER_ID_PHOTO_4X6:
	case PAPER_ID_HAGAKI_ALT:
	case PAPER_ID_PHOTO_L_ALT:
	case PAPER_ID_PHOTO_4X6_ALT:
		return true;
	default:
		return false;
	}
}

static uint32_t page_feed_distance(struct printer_state *printer)
{
	int32_t rows = state_dword(printer, STATE_IMAGE_END_Y) - state_dword(printer, STATE_IMAGE_START_Y);
	int dpi = (int16_t)state_word(printer, STATE_RENDER_DPI);

	return rows * (FEED_UNITS_PER_INCH / dpi);
}

static uint32_t blank_symbols(const struct raster_pass *pass)
{
	return (uint32_t)((int16_t)pass->output_bytes * 8) / pass->bits_per_symbol;
}

static void add_blank_symbols(struct swath_rows *rows, const struct raster_pass *pass,
			      unsigned int channel, uint32_t count)
{
	rows->symbols[channel][0] += blank_symbols(pass) * count;
}

static void append_blanks(struct output_swath *swath, struct raster_pass *pass, uint16_t count)
{
	memset(pass->scratch_row, COMMAND_BLANK_ROW, count);
	append_output_bytes(swath, pass->scratch_row, count);
}

static const struct swath_layout *find_swath_layout(struct printer_state *printer)
{
	const struct swath_layout *table = lpr_swath_layouts;
	uint16_t mode = state_word(printer, STATE_RASTER_MODE);
	uint16_t channels = four_channels(mode) ? 0 : RASTER_MONOCHROME;
	uint8_t interlace = state_byte(printer, STATE_INTERLACE_FLAGS) & LAYOUT_INTERLACE_MASK;
	unsigned int index;

	for (index = 0; index < SWATH_LAYOUT_COUNT; index++) {
		if (table[index].profile == (mode & LAYOUT_PROFILE_MASK) &&
		    table[index].resolution == (mode & RASTER_RESOLUTION_FAMILY_MASK) &&
		    table[index].channels == channels && table[index].interlace == interlace)
			return &table[index];
	}
	return NULL;
}

void select_swath_layout(struct printer_state *printer, struct raster_pass *pass)
{
	const struct swath_layout *layout = find_swath_layout(printer);
	bool matched = layout != NULL;

	if (!layout)
		layout = lpr_swath_layouts;
	pass->buffer_row = layout->buffer_row;
	pass->row_schedule = (uintptr_t)layout->row_schedule;
	pass->feed_schedule = (uintptr_t)layout->feed_schedule;
	pass->swath_order = (uintptr_t)layout->swath_order;
	pass->padding_schedule = (uintptr_t)layout->padding_schedule;
	pass->pass_lengths = layout->pass_lengths;
	pass->nozzle_rows = layout->nozzle_rows;
	pass->feed_step = layout->feed_step;
	pass->row_step = layout->row_step;
	pass->pass_count = layout->pass_count;
	pass->first_row = layout->first_row;
	pass->pass_rows = layout->pass_rows;
	/* The fallback uses entry zero's geometry, but clears its channel offsets. */
	if (matched)
		memcpy(pass->channel_offsets, layout->channel_offsets, sizeof(pass->channel_offsets));
	else
		memset(pass->channel_offsets, 0, sizeof(pass->channel_offsets));
	pass->leading_rows = layout->leading_rows;
	pass->trailing_rows = layout->trailing_rows;
	pass->flags = layout->flags;
	pass->feed_rows = layout->feed_rows;
}

uint16_t get_swath_count(struct printer_state *printer)
{
	const struct swath_layout *layout = find_swath_layout(printer);

	return layout ? layout->pass_count : lpr_swath_layouts[0].pass_count;
}

uint16_t get_first_swath_offset(struct printer_state *printer)
{
	const struct swath_layout *layout = find_swath_layout(printer);

	return layout ? layout->first_row : 0;
}

static unsigned int profile_width_scale(uint16_t profile)
{
	switch (profile) {
	case RASTER_PROFILE_0105:
	case RASTER_PROFILE_0205:
	case RASTER_PROFILE_0215:
	case RASTER_PROFILE_0500:
	case RASTER_PROFILE_0510:
	case RASTER_PROFILE_0600:
	case RASTER_PROFILE_0610:
	case RASTER_PROFILE_0700:
	case RASTER_PROFILE_0710:
		return 2;
	case RASTER_PROFILE_0106:
		return 3;
	case RASTER_PROFILE_0107:
	case RASTER_PROFILE_010F:
		return 4;
	default:
		return 1;
	}
}

void configure_raster_pass(struct printer_state *printer, struct raster_band *band, uint32_t planes,
			   struct raster_pass *pass)
{
	struct printer_buffers *buffers = &printer->buffers;
	uint16_t mode = state_word(printer, STATE_RASTER_MODE);
	unsigned int scale = profile_width_scale(mode & RASTER_PROFILE_MASK);
	uint16_t depth = mode & RASTER_DEPTH_MASK;
	int32_t padded;

	pass->mode = mode;
	pass->compression = state_byte(printer, STATE_ROW_COMPRESSION);
	pass->mirrored = state_byte(printer, STATE_IMAGE_ORIENTATION) & ORIENTATION_MIRRORED;
	/* This legacy field is a nonzero plane-presence flag, not a layout selector. */
	pass->interlaced = planes;
	pass->raster_planes = (void *)buffers->raster_planes;
	pass->output_bytes = ((band->width + 7) >> 3) * scale;
	pass->row_bytes = (((int16_t)state_word(printer, STATE_INK_COLUMNS) + 7) >> 3) * scale;
	pass->first_byte = ((int16_t)state_word(printer, STATE_FIRST_INK_COLUMN) >> 3) * scale;
	pass->plane_stride = PLANE_STRIDE_BYTES;
	if (depth == RASTER_FOUR_LEVEL_DOTS || depth == RASTER_FIVE_LEVEL_DOTS) {
		scale = depth == RASTER_FOUR_LEVEL_DOTS ? 2 : 4;
		pass->row_bytes *= scale;
		pass->first_byte *= scale;
		pass->output_bytes *= scale;
	}
	/* Preserve the inherited meaning of this misleadingly named flag. */
	pass->monochrome = state_word(printer, STATE_COLOR_SELECTION) == OUTPUT_COLOR;
	if ((mode & RASTER_CHANNEL_MASK) == RASTER_MONOCHROME)
		pass->raster_planes += (int16_t)pass->plane_stride * (COLOR_CHANNELS - 1);
	pass->encoded_row = (void *)buffers->band;
	pass->scratch_row = (void *)buffers->pass_rows;
	pass->mirrored_row = (void *)buffers->band_scratch;
	pass->correction_flags &= ~UINT32_C(0xff);
	pass->bits_per_symbol = depth == RASTER_BINARY_DOTS ? 1 : depth == RASTER_FIVE_LEVEL_DOTS ? 4 : 2;
	pass->quantization_levels = depth == RASTER_BINARY_DOTS ? BINARY_DOT_LEVELS :
		depth == RASTER_FIVE_LEVEL_DOTS ? FIVE_DOT_LEVELS : FOUR_DOT_LEVELS;
	pass->channel_count = COLOR_CHANNELS;
	/* Retain the signed 16-bit conversion before the legacy alignment. */
	padded = (int16_t)pass->output_bytes + ROW_PADDING_BYTES - 1;
	if (padded < 0)
		padded += ROW_PADDING_BYTES - 1;
	pass->padded_row_bytes = (uint32_t)padded & ~(ROW_PADDING_BYTES - 1U);
	pass->encoded_pixels = (pass->padded_row_bytes * 8) / pass->bits_per_symbol;
}

static const struct scheduled_row *row_schedule(const struct raster_pass *pass, unsigned int index)
{
	const struct scheduled_row *const *rows = (const void *)pass->row_schedule;

	return rows[index];
}

void build_row_schedule(struct printer_state *printer, intptr_t output_address, int row_count)
{
	struct raster_pass pass = {0};
	uint16_t family = state_word(printer, STATE_RASTER_MODE) & RASTER_RESOLUTION_FAMILY_MASK;
	uint8_t flags = state_byte(printer, STATE_INTERLACE_FLAGS);
	uint8_t pass_index = state_byte(printer, STATE_PASS_INDEX);
	int16_t row = state_word(printer, STATE_ROW_INDEX);
	int16_t *output = (void *)output_address;
	int index;

	if (family == RASTER_PROFILE_0600)
		row_count *= flags & INTERLACE_ALTERNATE_RESOLUTION ? 8 : 4;
	else if (family == RASTER_PROFILE_0500)
		row_count *= 2;
	else if (family == RASTER_PROFILE_0700)
		row_count *= 10;
	if ((int32_t)state_dword(printer, STATE_HALF_RATE_ROWS) > 0)
		row_count /= 2;
	if (row_count > MAX_BATCH_ROWS)
		row_count = MAX_BATCH_ROWS;
	select_swath_layout(printer, &pass);
	if (pass.buffer_row == buffer_sequential_row) {
		uint16_t filled = swath_rows(&printer->swaths[0])->filled;

		if (four_channels(state_word(printer, STATE_RASTER_MODE)))
			filled /= COLOR_CHANNELS;
		for (index = 0; index < row_count; index++)
			output[index] = index + filled + pass.first_row;
		return;
	}
	for (index = 0; index < row_count; index++) {
		unsigned int schedule_index = pass_index;

		if (!(flags & INTERLACE_ROTATING_SWATHS))
			schedule_index %= pass.leading_rows;
		output[index] = pass.first_row + row_schedule(&pass, schedule_index)[row].row;
		row++;
		if (row == pass.pass_lengths[pass_index]) {
			pass_index++;
			if (!(flags & INTERLACE_ROTATING_SWATHS))
				pass_index %= pass.pass_count;
			row = 0;
		}
	}
}

uint32_t uses_legacy_interlace(struct printer_state *printer)
{
	uint16_t paper = state_word(printer, STATE_PAPER_ID);
	uint16_t family = state_word(printer, STATE_RASTER_MODE) & RASTER_RESOLUTION_FAMILY_MASK;
	bool envelope;

	if (photo_media(state_dword(printer, STATE_REQUESTED_MEDIA)))
		return 0;
	switch (paper) {
	case PAPER_ID_ISO_B5:
	case PAPER_ID_ENVELOPE_C5:
	case PAPER_ID_ENVELOPE_YOU4:
	case PAPER_ID_ENVELOPE_CHOU3:
	case PAPER_ID_ENVELOPE_C4:
	case PAPER_ID_ENVELOPE_10:
	case PAPER_ID_ENVELOPE_DL:
	case PAPER_ID_ENVELOPE_MONARCH:
	case PAPER_ID_ENVELOPE_CHOU4:
	case PAPER_ID_ENVELOPE_KAKU2:
	case PAPER_ID_VARIANT_132:
		envelope = true;
		break;
	default:
		envelope = (paper == PAPER_ID_CUSTOM ||
			    (uint16_t)(paper - CUSTOM_PAPER_FIRST) < CUSTOM_PAPER_COUNT) &&
			   state_byte(printer, STATE_THICKNESS) == THICKNESS_SETTING_ENVELOPE;
	}
	return envelope && (family == RASTER_PROFILE_0500 || family == RASTER_FAMILY_0300 ||
			    family == RASTER_MEDIUM_RESOLUTION_FAMILY);
}

uint8_t paper_allows_single_pass(struct printer_state *printer)
{
	uint16_t paper = state_word(printer, STATE_PAPER_ID);
	unsigned int alignment_bit = 0;

	if (state_byte(printer, STATE_BORDERLESS) == 1 ||
	    (state_word(printer, STATE_RASTER_MODE) & RASTER_RESOLUTION_FAMILY_MASK) !=
	    RASTER_MEDIUM_RESOLUTION_FAMILY || state_word(printer, STATE_QUALITY) == 1)
		return 0;
	switch (paper) {
	case PAPER_ID_A4:
	case PAPER_ID_LETTER:
	case PAPER_ID_A4_ALT:
	case PAPER_ID_LETTER_ALT:
		alignment_bit = 1;
		break;
	case PAPER_ID_A3:
	case PAPER_ID_TABLOID:
	case PAPER_ID_JIS_B4:
	case PAPER_ID_A3_BORDERLESS:
	case PAPER_ID_JIS_B4_BORDERLESS:
	case PAPER_ID_TABLOID_BORDERLESS:
	case PAPER_ID_A3_FAMILY_1:
	case PAPER_ID_JIS_B4_FAMILY_1:
	case PAPER_ID_TABLOID_FAMILY_1:
	case PAPER_ID_A3_FAMILY_2:
	case PAPER_ID_JIS_B4_FAMILY_2:
	case PAPER_ID_TABLOID_FAMILY_2:
		alignment_bit = 2;
	}
	return (state_byte(printer, STATE_ALIGNMENT_FLAGS) >> alignment_bit) & 1;
}

uint8_t choose_interlace_flags(struct printer_state *printer)
{
	uint16_t mode = state_word(printer, STATE_RASTER_MODE);
	uint16_t family = mode & RASTER_RESOLUTION_FAMILY_MASK;
	uint16_t paper = state_word(printer, STATE_PAPER_ID);
	uint32_t media = state_dword(printer, STATE_MEDIA_SELECTION);
	uint8_t flags = 0;

	if (state_byte(printer, STATE_BORDERLESS) == 1 && family != RASTER_LOW_RESOLUTION_FAMILY)
		flags = INTERLACE_BORDERLESS;
	if ((media == MEDIA_PLAIN || media == MEDIA_HAGAKI_ADDRESS) && family == RASTER_PROFILE_0600)
		flags |= flags & INTERLACE_SPECIAL_BOUNDS ? INTERLACE_PLAIN_BORDERLESS : INTERLACE_PLAIN;
	if (uses_legacy_interlace(printer))
		flags = INTERLACE_LEGACY;
	if (paper_allows_single_pass(printer)) {
		flags = INTERLACE_SINGLE_PASS;
		if (paper != PAPER_ID_A4 && paper != PAPER_ID_LETTER &&
		    paper != PAPER_ID_A4_ALT && paper != PAPER_ID_LETTER_ALT)
			flags |= INTERLACE_SMALL_PAPER;
	}
	if (!photo_media(media))
		return flags;
	if ((mode & RASTER_PROFILE_MASK) == RASTER_PROFILE_0600) {
		flags |= INTERLACE_ALTERNATE_RESOLUTION;
		if (state_word(printer, STATE_DUPLEX_CONTROL) == 1 &&
		    (paper == PAPER_ID_PHOTO_L || paper == PAPER_ID_PHOTO_L_ALT))
			flags |= INTERLACE_SMALL_PAPER;
	} else if ((mode & RASTER_PROFILE_MASK) == RASTER_PROFILE_0700 && small_photo_paper(paper)) {
		flags |= INTERLACE_SMALL_PAPER;
	}
	return flags;
}

uint32_t is_last_swath(struct printer_state *printer, struct raster_pass *pass)
{
	uint32_t end = page_feed_distance(printer);
	uint32_t current = state_dword(printer, STATE_FEED_POSITION);

	return end < current || (end - current) / pass->feed_step <= pass->flags;
}

uint32_t mask_randomized_pass(struct printer_state *printer, struct raster_pass *pass,
			      uint8_t *input, uint16_t length, intptr_t channel)
{
	static const uint8_t alternating[] = {0xaa, 0x55, 0xcc, 0x33};
	static const uint8_t five_pass[] = {0x84, 0x21, 0x08, 0x42, 0x10};
	const int16_t *noise = lpr_noise_table;
	uint16_t profile = pass->mode & RASTER_PROFILE_MASK;
	bool paired_bits = pass->mode & RASTER_FOUR_LEVEL_DOTS;
	uint8_t *output = pass->scratch_row;
	uint32_t phase, random_phase;
	int row, group, mask_index = 0, next_mask = 0;
	const uint8_t *masks = NULL;
	unsigned int index;

	if (profile == RASTER_PROFILE_0600 || profile == RASTER_PROFILE_0610) {
		phase = state_byte(printer, STATE_MASK_PHASE);
		phase = ((phase >> 4) + phase) & TWO_PASS_PHASE_MASK;
		for (index = 0; index < length; index++)
			output[index] = input[index] & alternating[phase + (paired_bits ? 2 : 0)];
		return phase;
	}
	if (profile != RASTER_PROFILE_0700 && profile != RASTER_PROFILE_0710)
		return profile;
	if (!paired_bits) {
		phase = (uint8_t)pass->correction_flags % FIVE_PASS_COUNT;
		for (index = 0; index < length; index++) {
			output[index] = input[index] & five_pass[phase];
			phase = (phase + 1) % FIVE_PASS_COUNT;
		}
		return phase;
	}
	row = (state_dword(printer, STATE_FEED_POSITION) >> 2) +
		(int16_t)state_word(printer, STATE_ROW_INDEX);
	group = row / FIVE_PASS_COUNT;
	/* Preserve the signed table values and integer division at each step. */
	random_phase = (noise[(channel * NOISE_CHANNEL_PHASE + group) & NOISE_TABLE_MASK] +
			NOISE_CENTER) * NOISE_PHASES / NOISE_RANGE;
	if (!length)
		return random_phase;
	for (index = 0; index < length; index++) {
		if (index % FIVE_PASS_COUNT == 0) {
			int noise_index = (NOISE_TABLE_MASK - (index + channel + group)) & NOISE_TABLE_MASK;
			int stride = (noise[noise_index] + NOISE_CENTER) * 2 / NOISE_RANGE + 2;
			int pattern = (noise[random_phase] + NOISE_CENTER) * FIVE_PASS_PATTERNS / NOISE_RANGE;

			mask_index = ((row % FIVE_PASS_COUNT) * stride) % FIVE_PASS_COUNT;
			masks = lpr_alternate_pass_masks + pattern * FIVE_PASS_COUNT;
			random_phase = (random_phase + 1) & NOISE_TABLE_MASK;
		}
		output[index] = input[index] & masks[mask_index];
		next_mask = mask_index + 1;
		mask_index = next_mask % FIVE_PASS_COUNT;
	}
	/* Callers ignore this value, but keep the original wrap indicator. */
	return next_mask / FIVE_PASS_COUNT;
}

uint32_t use_edge_pass_mask(struct printer_state *printer, struct raster_pass *pass)
{
	uint32_t page_feed = page_feed_distance(printer);
	uint32_t current = (int16_t)state_word(printer, STATE_ROW_INDEX) *
		(uint32_t)pass->feed_step + state_dword(printer, STATE_FEED_POSITION);
	bool borderless = state_byte(printer, STATE_INTERLACE_FLAGS) & INTERLACE_SPECIAL_BOUNDS;
	uint32_t leading = page_feed - (borderless ? BORDERLESS_LEADING_EDGE : NORMAL_LEADING_EDGE);
	uint32_t trailing = page_feed - (borderless ? BORDERLESS_TRAILING_EDGE : NORMAL_TRAILING_EDGE);
	const uint16_t *positions = pass->row_positions[(int32_t)pass->batch_rows / 2];
	uint32_t start[2], end[2];
	unsigned int index;

	if (!small_photo_paper(state_word(printer, STATE_PAPER_ID)))
		return 1;
	leading = leading / FEED_EDGE_QUANTUM * FEED_EDGE_QUANTUM;
	trailing = trailing / FEED_EDGE_QUANTUM * FEED_EDGE_QUANTUM;
	for (index = 0; index < 2; index++) {
		end[index] = current + ((uint32_t)pass->pass_rows - positions[index]) * FEED_UNITS_PER_ROW;
		start[index] = current - ((uint32_t)positions[index] - (uint32_t)pass->first_row) *
			FEED_UNITS_PER_ROW;
	}
	/* Unsigned wrap at the page edges is part of the inherited scheduling. */
	if (current < leading) {
		if (end[0] < leading || end[1] < leading)
			return current >= trailing && start[0] < trailing && start[1] < trailing;
	} else if (current >= trailing) {
		return start[0] < trailing && start[1] < trailing;
	}
	return 1;
}

uint8_t mask_edge_pass(struct printer_state *printer, const struct output_swath *reserved_swath,
		       struct raster_pass *pass, uint8_t *input, uint16_t length)
{
	static const uint8_t masks[2][2][4] = {
		{{0xaa, 0x00, 0x55, 0x00}, {0xcc, 0x00, 0x33, 0x00}},
		{{0x88, 0x22, 0x11, 0x44}, {0xc0, 0x0c, 0x03, 0x30}},
	};
	bool edge = use_edge_pass_mask(printer, pass);
	bool paired_bits = pass->mode & RASTER_FOUR_LEVEL_DOTS;
	uint8_t mask = masks[edge][paired_bits][pass->correction_flags & FOUR_PASS_PHASE_MASK];
	unsigned int index;

	(void)reserved_swath;
	for (index = 0; index < length; index++)
		pass->scratch_row[index] = input[index] & mask;
	return mask;
}

static bool needs_pass_mask(const struct raster_pass *pass)
{
	uint16_t profile = pass->mode & RASTER_PROFILE_MASK;

	return profile == RASTER_PROFILE_0600 || profile == RASTER_PROFILE_0610;
}

static void append_interlaced_channel(struct printer_state *printer, struct output_swath *swath,
				      struct raster_pass *pass, uint8_t *input,
				      unsigned int channel)
{
	struct swath_rows *rows = swath_rows(swath);
	static const uint8_t blank = COMMAND_BLANK_ROW;

	if (!trim_trailing_zero_bytes((intptr_t)input, pass->output_bytes))
		goto blank_row;
	if (needs_pass_mask(pass)) {
		if (!(state_byte(printer, STATE_INTERLACE_FLAGS) & INTERLACE_ALTERNATE_RESOLUTION))
			mask_randomized_pass(printer, pass, input, pass->output_bytes, channel);
		else if (!mask_edge_pass(printer, swath, pass, input, pass->output_bytes))
			goto blank_row;
		input = pass->scratch_row;
	}
	count_packed_symbols((intptr_t)input, pass->output_bytes, pass->bits_per_symbol,
			     (int *)rows->symbols[channel]);
	append_compressed_row(swath, (intptr_t)input, pass->output_bytes, pass);
	return;
blank_row:
	add_blank_symbols(rows, pass, channel, 1);
	append_output_bytes(swath, (void *)&blank, sizeof(blank));
}

void buffer_interlaced_row(struct printer_state *printer, struct raster_pass *pass)
{
	unsigned int pass_index = state_byte(printer, STATE_PASS_INDEX);
	bool rotating = state_byte(printer, STATE_INTERLACE_FLAGS) & INTERLACE_ROTATING_SWATHS;
	unsigned int schedule_index = rotating ? pass_index : pass_index % pass->leading_rows;
	const struct scheduled_row *schedule = row_schedule(pass, schedule_index);
	struct scheduled_row entry = schedule[(int16_t)state_word(printer, STATE_ROW_INDEX)];
	unsigned int target = rotating ? (unsigned int)entry.swath + pass->pass_count -
		state_byte(printer, STATE_TRAILING_PASSES) :
		entry.swath + pass->leading_rows * (pass_index / pass->leading_rows);
	unsigned int swath_index = target % pass->pass_count;
	struct output_swath *swath;
	struct swath_rows *rows;
	unsigned int channels = channel_count(pass->mode);
	uint16_t missing;
	unsigned int channel, index;

	assert(swath_index < PRINTER_SWATH_CAPACITY);
	swath = &printer->swaths[swath_index];
	rows = swath_rows(swath);
	missing = entry.row * channels - rows->filled;

	if (missing) {
		if (swath->cursor == swath->start) {
			rows->leading_blanks += missing;
		} else {
			for (index = 0; index < missing; index++)
				add_blank_symbols(rows, pass, (index + rows->filled) % channels, 1);
			append_blanks(swath, pass, missing);
		}
		rows->filled = entry.row * channels;
	}
	if (channels == COLOR_CHANNELS) {
		if (!pass->interlaced) {
			if (swath->cursor == swath->start) {
				rows->leading_blanks += channels;
			} else {
				for (channel = 0; channel < channels; channel++)
					add_blank_symbols(rows, pass, channel, 1);
				append_blanks(swath, pass, channels);
			}
		} else {
			const uint16_t *order = lpr_raster_channel_order;

			for (channel = 0; channel < channels; channel++)
				append_interlaced_channel(printer, swath, pass,
					pass->raster_planes + (int16_t)pass->plane_stride * order[channel], channel);
		}
	} else if ((!pass->interlaced ||
		    !trim_trailing_zero_bytes((intptr_t)pass->raster_planes, pass->output_bytes)) &&
		   swath->cursor == swath->start) {
		rows->leading_blanks++;
	} else if (!pass->interlaced) {
		add_blank_symbols(rows, pass, 0, 1);
		append_blanks(swath, pass, 1);
	} else {
		append_interlaced_channel(printer, swath, pass, pass->raster_planes, 0);
	}
	rows->filled += channels;
	if (needs_pass_mask(pass)) {
		uint8_t phase = pass->correction_flags;

		if (!(state_byte(printer, STATE_INTERLACE_FLAGS) & INTERLACE_ALTERNATE_RESOLUTION)) {
			phase = (phase + 1) & TWO_PASS_PHASE_MASK;
			store_byte(printer, STATE_MASK_PHASE,
				   (state_byte(printer, STATE_MASK_PHASE) + 1) & RANDOM_PHASE_MASK);
		} else {
			phase = (phase + 1) & FOUR_PASS_PHASE_MASK;
		}
		pass->correction_flags = (pass->correction_flags & ~UINT32_C(0xff)) | phase;
	}
	if (!(uint8_t)pass->correction_flags)
		pass->raster_planes += (int16_t)pass->plane_stride * COLOR_CHANNELS;
}

void buffer_sequential_row(struct printer_state *printer, struct raster_pass *pass)
{
	struct output_swath *swath = &printer->swaths[0];
	struct swath_rows *rows = swath_rows(swath);
	const uint16_t *order = lpr_jpeg_channel_order;
	unsigned int channels = channel_count(pass->mode);
	uint16_t lengths[COLOR_CHANNELS] = {0};
	uint16_t first = UINT16_MAX, last = 0;
	bool raw_rows = pass->compression == ROW_CODEC_RANGE_CODER;
	static const uint8_t blank = COMMAND_BLANK_ROW;
	unsigned int channel;

	if (raw_rows)
		memset(pass->scratch_row, 0, pass->padded_row_bytes);
	if (pass->interlaced) {
		for (channel = 0; channel < channels; channel++) {
			uint8_t *input = pass->raster_planes +
				(channels == COLOR_CHANNELS ? (int16_t)pass->plane_stride * order[channel] : 0);
			uint16_t leading = count_leading_zero_bytes((char *)input, pass->output_bytes);

			lengths[channel] = trim_trailing_zero_bytes((intptr_t)input, pass->output_bytes);
			if (leading < first)
				first = leading;
			if (lengths[channel] > last)
				last = lengths[channel];
		}
		if (first < rows->first_ink)
			rows->first_ink = first;
		if (last > rows->last_ink)
			rows->last_ink = last;
	}
	if (!last && swath->cursor == swath->start) {
		rows->skipped_lines++;
	} else {
		for (channel = 0; channel < channels; channel++) {
			uint8_t *input = pass->raster_planes +
				(channels == COLOR_CHANNELS ? (int16_t)pass->plane_stride * order[channel] : 0);

			if (!lengths[channel]) {
				if (raw_rows) {
					append_output_bytes(swath, pass->scratch_row,
							    (uint16_t)pass->padded_row_bytes);
				} else {
					add_blank_symbols(rows, pass, channel, 1);
					append_output_bytes(swath, (void *)&blank, sizeof(blank));
				}
			} else {
				if (!raw_rows)
					count_packed_symbols((intptr_t)input, pass->output_bytes,
						pass->bits_per_symbol, (int *)rows->symbols[channel]);
				append_compressed_row(swath, (intptr_t)input, pass->output_bytes, pass);
			}
		}
		rows->filled += channels;
	}
	pass->raster_planes += (int16_t)pass->plane_stride * COLOR_CHANNELS;
}

uint32_t swath_has_printable_data(const struct output_swath *swath)
{
	const uint8_t *bytes = (const void *)swath->start;
	int32_t length = swath->cursor - swath->start;
	int32_t index;

	/* Long buffers bypass the scan, including long sequences of blank rows. */
	if (length > SHORT_BLANK_SWATH_BYTES)
		return 1;
	for (index = 0; index < length; index++)
		if (bytes[index] != COMMAND_BLANK_ROW)
			return 1;
	return 0;
}

void reset_swath_buffer(struct output_swath *swath)
{
	struct swath_rows *rows = swath_rows(swath);

	rows->filled = 0;
	rows->reserved_count = 0;
	swath->cursor = swath->start;
	rows->reserved_flag = 0;
	rows->leading_blanks = 0;
	rows->skipped_lines = 0;
	rows->first_ink = UINT16_MAX;
	rows->last_ink = 0;
	memset(rows->symbols, 0, sizeof(rows->symbols));
	rows->padding_rows = 0;
}

void initialize_swath_buffers(struct printer_state *printer)
{
	unsigned int count = get_swath_count(printer);
	unsigned int index;

	for (index = 0; index < count; index++) {
		struct output_swath *swath;

		assert(index < PRINTER_SWATH_CAPACITY);
		swath = &printer->swaths[index];
		memset(swath->numeric_state, 0, sizeof(swath->numeric_state));
		swath->cursor = swath->start;
		swath_rows(swath)->first_ink = UINT16_MAX;
	}
}

static void complete_feed_if_needed(struct printer_state *printer)
{
	uint8_t flags = state_byte(printer, STATE_FEED_COMPLETION_FLAGS);

	if (state_dword(printer, STATE_FEED_COMPLETION_POSITION) <
	    state_dword(printer, STATE_FEED_POSITION) &&
	    (flags & FEED_BUFFERED) && !(flags & FEED_COMPLETION_SENT)) {
		emit_feed_completion();
		store_byte(printer, STATE_FEED_COMPLETION_FLAGS, flags | FEED_COMPLETION_SENT);
	}
}

static void advance_feed(struct printer_state *printer, uint32_t distance)
{
	emit_vertical_feed(printer, distance);
	store_dword(printer, STATE_FEED_POSITION, state_dword(printer, STATE_FEED_POSITION) + distance);
}

static void count_swath_padding(struct swath_rows *rows, const struct raster_pass *pass)
{
	int filled = (int16_t)rows->filled;
	unsigned int channel;

	if ((pass->mode & RASTER_CHANNEL_MASK) == RASTER_MONOCHROME) {
		add_blank_symbols(rows, pass, 0, (uint32_t)pass->nozzle_rows - filled);
	} else {
		int remaining = ((int)pass->nozzle_rows * COLOR_CHANNELS - filled) / COLOR_CHANNELS;

		for (channel = 0; channel < COLOR_CHANNELS; channel++)
			add_blank_symbols(rows, pass, channel, remaining);
	}
}

static void write_swath_rows(struct printer_state *printer, struct output_swath *swath,
			     struct raster_pass *pass, uint16_t leading, uint16_t trailing)
{
	if (pass->mode & RASTER_FIVE_LEVEL_DOTS) {
		count_swath_padding(swath_rows(swath), pass);
		emit_ink_dot_counts(printer, swath);
	}
	if (leading)
		emit_blank_rows(printer, pass, leading);
	lpr_write_callback((void *)swath->start, swath->cursor - swath->start);
	if (trailing)
		emit_blank_rows(printer, pass, trailing);
}

void flush_current_swath(struct printer_state *printer, struct raster_pass *pass)
{
	uint8_t flags = state_byte(printer, STATE_INTERLACE_FLAGS);
	uint8_t pass_index = state_byte(printer, STATE_PASS_INDEX);
	const uint16_t *order = (const void *)pass->swath_order;
	const uint16_t *feed = (const void *)pass->feed_schedule;
	unsigned int swath_index = order[pass_index];
	struct output_swath *swath;
	struct swath_rows *rows;
	uint32_t distance;

	if (flags & INTERLACE_ROTATING_SWATHS)
		swath_index = (int32_t)(swath_index + pass->pass_count -
				       state_byte(printer, STATE_TRAILING_PASSES)) % (int)pass->pass_count;
	assert(swath_index < PRINTER_SWATH_CAPACITY);
	swath = &printer->swaths[swath_index];
	rows = swath_rows(swath);
	distance = (feed[pass_index] + rows->skipped_lines) * (uint32_t)pass->feed_step;
	if ((flags & INTERLACE_SINGLE_PASS) == INTERLACE_SINGLE_PASS)
		distance -= rows->padding_rows * FEED_UNITS_PER_ROW;
	advance_feed(printer, distance);
	if (swath_has_printable_data(swath)) {
		if (state_dword(printer, STATE_CORRECTION)) {
			bool reverse = paper_allows_single_pass(printer);

			if (!reverse) {
				store_word(printer, STATE_CORRECTION_ROWS,
					   rows->filled / channel_count(pass->mode));
				reverse = needs_row_correction(
				    (int *)(printer->numeric_state + STATE_CORRECTION));
			}
			lpr_write_callback(reverse ? lpr_command_direction_reverse :
					   lpr_command_direction_forward, 2);
		}
		lpr_write_callback(lpr_command_flush_swath, 5);
		if (pass->compression == ROW_CODEC_RANGE_CODER)
			emit_range_encoded_swath(printer, pass, swath);
		else
			write_swath_rows(printer, swath, pass, rows->leading_blanks,
					pass->nozzle_rows * channel_count(pass->mode) - rows->filled);
	}
	if (!(flags & INTERLACE_ROTATING_SWATHS)) {
		pass_index = (pass_index + 1) % (uint8_t)pass->pass_count;
	} else {
		uint8_t last_pass = pass->pass_count + pass->trailing_rows;

		pass_index++;
		if (!is_last_swath(printer, pass)) {
			if (pass_index >= last_pass)
				pass_index -= pass->pass_count;
		} else if (pass_index < last_pass) {
			store_byte(printer, STATE_TRAILING_PASSES, last_pass - pass_index);
			pass_index = last_pass;
		}
	}
	store_byte(printer, STATE_PASS_INDEX, pass_index);
	store_word(printer, STATE_ROW_INDEX, 0);
	reset_swath_buffer(swath);
	if ((flags & INTERLACE_SINGLE_PASS) == INTERLACE_SINGLE_PASS)
		pad_final_swath(printer, pass);
	complete_feed_if_needed(printer);
}

void pad_final_swath(struct printer_state *printer, struct raster_pass *pass)
{
	uint32_t end = page_feed_distance(printer);
	uint32_t current = state_dword(printer, STATE_FEED_POSITION);
	uint32_t remaining;
	int16_t padding;
	uint16_t blanks;
	unsigned int channels = channel_count(pass->mode), channel;
	struct output_swath *swath = &printer->swaths[0];
	struct swath_rows *rows = swath_rows(swath);

	if (current >= end)
		return;
	remaining = (end - current) / pass->feed_step;
	if (remaining >= pass->flags)
		return;
	padding = (uint16_t)pass->flags - (int16_t)remaining;
	if (!padding)
		return;
	for (channel = 0; channel < channels; channel++)
		add_blank_symbols(rows, pass, channel, padding);
	blanks = padding * channels;
	append_blanks(swath, pass, blanks);
	rows->filled = blanks;
	rows->padding_rows = padding;
	store_word(printer, STATE_ROW_INDEX, padding);
}

void flush_remaining_swaths(struct printer_state *printer, struct raster_pass *pass)
{
	const uint16_t *padding = (const void *)pass->padding_schedule;
	const uint16_t *order = (const void *)pass->swath_order;
	const uint16_t *feed = (const void *)pass->feed_schedule;
	unsigned int channels = channel_count(pass->mode);
	uint16_t padding_rows = 0;
	unsigned int index;

	for (index = 0; index < pass->pass_count; index++) {
		unsigned int pass_index = state_byte(printer, STATE_PASS_INDEX);
		unsigned int count = (uint8_t)pass->pass_count;
		unsigned int swath_index = (order[pass_index] + padding[index]) % count;
		struct output_swath *swath;
		struct swath_rows *rows;
		int32_t unfilled;
		uint16_t remaining;
		uint32_t distance;
		uint32_t reduction;

		assert(swath_index < PRINTER_SWATH_CAPACITY);
		swath = &printer->swaths[swath_index];
		rows = swath_rows(swath);
		unfilled = pass->nozzle_rows * channels - (int16_t)rows->filled;
		remaining = unfilled / (int)channels;
		distance = feed[(pass_index + padding[index]) % count] * (uint32_t)pass->feed_step;
		reduction = remaining * FEED_UNITS_PER_ROW;

		/* If a later feed is too short, retain the previous padding_rows value. */
		if (distance < reduction)
			reduction = 0;
		else
			padding_rows = remaining;
		advance_feed(printer, distance - reduction);
		if (swath_has_printable_data(swath)) {
			lpr_write_callback(lpr_command_flush_swath, 5);
			write_swath_rows(printer, swath, pass,
					rows->leading_blanks + padding_rows * channels,
					(pass->nozzle_rows - padding_rows) * channels - rows->filled);
		}
		reset_swath_buffer(swath);
		complete_feed_if_needed(printer);
	}
}

static int raster_batch_rows(struct printer_state *printer, int row_count, uint16_t mode)
{
	int limit = 16;

	switch (mode & RASTER_PROFILE_MASK) {
	case RASTER_PROFILE_010F:
		row_count *= 2;
		break;
	case RASTER_PROFILE_0201:
	case RASTER_PROFILE_0211:
		if ((int32_t)state_dword(printer, STATE_HALF_RATE_ROWS) > 0)
			row_count = (row_count + 1) / 2;
		/* fall through */
	case RASTER_PROFILE_0205:
	case RASTER_PROFILE_0215:
		limit = 1;
		break;
	case RASTER_PROFILE_0500:
	case RASTER_PROFILE_0508:
	case RASTER_PROFILE_0510:
	case RASTER_PROFILE_0518:
		row_count *= 2;
		limit = 8;
		break;
	case RASTER_PROFILE_0600:
	case RASTER_PROFILE_0610:
		if (state_byte(printer, STATE_INTERLACE_FLAGS) & INTERLACE_ALTERNATE_RESOLUTION) {
			row_count *= 8;
			limit = 32;
			break;
		}
		/* fall through */
	case RASTER_PROFILE_0608:
	case RASTER_PROFILE_0618:
		row_count *= 4;
		break;
	case RASTER_PROFILE_0700:
	case RASTER_PROFILE_0710:
		row_count *= 10;
		limit = MAX_BATCH_ROWS;
		break;
	}
	return row_count < limit ? row_count : limit;
}

void process_raster_pass(struct printer_state *printer, int row_count, struct raster_band *band,
			 uint32_t planes)
{
	struct raster_pass pass = {0};
	uint8_t ink_mode;
	int index;

	configure_raster_pass(printer, band, planes, &pass);
	if (state_byte(printer, STATE_INTERLACE_FLAGS) & INTERLACE_ALTERNATE_RESOLUTION)
		build_row_schedule(printer, (intptr_t)pass.row_positions, row_count);
	row_count = raster_batch_rows(printer, row_count, pass.mode);
	if ((state_word(printer, STATE_BAND_PHASE) == BAND_FIRST || !printer->swaths[0].start) &&
	    state_word(printer, STATE_FEED_OFFSET_MULTIPLIER)) {
		uint32_t height = band->height;
		uint32_t limit, current;

		if ((int32_t)height < 0)
			height += 7;
		limit = (height & ~7U) * (FEED_UNITS_PER_INCH / (int16_t)state_word(printer, STATE_RENDER_DPI)) *
			state_word(printer, STATE_FEED_OFFSET_MULTIPLIER);
		current = state_dword(printer, STATE_FEED_POSITION);
		if (current < limit) {
			uint8_t flags = state_byte(printer, STATE_FEED_COMPLETION_FLAGS);

			if (state_dword(printer, STATE_FEED_COMPLETION_POSITION) < limit &&
			    (flags & FEED_BUFFERED) && !(flags & FEED_COMPLETION_SENT)) {
				emit_feed_completion();
				store_byte(printer, STATE_FEED_COMPLETION_FLAGS, flags | FEED_COMPLETION_SENT);
			}
			advance_feed(printer, limit - current);
		}
	}
	if (pass.interlaced) {
		bool mono = (pass.mode & RASTER_CHANNEL_MASK) == RASTER_MONOCHROME;

		switch (pass.mode & RASTER_DEPTH_MASK) {
		case RASTER_FOUR_LEVEL_DOTS:
			ink_mode = mono ? INK_FOUR_LEVEL_MONO : INK_FOUR_LEVEL_COLOR;
			break;
		case RASTER_FIVE_LEVEL_DOTS:
			ink_mode = mono ? INK_FIVE_LEVEL_MONO : INK_FIVE_LEVEL_COLOR;
			break;
		default:
			ink_mode = mono ? INK_BINARY_MONO : INK_BINARY_COLOR;
		}
		if (ink_mode != state_byte(printer, STATE_CURRENT_INK_MODE)) {
			store_byte(printer, STATE_CURRENT_INK_MODE, ink_mode);
			emit_raster_color_mode(printer);
		}
	}
	pass.plane_flags = state_byte(printer, STATE_CURRENT_INK_MODE) >= INK_FOUR_LEVEL_MONO ?
		MULTILEVEL_PLANE_FLAG : 0;
	select_swath_layout(printer, &pass);
	if (state_word(printer, STATE_BAND_PHASE) == BAND_FIRST) {
		store_byte(printer, STATE_PASS_INDEX, 0);
		store_byte(printer, STATE_TRAILING_PASSES, 0);
		store_word(printer, STATE_ROW_INDEX, 0);
		if (pass.feed_rows)
			advance_feed(printer, pass.feed_rows * (uint32_t)pass.feed_step);
	}
	for (index = 0; index < row_count; index++) {
		int16_t row;
		bool full;

		pass.batch_rows = index;
		pass.buffer_row(printer, &pass);
		row = state_word(printer, STATE_ROW_INDEX) + 1;
		store_word(printer, STATE_ROW_INDEX, row);
		if (pass.buffer_row == buffer_sequential_row)
			full = (int16_t)swath_rows(&printer->swaths[0])->filled ==
			       (int16_t)(pass.pass_lengths[0] * channel_count(pass.mode));
		else
			full = row == pass.pass_lengths[state_byte(printer, STATE_PASS_INDEX)];
		if (full)
			flush_current_swath(printer, &pass);
	}
	if (state_word(printer, STATE_BAND_PHASE) == BAND_LAST) {
		uint8_t flags = state_byte(printer, STATE_INTERLACE_FLAGS);

		if (!(flags & INTERLACE_LEGACY) ||
		    (flags & INTERLACE_SINGLE_PASS) == INTERLACE_SINGLE_PASS) {
			for (index = 0; index < pass.pass_count; index++)
				flush_current_swath(printer, &pass);
		} else {
			flush_remaining_swaths(printer, &pass);
		}
	}
}
