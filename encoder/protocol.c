/* Serialize the Brother raster/JPEG dialect described in PROTOCOL.md. */
#include "protocol.h"
#include "data.h"
#include "state.h"
#include "swath.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

struct code_mapping {
	uint32_t selection;
	uint16_t code;
};

/* Native renderer fields retain their layout; wire integers are written below. */
static uint8_t state_byte(struct printer_state *printer, enum protocol_state_offset offset)
{
	return printer->numeric_state[offset];
}

static uint16_t state_word(struct printer_state *printer, enum protocol_state_offset offset)
{
	uint16_t value;

	memcpy(&value, printer->numeric_state + offset, sizeof(value));
	return value;
}

static uint32_t state_dword(struct printer_state *printer, enum protocol_state_offset offset)
{
	uint32_t value;

	memcpy(&value, printer->numeric_state + offset, sizeof(value));
	return value;
}

static void store_byte(struct printer_state *printer, enum protocol_state_offset offset,
		       uint8_t value)
{
	printer->numeric_state[offset] = value;
}

static void store_dword(struct printer_state *printer, enum protocol_state_offset offset,
			uint32_t value)
{
	memcpy(printer->numeric_state + offset, &value, sizeof(value));
}

static void put_u16le(uint8_t *output, uint16_t value)
{
	output[0] = value;
	output[1] = value >> 8;
}

static void put_u32le(uint8_t *output, uint32_t value)
{
	put_u16le(output, value);
	put_u16le(output + sizeof(uint16_t), value >> 16);
}

static uint16_t lookup_code(const struct code_mapping *table, size_t count,
			    uint32_t selection, uint16_t fallback)
{
	size_t index;

	for (index = 0; index < count; index++) {
		if (table[index].selection == selection)
			return table[index].code;
	}
	return fallback;
}

static void write_command(const uint8_t *command, size_t length)
{
	lpr_write_callback(command, length);
}

static void write_byte_parameter(uint8_t opcode, uint8_t value)
{
	const uint8_t command[] = { opcode, value };

	write_command(command, sizeof(command));
}

static void write_word_parameter(uint8_t opcode, uint16_t value)
{
	uint8_t command[1 + sizeof(value)] = { opcode };

	put_u16le(command + 1, value);
	write_command(command, sizeof(command));
}

static void write_subcommand(uint8_t opcode, uint8_t selector, uint8_t value)
{
	const uint8_t command[] = { opcode, selector, value };

	write_command(command, sizeof(command));
}

static void write_flag(uint8_t selector, bool enabled)
{
	uint8_t command[] = {
		COMMAND_CHECKSUMMED_FLAG, selector, FLAG_PAYLOAD_BYTES,
		CONTROL_NUL, enabled, 0,
	};
	size_t checksum_index = sizeof(command) - 1;
	size_t index;

	/* Including the checksum, the sum of all command bytes is zero. */
	for (index = 0; index < checksum_index; index++)
		command[checksum_index] -= command[index];
	write_command(command, sizeof(command));
}

static void initialize_printer(void)
{
	const uint8_t command[] = { CONTROL_ESCAPE, COMMAND_INITIALIZE };

	write_command(command, sizeof(command));
}

static void select_language(uint8_t language, uint8_t raster_mode)
{
	const uint8_t command[] = {
		CONTROL_ESCAPE, COMMAND_LANGUAGE, language, raster_mode,
		CONTROL_CARRIAGE_RETURN,
	};

	write_command(command, sizeof(command));
}

static void initialize_page_origin(void)
{
	/* Preserve Y before X. Nonzero coordinate units are not established. */
	write_word_parameter(COMMAND_Y_ORIGIN, 0);
	write_word_parameter(COMMAND_X_ORIGIN, 0);
}

void emit_reset_padding(void)
{
	static const uint8_t padding[INITIALIZATION_BLOCK_BYTES];
	size_t sent;

	for (sent = 0; sent < INITIALIZATION_PADDING_BYTES; sent += sizeof(padding))
		write_command(padding, sizeof(padding));
}

uint32_t begin_protocol_job(struct printer_state *printer)
{
	if (state_byte(printer, STATE_IMAGE_ENCODING) == ENCODING_JPEG)
		begin_jpeg_job(printer);
	else
		begin_raster_job(printer);
	return 0;
}

uint32_t end_protocol_job(struct printer_state *printer)
{
	(void)printer;
	initialize_printer();
	return 0;
}

