/* Printer command serialization and model-specific protocol constants. */
#ifndef BROTHER_PROTOCOL_H
#define BROTHER_PROTOCOL_H

#include <stdint.h>

struct output_swath;
struct printer_state;
struct raster_pass;

/*
 * ESC @, M, G, Z and FF also appear in Brother's Raster Command Reference.
 * The MFC language selectors, compression values and extensions below are
 * model-specific. They must not be substituted with ESC/POS or P-touch codes.
 * See PROTOCOL.md for wire formats and the limits of known interpretations.
 */
enum protocol_command {
	CONTROL_NUL = 0x00,
	CONTROL_FORM_FEED = 0x0c,
	CONTROL_CARRIAGE_RETURN = 0x0d,
	CONTROL_ESCAPE = 0x1b,
	COMMAND_INITIALIZE = '@',
	COMMAND_LANGUAGE = '|',
	COMMAND_RESOLUTION = 'R',
	COMMAND_INK_MODE = 'I',
	COMMAND_COMPRESSION = 'M',
	COMMAND_BIDIRECTIONAL = 'D',
	COMMAND_MEDIA = 'N',
	COMMAND_NOZZLES = 'P',
	COMMAND_PAPER_SOURCE = 'T',
	COMMAND_PAPER_SIZE = 's',
	COMMAND_IMAGE_BOUNDS = 'S',
	COMMAND_PAPER_FAMILY = 'y',
	COMMAND_Y_ORIGIN = 'Y',
	COMMAND_X_ORIGIN = 'X',
	COMMAND_CHECKSUMMED_FLAG = 'U',
	COMMAND_DUPLEX = '2',
	COMMAND_JOB_IDENTIFIER = '4',
	COMMAND_SYMBOL_COUNT = 'g',
	COMMAND_VERTICAL_FEED = 'F',
	COMMAND_RASTER_DATA = 'G',
	COMMAND_RANGE_START = 'b',
	COMMAND_RANGE_END = 'j',
	COMMAND_RANGE_CHANNEL = 'e',
	COMMAND_BLANK_ROW = 'Z',
	COMMAND_FEED_COMPLETION = 't',
	COMMAND_JPEG_RESOLUTION = 'q',
	COMMAND_JPEG_DIMENSIONS = 'p',
	COMMAND_JPEG_COLOR_FLAGS = 'h',
};

enum protocol_parameter {
	LANGUAGE_RASTER = 1,
	LANGUAGE_JPEG = 2,
	RASTER_ROWS = 0,
	RASTER_RANGE_SWATHS = 1,
	COMPRESSION_WORD_RUNS = 0x06,
	COMPRESSION_PACKBITS = 0x07,
	COMPRESSION_ZERO_RUNS = 0x08,
	COMPRESSION_RANGE_CODER = 0x0c,
	BIDIRECTIONAL_ALLOWED = 0x00,
	BIDIRECTIONAL_DISABLED = 0xff,
	MEDIA_THICKNESS = 0x04,
	MEDIA_TYPE = 0x05,
	THICKNESS_REGULAR = 0x00,
	THICKNESS_THICK = 0x02,
	THICKNESS_ENVELOPE = 0x04,
	NOZZLE_RANGE = 0x90,
	SOURCE_TRAY_1 = 0x01,
	SOURCE_TRAY_2 = 0x02,
	SOURCE_AUTOMATIC = 0x03,
	SOURCE_MULTIPURPOSE = 0x06,
	SOURCE_MANUAL = 0xff,
	FLAG_COLOR_OUTPUT = 'p',
	FLAG_SINGLE_PASS = 'q',
	FLAG_PAPER_CONTROL = 'r',
	FLAG_PAYLOAD_BYTES = 2,
	DUPLEX_SIDE = 'R',
	DUPLEX_DRYING = 'S',
	SIDE_ODD_PAGE = 1,
	SIDE_EVEN_PAGE = 2,
	SIDE_ALTERNATE_3 = 3,
	SIDE_ALTERNATE_4 = 4,
	SIDE_UNSPECIFIED = 0xff,
	DRYING_NORMAL = 0,
	DRYING_SLOW = 1,
	DRYING_REDUCED_INK = 2,
	JOB_IDENTIFIER_TEXT = 'Y',
	JPEG_END_PAGE_HIGH_BYTE = 0xff,
	INK_BINARY_MONO = 0x00,
	INK_BINARY_COLOR = 0x10,
	INK_FOUR_LEVEL_MONO = 0x30,
	INK_FOUR_LEVEL_COLOR = 0x31,
	INK_FIVE_LEVEL_MONO = 0x40,
	INK_FIVE_LEVEL_COLOR = 0x41,
	JPEG_COLOR_SLOW_DRYING = 0x01,
	JPEG_COLOR_VIVID = 0x02,
	JPEG_COLOR_UNCORRECTED = 0x04,
	JPEG_RESOLUTION_CLASS_0 = 0,
	JPEG_RESOLUTION_CLASS_1 = 1,
	JPEG_RESOLUTION_CLASS_2 = 2,
	COUNTER_CHANNEL_0 = 0,
	COUNTER_CHANNEL_1 = 1,
	COUNTER_CHANNEL_2 = 2,
	COUNTER_CHANNEL_3 = 3,
	BINARY_DOT_ON_CODE = 3,
};