uint32_t begin_protocol_page(struct printer_state *printer, uint32_t page_number)
{
	uint8_t encoding = state_byte(printer, STATE_IMAGE_ENCODING);

	if (encoding == ENCODING_JPEG)
		begin_jpeg_page(printer, page_number);
	else if (encoding != ENCODING_DEFERRED_HEADER)
		begin_raster_page(printer, page_number);
	return 0;
}

uint32_t end_protocol_page(struct printer_state *printer)
{
	static const uint8_t jpeg_end[] = {
		COMMAND_RASTER_DATA, CONTROL_NUL, CONTROL_NUL,
		JPEG_END_PAGE_HIGH_BYTE,
	};
	static const uint8_t form_feed = CONTROL_FORM_FEED;

	if (state_byte(printer, STATE_PAGE_END_ENCODING) == ENCODING_JPEG)
		write_command(jpeg_end, sizeof(jpeg_end));
	else
		write_command(&form_feed, sizeof(form_feed));
	return 0;
}

void begin_raster_job(struct printer_state *printer)
{
	(void)printer;
	emit_reset_padding();
	select_language(LANGUAGE_RASTER, RASTER_ROWS);
	initialize_printer();
}

void begin_raster_page(struct printer_state *printer, uint32_t page_number)
{
	bool range_coded = state_byte(printer, STATE_ROW_COMPRESSION) ==
			   ROW_CODEC_RANGE_CODER;

	select_language(LANGUAGE_RASTER,
			range_coded ? RASTER_RANGE_SWATHS : RASTER_ROWS);
	emit_raster_resolution(printer);
	select_raster_color_mode(printer);
	emit_compression_mode(printer);
	emit_bidirectional_mode(printer);
	emit_paper_thickness(printer);
	emit_media_type(printer);
	emit_nozzle_range(printer);
	emit_paper_source(printer);
	emit_paper_size(printer);
	emit_printable_area(printer);
	emit_duplex_drying_mode(printer);
	emit_duplex_side(printer, page_number);
	emit_paper_family_mode(printer);
	emit_color_flag(printer);
	emit_single_pass_flag(printer);
	emit_paper_control_flag(printer);
	initialize_page_origin();
	emit_reset_padding();
	fflush(stdout);
}

void emit_raster_resolution(struct printer_state *printer)
{
	static const struct code_mapping resolutions[] = {
		{ RASTER_PROFILE_0100, RESOLUTION_CODE_00_00 },
		{ RASTER_PROFILE_0102, RESOLUTION_CODE_11_00 },
		{ RASTER_PROFILE_0103, RESOLUTION_CODE_02_00 },
		{ RASTER_PROFILE_0106, RESOLUTION_CODE_11_00 },
		{ RASTER_PROFILE_0107, RESOLUTION_CODE_02_00 },
		{ RASTER_PROFILE_010E, RESOLUTION_CODE_11_01 },
		{ RASTER_PROFILE_010F, RESOLUTION_CODE_02_01 },
		{ RASTER_PROFILE_0200, RESOLUTION_CODE_01_01 },
		{ RASTER_PROFILE_0201, RESOLUTION_CODE_02_01 },
		{ RASTER_PROFILE_0205, RESOLUTION_CODE_02_01 },
		{ RASTER_PROFILE_0210, RESOLUTION_CODE_01_01 },
		{ RASTER_PROFILE_0211, RESOLUTION_CODE_02_01 },
		{ RASTER_PROFILE_0215, RESOLUTION_CODE_02_01 },
		{ RASTER_PROFILE_0500, RESOLUTION_CODE_03_03 },
		{ RASTER_PROFILE_0508, RESOLUTION_CODE_02_03 },
		{ RASTER_PROFILE_0510, RESOLUTION_CODE_03_03 },
		{ RASTER_PROFILE_0518, RESOLUTION_CODE_02_03 },
		{ RASTER_PROFILE_0608, RESOLUTION_CODE_02_04 },
		{ RASTER_PROFILE_0618, RESOLUTION_CODE_02_04 },
		{ RASTER_PROFILE_0700, RESOLUTION_CODE_03_06 },
		{ RASTER_PROFILE_0710, RESOLUTION_CODE_03_06 },
	};
	uint16_t profile = state_word(printer, STATE_RASTER_MODE) &
			   RASTER_PROFILE_MASK;
	uint16_t code = lookup_code(resolutions, ARRAY_SIZE(resolutions), profile,
				    RESOLUTION_CODE_02_02);

	if (profile == RASTER_PROFILE_0600 || profile == RASTER_PROFILE_0610) {
		bool alternate = state_byte(printer, STATE_INTERLACE_FLAGS) &
				 INTERLACE_ALTERNATE_RESOLUTION;
		uint8_t encoding = state_byte(printer, STATE_IMAGE_ENCODING);
		/* This legacy test deliberately includes both adjacent bytes. */
		bool deferred = state_word(printer, STATE_IMAGE_ENCODING) ==
				ENCODING_DEFERRED_HEADER;

		code = alternate && (encoding == ENCODING_RASTER || deferred) ?
			RESOLUTION_CODE_03_06 : RESOLUTION_CODE_03_04;
	}
	write_word_parameter(COMMAND_RESOLUTION, code);
}