enum protocol_limits {
	INITIALIZATION_PADDING_BYTES = 4608,
	INITIALIZATION_BLOCK_BYTES = 256,
	JOB_IDENTIFIER_MAX_BYTES = 20,
	MAX_FEED_DISTANCE = 32767,
	IMAGE_BOUNDS_DPI = 600,
	PAPER_SIZE_DPI = 300,
	TENTHS_MM_PER_INCH = 254,
	ROW_ALIGNMENT_PIXELS = 8,
	SPECIAL_LEFT_BOUND = -5,
	BORDERLESS_OVERRUN_600DPI = 106,
	COLOR_CHANNELS = 4,
	BINARY_DOT_LEVELS = 2,
	FOUR_DOT_LEVELS = 4,
	COUNTERS_PER_CHANNEL = 5,
	SWATH_SYMBOL_COUNTS_OFFSET = 0x18,
	/* Legacy callback argument, not a byte in the printer stream. */
	FEED_COMPLETION_CALLBACK_TAG = 0x7b,
};

/* Offsets in printer_state.numeric_state, not printer command addresses. */
enum protocol_state_offset {
	STATE_PAPER_HEIGHT_TENTHS_MM = 0x0004,
	STATE_PAPER_WIDTH_TENTHS_MM = 0x0006,
	STATE_PAPER_SOURCE = 0x0008,
	STATE_COLOR_SELECTION = 0x000c,
	STATE_DUPLEX_BINDING = 0x000e,
	STATE_MEDIA_SELECTION = 0x0010,
	STATE_THICKNESS = 0x00d0,
	STATE_SLOW_DRYING = 0x00e6,
	STATE_BIDIRECTIONAL_FLAGS = 0x0109,
	STATE_COLOR_MATCHING = 0x021f,
	STATE_DUPLEX_DRYING = 0x05ad,
	STATE_PAPER_CONTROL_FLAGS = 0x0818,
	STATE_PAPER_ID = 0x0848,
	STATE_PAGE_SELECTION = 0x0860,
	STATE_PAPER_ROTATED = 0x0878,
	STATE_INTERLACE_FLAGS = 0x0888,
	STATE_RASTER_MODE = 0x088a,
	STATE_FEED_COMPLETION_ENABLED = 0x088c,
	STATE_IMAGE_ORIENTATION = 0x089c,
	STATE_CURRENT_INK_MODE = 0x1176,
	STATE_RENDER_DPI = 0x11b0,
	STATE_FEED_COMPLETION_FLAGS = 0x11e8,
	STATE_FEED_COMPLETION_POSITION = 0x11ec,
	STATE_IMAGE_START_X = 0x11fc,
	STATE_IMAGE_START_Y = 0x1200,
	STATE_IMAGE_END_X = 0x1204,
	STATE_IMAGE_END_Y = 0x1208,
	STATE_IMAGE_TOP_BOUND = 0x120c,
	STATE_IMAGE_LEFT_BOUND = 0x1210,
	STATE_IMAGE_BOTTOM_BOUND = 0x1214,
	STATE_IMAGE_RIGHT_BOUND = 0x1218,
	STATE_IMAGE_ENCODING = 0x129c,
	STATE_PAGE_END_ENCODING = 0x129d,
	STATE_ROW_COMPRESSION = 0x813c2,
	STATE_SELECTED_PAGE_SIDE = 0x817e8,
};