void select_raster_color_mode(struct printer_state *printer)
{
	uint16_t mode = state_word(printer, STATE_RASTER_MODE);
	bool monochrome = (mode & RASTER_CHANNEL_MASK) == RASTER_MONOCHROME;
	uint8_t ink_mode;

	switch (mode & RASTER_DEPTH_MASK) {
	case RASTER_BINARY_DOTS:
		ink_mode = monochrome ? INK_BINARY_MONO : INK_BINARY_COLOR;
		break;
	case RASTER_FIVE_LEVEL_DOTS:
		ink_mode = monochrome ? INK_FIVE_LEVEL_MONO : INK_FIVE_LEVEL_COLOR;
		break;
	default:
		ink_mode = monochrome ? INK_FOUR_LEVEL_MONO : INK_FOUR_LEVEL_COLOR;
		break;
	}
	store_byte(printer, STATE_CURRENT_INK_MODE, ink_mode);
	emit_raster_color_mode(printer);
}

void emit_raster_color_mode(struct printer_state *printer)
{
	write_byte_parameter(COMMAND_INK_MODE,
			     state_byte(printer, STATE_CURRENT_INK_MODE));
}

void emit_compression_mode(struct printer_state *printer)
{
	static const struct code_mapping codecs[] = {
		{ ROW_CODEC_ZERO_RUNS, COMPRESSION_ZERO_RUNS },
		{ ROW_CODEC_WORD_RUNS, COMPRESSION_WORD_RUNS },
		{ ROW_CODEC_RANGE_CODER, COMPRESSION_RANGE_CODER },
	};
	uint8_t code = lookup_code(codecs, ARRAY_SIZE(codecs),
				  state_byte(printer, STATE_ROW_COMPRESSION),
				  COMPRESSION_PACKBITS);

	write_byte_parameter(COMMAND_COMPRESSION, code);
}

void emit_bidirectional_mode(struct printer_state *printer)
{
	uint8_t mask = BIDIRECTIONAL_PLAIN_MEDIA;
	uint16_t profile = state_word(printer, STATE_RASTER_MODE) &
			   RASTER_PROFILE_MASK;
	bool enabled;

	switch (state_dword(printer, STATE_MEDIA_SELECTION)) {
	case MEDIA_INKJET:
	case MEDIA_OTHER_PHOTO:
	case MEDIA_VARIANT_17:
	case MEDIA_BROTHER_BP71:
	case MEDIA_HAGAKI_REVERSE:
	case MEDIA_VARIANT_1C:
	case MEDIA_VARIANT_1D:
	case MEDIA_BROTHER_BP60:
	case MEDIA_VARIANT_28:
		mask = BIDIRECTIONAL_PHOTO_MEDIA;
		break;
	}
	enabled = state_byte(printer, STATE_BIDIRECTIONAL_FLAGS) & mask;
	if (profile == RASTER_PROFILE_0700 || profile == RASTER_PROFILE_0710 ||
	    paper_allows_single_pass(printer))
		enabled = false;
	write_byte_parameter(COMMAND_BIDIRECTIONAL,
			     enabled ? BIDIRECTIONAL_ALLOWED : BIDIRECTIONAL_DISABLED);
}

void emit_paper_thickness(struct printer_state *printer)
{
	static const struct code_mapping thicknesses[] = {
		{ THICKNESS_SETTING_THICK, THICKNESS_THICK },
		{ THICKNESS_SETTING_ENVELOPE, THICKNESS_ENVELOPE },
	};
	uint8_t thickness = lookup_code(thicknesses, ARRAY_SIZE(thicknesses),
				       state_byte(printer, STATE_THICKNESS),
				       THICKNESS_REGULAR);

	write_subcommand(COMMAND_MEDIA, MEDIA_THICKNESS, thickness);
}

void emit_media_type(struct printer_state *printer)
{
	static const struct code_mapping media[] = {
		{ MEDIA_INKJET, MEDIA_CODE_INKJET },
		{ MEDIA_BROTHER_BP60, MEDIA_CODE_INKJET },
		{ MEDIA_OTHER_PHOTO, MEDIA_CODE_OTHER_PHOTO },
		{ MEDIA_VARIANT_17, MEDIA_CODE_VARIANT_17 },
		{ MEDIA_BROTHER_BP71, MEDIA_CODE_BROTHER_BP71 },
		{ MEDIA_HAGAKI_ADDRESS, MEDIA_CODE_HAGAKI_ADDRESS },
		{ MEDIA_HAGAKI_REVERSE, MEDIA_CODE_HAGAKI_REVERSE },
		{ MEDIA_VARIANT_1C, MEDIA_CODE_VARIANT_1C },
		{ MEDIA_VARIANT_1D, MEDIA_CODE_VARIANT_1D },
		{ MEDIA_VARIANT_28, MEDIA_CODE_VARIANT_28 },
	};
	uint8_t code = lookup_code(media, ARRAY_SIZE(media),
				  state_dword(printer, STATE_MEDIA_SELECTION),
				  MEDIA_CODE_PLAIN);

	write_subcommand(COMMAND_MEDIA, MEDIA_TYPE, code);
}

void emit_nozzle_range(struct printer_state *printer)
{
	struct raster_pass pass = { 0 };
	uint8_t command[2 + 2 * sizeof(uint16_t)] = {
		COMMAND_NOZZLES, NOZZLE_RANGE,
	};

	select_swath_layout(printer, &pass);
	put_u16le(command + 2, pass.first_row);
	put_u16le(command + 2 + sizeof(uint16_t), pass.pass_rows);
	write_command(command, sizeof(command));
}

void emit_paper_source(struct printer_state *printer)
{
	static const struct code_mapping sources[] = {
		{ SOURCE_SETTING_TRAY_1, SOURCE_TRAY_1 },
		{ SOURCE_SETTING_TRAY_2, SOURCE_TRAY_2 },
		{ SOURCE_SETTING_MULTIPURPOSE, SOURCE_MULTIPURPOSE },
		{ SOURCE_SETTING_MANUAL, SOURCE_MANUAL },
	};
	uint8_t code = lookup_code(sources, ARRAY_SIZE(sources),
				  state_word(printer, STATE_PAPER_SOURCE),
				  SOURCE_AUTOMATIC);

	write_byte_parameter(COMMAND_PAPER_SOURCE, code);
}