/* Internal renderer selections, distinct from the wire values above. */
enum renderer_selection {
	OUTPUT_COLOR = 2,
	BINDING_SIMPLEX = 1,
	BINDING_DUPLEX_SHORT_EDGE = 2,
	BINDING_DUPLEX_LONG_EDGE = 3,
	PAGE_SELECTION_FILTERED = 1,
	THICKNESS_SETTING_THICK = 1,
	THICKNESS_SETTING_ENVELOPE = 2,
	SOURCE_SETTING_TRAY_1 = 1,
	SOURCE_SETTING_TRAY_2 = 2,
	SOURCE_SETTING_MULTIPURPOSE = 3,
	SOURCE_SETTING_MANUAL = 4,
	MATCHING_NONE = 0,
	MATCHING_VIVID = 4,
	SLOW_DRYING_ENABLED = 1,
	ENCODING_RASTER = 0,
	ENCODING_JPEG = 1,
	ENCODING_DEFERRED_HEADER = 3,
	ROW_CODEC_ZERO_RUNS = 2,
	ROW_CODEC_WORD_RUNS = 3,
	ROW_CODEC_RANGE_CODER = 5,
	BIDIRECTIONAL_PLAIN_MEDIA = 0x01,
	BIDIRECTIONAL_PHOTO_MEDIA = 0x02,
	PAPER_CONTROL_ENABLED = 0x10,
	INTERLACE_ALTERNATE_RESOLUTION = 0x40,
	INTERLACE_SPECIAL_BOUNDS = 0x80,
	ORIENTATION_MIRRORED = 0x01,
	ORIENTATION_REVERSED = 0x02,
	RASTER_PROFILE_MASK = 0x0fff,
	RASTER_RESOLUTION_FAMILY_MASK = 0x0f00,
	RASTER_LOW_RESOLUTION_FAMILY = 0x0100,
	RASTER_MEDIUM_RESOLUTION_FAMILY = 0x0200,
	RASTER_CHANNEL_MASK = 0x3000,
	RASTER_MONOCHROME = 0x1000,
	RASTER_DEPTH_MASK = 0xc000,
	RASTER_BINARY_DOTS = 0x0000,
	RASTER_FOUR_LEVEL_DOTS = 0x4000,
	RASTER_FIVE_LEVEL_DOTS = 0x8000,
};

/* The full DPI/media meaning of these renderer profile IDs is unresolved. */
enum raster_profile {
	RASTER_PROFILE_0100 = 0x100,
	RASTER_PROFILE_0102 = 0x102,
	RASTER_PROFILE_0103 = 0x103,
	RASTER_PROFILE_0106 = 0x106,
	RASTER_PROFILE_0107 = 0x107,
	RASTER_PROFILE_010E = 0x10e,
	RASTER_PROFILE_010F = 0x10f,
	RASTER_PROFILE_0200 = 0x200,
	RASTER_PROFILE_0201 = 0x201,
	RASTER_PROFILE_0205 = 0x205,
	RASTER_PROFILE_0210 = 0x210,
	RASTER_PROFILE_0211 = 0x211,
	RASTER_PROFILE_0215 = 0x215,
	RASTER_PROFILE_0500 = 0x500,
	RASTER_PROFILE_0508 = 0x508,
	RASTER_PROFILE_0510 = 0x510,
	RASTER_PROFILE_0518 = 0x518,
	RASTER_PROFILE_0600 = 0x600,
	RASTER_PROFILE_0608 = 0x608,
	RASTER_PROFILE_0610 = 0x610,
	RASTER_PROFILE_0618 = 0x618,
	RASTER_PROFILE_0700 = 0x700,
	RASTER_PROFILE_0710 = 0x710,
};

/* R's two opaque class bytes, packed low byte first for the lookup table. */
enum resolution_code {
	RESOLUTION_CODE_00_00 = 0x0000,
	RESOLUTION_CODE_11_00 = 0x0011,
	RESOLUTION_CODE_02_00 = 0x0002,
	RESOLUTION_CODE_11_01 = 0x0111,
	RESOLUTION_CODE_01_01 = 0x0101,
	RESOLUTION_CODE_02_01 = 0x0102,
	RESOLUTION_CODE_02_02 = 0x0202,
	RESOLUTION_CODE_03_03 = 0x0303,
	RESOLUTION_CODE_02_03 = 0x0302,
	RESOLUTION_CODE_02_04 = 0x0402,
	RESOLUTION_CODE_03_04 = 0x0403,
	RESOLUTION_CODE_03_06 = 0x0603,
};