void emit_paper_size(struct printer_state *printer)
{
	static const struct code_mapping papers[] = {
		{ PAPER_ID_LETTER, PAPER_CODE_LETTER },
		{ PAPER_ID_LETTER_ALT, PAPER_CODE_LETTER },
		{ PAPER_ID_EXECUTIVE, PAPER_CODE_EXECUTIVE },
		{ PAPER_ID_A4, PAPER_CODE_A4 },
		{ PAPER_ID_A4_ALT, PAPER_CODE_A4 },
		{ PAPER_ID_JIS_B5, PAPER_CODE_JIS_B5 },
		{ PAPER_ID_TABLOID, PAPER_CODE_TABLOID },
		{ PAPER_ID_TABLOID_BORDERLESS, PAPER_CODE_TABLOID },
		{ PAPER_ID_TABLOID_FAMILY_1, PAPER_CODE_TABLOID },
		{ PAPER_ID_TABLOID_FAMILY_2, PAPER_CODE_TABLOID },
		{ PAPER_ID_A3, PAPER_CODE_A3 },
		{ PAPER_ID_A3_BORDERLESS, PAPER_CODE_A3 },
		{ PAPER_ID_A3_FAMILY_1, PAPER_CODE_A3 },
		{ PAPER_ID_A3_FAMILY_2, PAPER_CODE_A3 },
		{ PAPER_ID_JIS_B4, PAPER_CODE_JIS_B4 },
		{ PAPER_ID_JIS_B4_BORDERLESS, PAPER_CODE_JIS_B4 },
		{ PAPER_ID_JIS_B4_FAMILY_1, PAPER_CODE_JIS_B4 },
		{ PAPER_ID_JIS_B4_FAMILY_2, PAPER_CODE_JIS_B4 },
		{ PAPER_ID_LEGAL, PAPER_CODE_LEGAL },
		{ PAPER_ID_A5, PAPER_CODE_A5 },
		{ PAPER_ID_A6, PAPER_CODE_A6 },
		{ PAPER_ID_A6_ALT, PAPER_CODE_A6 },
		{ PAPER_ID_FOLIO, PAPER_CODE_FOLIO },
		{ PAPER_ID_ISO_B5, PAPER_CODE_ISO_B5 },
		{ PAPER_ID_JIS_B6, PAPER_CODE_JIS_B6 },
		{ PAPER_ID_ENVELOPE_10, PAPER_CODE_ENVELOPE_10 },
		{ PAPER_ID_ENVELOPE_DL, PAPER_CODE_ENVELOPE_DL },
		{ PAPER_ID_ENVELOPE_C5, PAPER_CODE_ENVELOPE_C5 },
		{ PAPER_ID_ENVELOPE_C4, PAPER_CODE_ENVELOPE_C4 },
		{ PAPER_ID_ENVELOPE_MONARCH, PAPER_CODE_ENVELOPE_MONARCH },
		{ PAPER_ID_ENVELOPE_KAKU2, PAPER_CODE_ENVELOPE_KAKU2 },
		{ PAPER_ID_ENVELOPE_CHOU3, PAPER_CODE_ENVELOPE_CHOU3 },
		{ PAPER_ID_ENVELOPE_CHOU4, PAPER_CODE_ENVELOPE_CHOU4 },
		{ PAPER_ID_ENVELOPE_YOU4, PAPER_CODE_ENVELOPE_YOU4 },
		{ PAPER_ID_DOUBLE_HAGAKI, PAPER_CODE_DOUBLE_HAGAKI },
		{ PAPER_ID_HAGAKI, PAPER_CODE_HAGAKI },
		{ PAPER_ID_HAGAKI_ALT, PAPER_CODE_HAGAKI },
		{ PAPER_ID_PHOTO_4X6, PAPER_CODE_PHOTO_4X6 },
		{ PAPER_ID_PHOTO_4X6_ALT, PAPER_CODE_PHOTO_4X6 },
		{ PAPER_ID_PHOTO_5X8, PAPER_CODE_PHOTO_5X8 },
		{ PAPER_ID_PHOTO_5X8_ALT, PAPER_CODE_PHOTO_5X8 },
		{ PAPER_ID_PHOTO_L, PAPER_CODE_PHOTO_L },
		{ PAPER_ID_PHOTO_L_ALT, PAPER_CODE_PHOTO_L },
		{ PAPER_ID_PHOTO_2L, PAPER_CODE_PHOTO_2L },
		{ PAPER_ID_PHOTO_2L_ALT, PAPER_CODE_PHOTO_2L },
		{ PAPER_ID_VARIANT_132, PAPER_CODE_VARIANT_132 },
		{ PAPER_ID_VARIANT_133, PAPER_CODE_VARIANT_133 },
		{ PAPER_ID_DISC_12CM, PAPER_CODE_DISC_12CM },
	};
	uint16_t paper = state_word(printer, STATE_PAPER_ID);
	bool rotated = state_dword(printer, STATE_PAPER_ROTATED) != 0;
	uint8_t code = lookup_code(papers, ARRAY_SIZE(papers), paper, PAPER_CODE_CUSTOM);

	if (rotated) {
		switch (paper) {
		case PAPER_ID_LETTER:
		case PAPER_ID_LETTER_ALT:
			code = PAPER_CODE_LETTER_ROTATED;
			break;
		case PAPER_ID_A4:
		case PAPER_ID_A4_ALT:
			code = PAPER_CODE_A4_ROTATED;
			break;
		case PAPER_ID_EXECUTIVE:
			code = PAPER_CODE_EXECUTIVE_ROTATED;
			break;
		case PAPER_ID_JIS_B5:
			code = PAPER_CODE_JIS_B5_ROTATED;
			break;
		}
	}
	if (paper == PAPER_ID_CUSTOM) {
		int16_t height = state_word(printer, STATE_PAPER_HEIGHT_TENTHS_MM);
		int16_t width = state_word(printer, STATE_PAPER_WIDTH_TENTHS_MM);
		uint16_t first = (rotated ? width : height) * PAPER_SIZE_DPI /
				 TENTHS_MM_PER_INCH;
		uint16_t second = (rotated ? height : width) * PAPER_SIZE_DPI /
				  TENTHS_MM_PER_INCH;
		uint8_t command[2 + 2 * sizeof(uint16_t)] = {
			COMMAND_PAPER_SIZE, PAPER_CODE_CUSTOM,
		};

		put_u16le(command + 2, first);
		put_u16le(command + 2 + sizeof(uint16_t), second);
		write_command(command, sizeof(command));
	} else {
		/* Unknown IDs retain the legacy two-byte s ff fallback. */
		write_byte_parameter(COMMAND_PAPER_SIZE, code);
	}
}

void emit_printable_area(struct printer_state *printer)
{
	int16_t left = state_dword(printer, STATE_IMAGE_LEFT_BOUND);
	int16_t right = state_dword(printer, STATE_IMAGE_RIGHT_BOUND);
	int16_t top = state_dword(printer, STATE_IMAGE_TOP_BOUND);
	uint16_t bottom = state_dword(printer, STATE_IMAGE_BOTTOM_BOUND);
	uint8_t orientation = state_byte(printer, STATE_IMAGE_ORIENTATION);
	uint8_t command[1 + 4 * sizeof(uint16_t)] = { COMMAND_IMAGE_BOUNDS };

	if (orientation & ORIENTATION_MIRRORED) {
		int32_t start = state_dword(printer, STATE_IMAGE_START_X);
		int32_t end = state_dword(printer, STATE_IMAGE_END_X);
		int remainder = (end - start) % ROW_ALIGNMENT_PIXELS;
		int padding = remainder ? ROW_ALIGNMENT_PIXELS - remainder : 0;
		int16_t dpi = state_word(printer, STATE_RENDER_DPI);

		top -= padding * IMAGE_BOUNDS_DPI / dpi;
	}
	if (state_byte(printer, STATE_INTERLACE_FLAGS) & INTERLACE_SPECIAL_BOUNDS) {
		if (printer->borderless_expansion) {
			/* Place the enlarged image beyond the sheet, as in the Mac filter. */
			left -= BORDERLESS_OVERRUN_600DPI;
			right -= BORDERLESS_OVERRUN_600DPI;
			top -= BORDERLESS_OVERRUN_600DPI;
			bottom -= BORDERLESS_OVERRUN_600DPI;
		} else {
			left = SPECIAL_LEFT_BOUND;
			right = top = bottom = 0;
		}
	}
	if (orientation & ORIENTATION_REVERSED) {
		int16_t swap = left;

		left = right;
		right = swap;
	}
	put_u16le(command + 1, left);
	put_u16le(command + 1 + sizeof(uint16_t), right);
	put_u16le(command + 1 + 2 * sizeof(uint16_t), top);
	put_u16le(command + 1 + 3 * sizeof(uint16_t), bottom);
	write_command(command, sizeof(command));
}

void emit_paper_family_mode(struct printer_state *printer)
{
	uint16_t paper = state_word(printer, STATE_PAPER_ID);
	bool alternate_family = paper >= PAPER_ID_A3_FAMILY_1 &&
				paper <= PAPER_ID_TABLOID_FAMILY_2;

	write_byte_parameter(COMMAND_PAPER_FAMILY, alternate_family);
}

void emit_color_flag(struct printer_state *printer)
{
	bool color = state_word(printer, STATE_COLOR_SELECTION) == OUTPUT_COLOR;

	write_flag(FLAG_COLOR_OUTPUT, color);
}

void emit_single_pass_flag(struct printer_state *printer)
{
	write_flag(FLAG_SINGLE_PASS, paper_allows_single_pass(printer) != 0);
}

void emit_paper_control_flag(struct printer_state *printer)
{
	bool enabled = state_byte(printer, STATE_PAPER_CONTROL_FLAGS) &
		       PAPER_CONTROL_ENABLED;

	write_flag(FLAG_PAPER_CONTROL, enabled);
}