/* Additional media variants have no verified public option name. */
enum media_selection {
	MEDIA_PLAIN = 0x0b,
	MEDIA_INKJET = 0x0f,
	MEDIA_OTHER_PHOTO = 0x11,
	MEDIA_VARIANT_17 = 0x17,
	MEDIA_BROTHER_BP71 = 0x19,
	MEDIA_HAGAKI_ADDRESS = 0x1a,
	MEDIA_HAGAKI_REVERSE = 0x1b,
	MEDIA_VARIANT_1C = 0x1c,
	MEDIA_VARIANT_1D = 0x1d,
	MEDIA_BROTHER_BP60 = 0x1e,
	MEDIA_VARIANT_28 = 0x28,
	MEDIA_CODE_PLAIN = 0x00,
	MEDIA_CODE_INKJET = 0x01,
	MEDIA_CODE_OTHER_PHOTO = 0x06,
	MEDIA_CODE_BROTHER_BP71 = 0x07,
	MEDIA_CODE_HAGAKI_ADDRESS = 0x0c,
	MEDIA_CODE_HAGAKI_REVERSE = 0x08,
	MEDIA_CODE_VARIANT_17 = 0x05,
	MEDIA_CODE_VARIANT_1C = 0x0d,
	MEDIA_CODE_VARIANT_1D = 0x0e,
	MEDIA_CODE_VARIANT_28 = 0x04,
};

enum paper_identifier {
	PAPER_ID_LETTER = 0x001,
	PAPER_ID_TABLOID = 0x003,
	PAPER_ID_LEGAL = 0x005,
	PAPER_ID_EXECUTIVE = 0x007,
	PAPER_ID_A3 = 0x008,
	PAPER_ID_A4 = 0x009,
	PAPER_ID_A5 = 0x00b,
	PAPER_ID_JIS_B4 = 0x00c,
	PAPER_ID_JIS_B5 = 0x00d,
	PAPER_ID_FOLIO = 0x00e,
	PAPER_ID_ENVELOPE_10 = 0x014,
	PAPER_ID_ENVELOPE_DL = 0x01b,
	PAPER_ID_ENVELOPE_C5 = 0x01c,
	PAPER_ID_ENVELOPE_C4 = 0x01e,
	PAPER_ID_ISO_B5 = 0x022,
	PAPER_ID_ENVELOPE_MONARCH = 0x025,
	PAPER_ID_HAGAKI = 0x02b,
	PAPER_ID_A6 = 0x046,
	PAPER_ID_ENVELOPE_KAKU2 = 0x047,
	PAPER_ID_ENVELOPE_CHOU3 = 0x049,
	PAPER_ID_ENVELOPE_CHOU4 = 0x04a,
	PAPER_ID_DOUBLE_HAGAKI = 0x052,
	PAPER_ID_JIS_B6 = 0x058,
	PAPER_ID_ENVELOPE_YOU4 = 0x05b,
	PAPER_ID_CUSTOM = 0x100,
	PAPER_ID_PHOTO_4X6 = 0x109,
	PAPER_ID_PHOTO_5X8 = 0x10a,
	PAPER_ID_PHOTO_L = 0x110,
	PAPER_ID_PHOTO_2L = 0x111,
	PAPER_ID_A4_ALT = 0x112,
	PAPER_ID_LETTER_ALT = 0x113,
	PAPER_ID_A6_ALT = 0x114,
	PAPER_ID_HAGAKI_ALT = 0x115,
	PAPER_ID_PHOTO_L_ALT = 0x116,
	PAPER_ID_PHOTO_2L_ALT = 0x117,
	PAPER_ID_PHOTO_4X6_ALT = 0x118,
	PAPER_ID_PHOTO_5X8_ALT = 0x119,
	PAPER_ID_A3_BORDERLESS = 0x11b,
	PAPER_ID_JIS_B4_BORDERLESS = 0x11c,
	PAPER_ID_TABLOID_BORDERLESS = 0x11d,
	PAPER_ID_A3_FAMILY_1 = 0x11f,
	PAPER_ID_JIS_B4_FAMILY_1 = 0x120,
	PAPER_ID_TABLOID_FAMILY_1 = 0x121,
	PAPER_ID_A3_FAMILY_2 = 0x122,
	PAPER_ID_JIS_B4_FAMILY_2 = 0x123,
	PAPER_ID_TABLOID_FAMILY_2 = 0x124,
	PAPER_ID_VARIANT_132 = 0x132,
	PAPER_ID_VARIANT_133 = 0x133,
	PAPER_ID_DISC_12CM = 0x134,
};