void emit_duplex_drying_mode(struct printer_state *printer)
{
	uint8_t drying = state_byte(printer, STATE_DUPLEX_DRYING);

	if (state_word(printer, STATE_DUPLEX_BINDING) == BINDING_SIMPLEX)
		return;
	if (drying != DRYING_SLOW && drying != DRYING_REDUCED_INK)
		drying = DRYING_NORMAL;
	write_subcommand(COMMAND_DUPLEX, DUPLEX_DRYING, drying);
}

void emit_duplex_side(struct printer_state *printer, uint32_t page_number)
{
	uint16_t binding = state_word(printer, STATE_DUPLEX_BINDING);
	uint8_t side = SIDE_UNSPECIFIED;

	if (binding != BINDING_DUPLEX_SHORT_EDGE &&
	    binding != BINDING_DUPLEX_LONG_EDGE)
		return;
	if (state_byte(printer, STATE_PAGE_SELECTION) == PAGE_SELECTION_FILTERED) {
		switch (state_byte(printer, STATE_SELECTED_PAGE_SIDE)) {
		case SIDE_ALTERNATE_3:
			side = SIDE_ALTERNATE_4;
			break;
		case SIDE_ALTERNATE_4:
			side = SIDE_ALTERNATE_3;
			break;
		}
		/* Preserve the duplicate command in these unvalidated side modes. */
		if (side != SIDE_UNSPECIFIED)
			write_subcommand(COMMAND_DUPLEX, DUPLEX_SIDE, side);
	} else {
		side = page_number & 1 ? SIDE_ODD_PAGE : SIDE_EVEN_PAGE;
	}
	write_subcommand(COMMAND_DUPLEX, DUPLEX_SIDE, side);
}

void emit_blank_rows(struct printer_state *printer, struct raster_pass *pass, uint16_t count)
{
	(void)printer;
	memset(pass->scratch_row, COMMAND_BLANK_ROW, count);
	write_command(pass->scratch_row, count);
}

void emit_ink_dot_counts(struct printer_state *printer, const struct output_swath *swath)
{
	/* Wire channel identities are deliberately not named after ink colors. */
	static const uint8_t channel_codes[COLOR_CHANNELS] = {
		COUNTER_CHANNEL_2, COUNTER_CHANNEL_3,
		COUNTER_CHANNEL_1, COUNTER_CHANNEL_0,
	};
	uint16_t mode = state_word(printer, STATE_RASTER_MODE);
	uint16_t depth = mode & RASTER_DEPTH_MASK;
	bool monochrome = (mode & RASTER_CHANNEL_MASK) == RASTER_MONOCHROME;
	unsigned int channels = monochrome ? 1 : COLOR_CHANNELS;
	unsigned int levels = COUNTERS_PER_CHANNEL;
	const uint8_t *counts = swath->numeric_state + SWATH_SYMBOL_COUNTS_OFFSET;
	unsigned int channel, level;

	if (depth == RASTER_BINARY_DOTS)
		levels = BINARY_DOT_LEVELS;
	else if (depth == RASTER_FOUR_LEVEL_DOTS)
		levels = FOUR_DOT_LEVELS;
	for (channel = 0; channel < channels; channel++) {
		for (level = 0; level < levels; level++) {
			uint8_t command[3 + sizeof(uint32_t)] = {
				COMMAND_SYMBOL_COUNT,
				monochrome ? COUNTER_CHANNEL_0 : channel_codes[channel],
				levels == BINARY_DOT_LEVELS && level ?
					BINARY_DOT_ON_CODE : level,
			};
			unsigned int index = channel * COUNTERS_PER_CHANNEL + level;
			uint32_t count;

			memcpy(&count, counts + index * sizeof(count), sizeof(count));
			put_u32le(command + 3, count);
			write_command(command, sizeof(command));
		}
	}
}

void emit_vertical_feed(struct printer_state *printer, uint32_t distance)
{
	(void)printer;
	while (distance >= MAX_FEED_DISTANCE) {
		write_word_parameter(COMMAND_VERTICAL_FEED, MAX_FEED_DISTANCE);
		distance -= MAX_FEED_DISTANCE;
	}
	/* Preserve the final command even when the remainder is zero. */
	write_word_parameter(COMMAND_VERTICAL_FEED, distance);
}

void begin_jpeg_job(struct printer_state *printer)
{
	(void)printer;
	emit_reset_padding();
	select_language(LANGUAGE_JPEG, RASTER_ROWS);
	initialize_printer();
}

void begin_jpeg_page(struct printer_state *printer, uint32_t page_number)
{
	select_language(LANGUAGE_JPEG, RASTER_ROWS);
	emit_jpeg_job_identifier();
	emit_raster_resolution(printer);
	emit_jpeg_resolution(printer);
	emit_jpeg_dimensions(printer);
	emit_jpeg_color_mode(printer);
	emit_bidirectional_mode(printer);
	emit_paper_thickness(printer);
	emit_media_type(printer);
	emit_paper_source(printer);
	emit_paper_size(printer);
	emit_printable_area(printer);
	emit_duplex_drying_mode(printer);
	emit_duplex_side(printer, page_number);
	emit_jpeg_color_matching(printer);
	emit_paper_family_mode(printer);
	initialize_page_origin();
}

void emit_jpeg_job_identifier(void)
{
	uint8_t command[2 + JOB_IDENTIFIER_MAX_BYTES + 1] = {
		COMMAND_JOB_IDENTIFIER, JOB_IDENTIFIER_TEXT,
	};
	size_t length = 0;

	if (lpr_jpeg_job_identifier) {
		length = strnlen(lpr_jpeg_job_identifier, JOB_IDENTIFIER_MAX_BYTES);
		memcpy(command + 2, lpr_jpeg_job_identifier, length);
	}
	/* Zero initialization supplies the NUL, including for an empty ID. */
	write_command(command, 2 + length + 1);
}

void emit_jpeg_resolution(struct printer_state *printer)
{
	uint16_t family = state_word(printer, STATE_RASTER_MODE) &
			  RASTER_RESOLUTION_FAMILY_MASK;
	uint8_t code = JPEG_RESOLUTION_CLASS_2;

	if (family == RASTER_LOW_RESOLUTION_FAMILY)
		code = JPEG_RESOLUTION_CLASS_0;
	else if (family == RASTER_MEDIUM_RESOLUTION_FAMILY)
		code = JPEG_RESOLUTION_CLASS_1;
	write_subcommand(COMMAND_JPEG_RESOLUTION, code, code);
}

void emit_jpeg_dimensions(struct printer_state *printer)
{
	uint16_t width = state_word(printer, STATE_IMAGE_END_X) -
			 state_word(printer, STATE_IMAGE_START_X);
	uint16_t height = state_word(printer, STATE_IMAGE_END_Y) -
			  state_word(printer, STATE_IMAGE_START_Y);
	uint8_t command[1 + 2 * sizeof(uint16_t)] = { COMMAND_JPEG_DIMENSIONS };

	put_u16le(command + 1, width);
	put_u16le(command + 1 + sizeof(uint16_t), height);
	write_command(command, sizeof(command));
}

void emit_jpeg_color_matching(struct printer_state *printer)
{
	uint8_t flags = 0;

	if (state_byte(printer, STATE_SLOW_DRYING) == SLOW_DRYING_ENABLED)
		flags |= JPEG_COLOR_SLOW_DRYING;
	switch (state_byte(printer, STATE_COLOR_MATCHING)) {
	case MATCHING_NONE:
		flags |= JPEG_COLOR_UNCORRECTED;
		break;
	case MATCHING_VIVID:
		flags |= JPEG_COLOR_VIVID;
		break;
	}
	write_byte_parameter(COMMAND_JPEG_COLOR_FLAGS, flags);
}

void emit_jpeg_color_mode(struct printer_state *printer)
{
	bool color = state_word(printer, STATE_COLOR_SELECTION) == OUTPUT_COLOR;

	write_byte_parameter(COMMAND_INK_MODE,
			     color ? INK_FOUR_LEVEL_COLOR : INK_FOUR_LEVEL_MONO);
}

void emit_feed_completion(void)
{
	static const uint8_t command = COMMAND_FEED_COMPLETION;

	lpr_transfer_end_callback(&command, sizeof(command),
				  FEED_COMPLETION_CALLBACK_TAG);
}

void initialize_feed_completion(struct printer_state *printer)
{
	bool available = feed_completion_available() != 0;

	store_byte(printer, STATE_FEED_COMPLETION_FLAGS, available);
	store_dword(printer, STATE_FEED_COMPLETION_POSITION, 0);
	store_dword(printer, STATE_FEED_COMPLETION_ENABLED, available);
}

uint32_t feed_completion_available(void)
{
	/* The direct CUPS path has no response callback for this feature. */
	return 0;
}