enum paper_code {
	PAPER_CODE_LETTER = 0x00,
	PAPER_CODE_A4 = 0x01,
	PAPER_CODE_LEGAL = 0x02,
	PAPER_CODE_EXECUTIVE = 0x03,
	PAPER_CODE_JIS_B5 = 0x04,
	PAPER_CODE_ISO_B5 = 0x05,
	PAPER_CODE_ENVELOPE_C5 = 0x06,
	PAPER_CODE_A5 = 0x07,
	PAPER_CODE_JIS_B6 = 0x08,
	PAPER_CODE_ENVELOPE_DL = 0x0a,
	PAPER_CODE_A6 = 0x0b,
	PAPER_CODE_ENVELOPE_10 = 0x0c,
	PAPER_CODE_ENVELOPE_MONARCH = 0x0d,
	PAPER_CODE_HAGAKI = 0x11,
	PAPER_CODE_ENVELOPE_C4 = 0x12,
	PAPER_CODE_PHOTO_5X8 = 0x13,
	PAPER_CODE_PHOTO_4X6 = 0x14,
	PAPER_CODE_ENVELOPE_YOU4 = 0x15,
	PAPER_CODE_PHOTO_L = 0x18,
	PAPER_CODE_PHOTO_2L = 0x19,
	PAPER_CODE_DOUBLE_HAGAKI = 0x1a,
	PAPER_CODE_TABLOID = 0x1d,
	PAPER_CODE_A3 = 0x1e,
	PAPER_CODE_JIS_B4 = 0x1f,
	PAPER_CODE_FOLIO = 0x21,
	PAPER_CODE_DISC_12CM = 0x24,
	PAPER_CODE_VARIANT_133 = 0x25,
	PAPER_CODE_ENVELOPE_CHOU3 = 0x26,
	PAPER_CODE_ENVELOPE_CHOU4 = 0x27,
	PAPER_CODE_ENVELOPE_KAKU2 = 0x28,
	PAPER_CODE_VARIANT_132 = 0x29,
	PAPER_CODE_A4_ROTATED = 0x2a,
	PAPER_CODE_LETTER_ROTATED = 0x2b,
	PAPER_CODE_EXECUTIVE_ROTATED = 0x2c,
	PAPER_CODE_JIS_B5_ROTATED = 0x2d,
	PAPER_CODE_CUSTOM = 0xff,
};

/* Serialize Brother job, page and raster commands. */
void emit_duplex_side(struct printer_state *printer, uint32_t page_number);
void emit_paper_family_mode(struct printer_state *printer);
void emit_blank_rows(struct printer_state *printer, struct raster_pass *pass, uint16_t count);
void emit_feed_completion(void);
uint32_t begin_protocol_job(struct printer_state *printer);
uint32_t end_protocol_job(struct printer_state *printer);
uint32_t begin_protocol_page(struct printer_state *printer, uint32_t page_number);
uint32_t end_protocol_page(struct printer_state *printer);
void emit_reset_padding(void);
void begin_raster_job(struct printer_state *printer);
void begin_raster_page(struct printer_state *printer, uint32_t page_number);
void emit_jpeg_job_identifier(void);
void emit_raster_resolution(struct printer_state *printer);
void select_raster_color_mode(struct printer_state *printer);
void emit_raster_color_mode(struct printer_state *printer);
void emit_compression_mode(struct printer_state *printer);
void emit_bidirectional_mode(struct printer_state *printer);
void emit_paper_thickness(struct printer_state *printer);
void emit_media_type(struct printer_state *printer);
void emit_nozzle_range(struct printer_state *printer);
void emit_paper_source(struct printer_state *printer);
void emit_paper_size(struct printer_state *printer);
void emit_printable_area(struct printer_state *printer);
void emit_color_flag(struct printer_state *printer);
void emit_single_pass_flag(struct printer_state *printer);
void emit_paper_control_flag(struct printer_state *printer);
void emit_duplex_drying_mode(struct printer_state *printer);
void emit_ink_dot_counts(struct printer_state *printer, const struct output_swath *swath);
void emit_vertical_feed(struct printer_state *printer, uint32_t distance);
void begin_jpeg_job(struct printer_state *printer);
void begin_jpeg_page(struct printer_state *printer, uint32_t page_number);
void emit_jpeg_resolution(struct printer_state *printer);
void emit_jpeg_dimensions(struct printer_state *printer);
void emit_jpeg_color_matching(struct printer_state *printer);
void emit_jpeg_color_mode(struct printer_state *printer);
void initialize_feed_completion(struct printer_state *printer);
uint32_t feed_completion_available(void);

#endif
