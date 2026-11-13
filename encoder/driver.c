/* Translate job choices into renderer state and manage page/band submission. */
#include "driver.h"
#include "config.h"
#include "data.h"
#include "enhancement.h"
#include "geometry.h"
#include "jpeg.h"
#include "memory.h"
#include "output.h"
#include "paper.h"
#include "protocol.h"
#include "raster.h"
#include "settings.h"
#include "state.h"
#include "swath.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Indices in the retained 27-word device option record. */
enum device_option_index {
	DEVICE_BOTTOM_MARGIN = 0,
	DEVICE_LEFT_MARGIN = 1,
	DEVICE_HEIGHT_120DPI = 2,
	DEVICE_WIDTH_120DPI = 3,
	DEVICE_HORIZONTAL_DPI = 4,
	DEVICE_VERTICAL_DPI = 5,
	DEVICE_PRINTABLE_WIDTH = 6,
	DEVICE_PRINTABLE_HEIGHT = 7,
	DEVICE_PAPER_ID = 8,
	DEVICE_BRIGHTNESS_CONTRAST = 9,
	DEVICE_RED_GREEN = 10,
	DEVICE_BLUE_LEVELS = 11,
	DEVICE_QUALITY = 12,
	DEVICE_RENDER_DPI = 13,
	DEVICE_MEDIA = 15,
	DEVICE_PAPER_FLAGS = 18,
	DEVICE_RENDER_FLAGS = 19,
	DEVICE_RESERVED_CONTROL = 20,
	DEVICE_CHANNEL_LEVELS = 21,
	DEVICE_DENSITY = 25,
	DEVICE_INTERFACE = 26,
};

/* Additional numeric-state fields used by job setup. */
enum driver_state_offset {
	STATE_JOB_ACTIVE = 0x0002,
	STATE_REQUESTED_DPI = 0x000a,
	STATE_PAPER_ID_DWORD = 0x0014,
	STATE_QUALITY = 0x00f8,
	STATE_ENHANCEMENT_ENABLED = 0x0125,
	STATE_BRIGHTNESS = 0x020e,
	STATE_CONTRAST = 0x0210,
	STATE_RED = 0x0212,
	STATE_GREEN = 0x0214,
	STATE_BLUE = 0x0216,
	STATE_DITHER_ENABLED = 0x021e,
	STATE_PAGE_FILTER_ENABLED = 0x0220,
	STATE_JPEG_PRIORITY = 0x074c,
	STATE_REQUESTED_COMPRESSION = 0x0770,
	STATE_MULTILEVEL_ENABLED = 0x0771,
	STATE_ALIGNMENT_FLAGS = 0x081a,
	STATE_INTERFACE_FLAGS = 0x0834,
	STATE_REQUESTED_MEDIA = 0x0858,
	STATE_REQUESTED_DRYING = 0x085c,
	STATE_ENVELOPE_ROTATED = 0x087c,
	STATE_BORDERLESS = 0x0889,
	STATE_CALIBRATION_MODE = 0x08a4,
	STATE_HALFTONE_DITHER = 0x08e6,
	STATE_ENHANCEMENT_ACTIVE = 0x1178,
	STATE_FIRST_PAGE = 0x817e4,
	STATE_JOB_FLAGS = 0x817e6,
	STATE_PAGE_NUMBER = 0x817ec,
	STATE_LEFT_ORIGIN = 0x817f0,
	STATE_BOTTOM_ORIGIN = 0x817fc,
	STATE_PRINTABLE_WIDTH = 0x81800,
	STATE_PRINTABLE_HEIGHT = 0x81804,
	STATE_PAPER_WIDTH_600DPI = 0x81808,
	STATE_PAPER_HEIGHT_600DPI = 0x8180c,
	STATE_DUPLEX_CONTROL = 0x81810,
};

/* Configuration selections, before conversion to protocol.h renderer IDs. */
enum job_selection {
	JOB_MEDIA_PLAIN = 0,
	JOB_MEDIA_INKJET = 1,
	JOB_MEDIA_PHOTO = 2,
	JOB_MEDIA_VARIANT_3 = 3,
	JOB_MEDIA_VARIANT_5 = 5,
	JOB_MEDIA_HAGAKI_ADDRESS = 6,
	JOB_MEDIA_HAGAKI_REVERSE = 7,
	JOB_MEDIA_BP71 = 8,
	JOB_MEDIA_PLAIN_DUPLEX = 9,
	JOB_MEDIA_DISC = 10,
	JOB_MEDIA_DISC_ALTERNATE = 11,
	JOB_MEDIA_BP60 = 12,
	QUALITY_FAST = 1,
	QUALITY_NORMAL = 2,
	QUALITY_PHOTO = 3,
	QUALITY_BEST = 4,
	PAGE_MONOCHROME = 0,
	PAGE_COLOR = 1,
	PAGE_SIMPLEX = 0,
	PAGE_DUPLEX_LONG_EDGE = 1,
	PAGE_DUPLEX_SHORT_EDGE = 2,
	PAGE_SET_ALL = 0,
	PAGE_SET_EVEN = 3,
	PAGE_SET_ODD = 4,
	JPEG_RECOMMENDED = 0,
	JPEG_QUALITY_PRIORITY = 1,
	JPEG_SPEED_PRIORITY = 2,
};

enum driver_flags {
	RENDER_DEFAULT_FLAGS = 0x000c,
	RENDER_BIDIRECTIONAL = 0x0010,
	RENDER_MIRRORED = 0x0040,
	RENDER_VIVID = 0x0080,
	RENDER_NO_COLOR_MATCHING = 0x0100,
	RENDER_DIFFUSION = 0x0200,
	RENDER_ENHANCEMENT = 0x0400,
	RENDER_SLOW_DRYING = 0x4000,
	RENDER_REVERSED = 0x8000,
	PAPER_DEFAULT_FLAGS = 0x0039,
	PAPER_BIDIRECTIONAL_MASK = 0x0038,
	PAPER_BORDERLESS_FLAG = 0x0040,
	ALIGN_OTHER = 0x01,
	ALIGN_A4_LETTER = 0x02,
	ALIGN_A3_B4_LEDGER = 0x04,
	ALIGN_DEDICATED = 0x10,
	INTERFACE_WIRELESS = 0x01,
	FEED_BUFFERED = 0x01,
};

enum driver_constants {
	POINTS_PER_INCH = 72,
	PAGE_GEOMETRY_DPI = 120,
	FAST_RENDER_DPI = 150,
	NORMAL_RENDER_DPI = 600,
	SOURCE_SETTING_AUTOMATIC = 7,
	DEFAULT_JOB_FLAGS = 0x11,
	DEFAULT_CHANNEL_LEVELS = 0x0202,
	DEFAULT_BLUE_LEVELS = 0x0200,
	DEFAULT_DENSITY = 100,
	DEFAULT_DUPLEX_CONTROL = 1,
	UNKNOWN_FIRST_PAGE = -1,
	IPP_HIGH_QUALITY = 5,
	BORDERLESS_DEFAULT_MARGIN = 6,
	SMALL_MARGIN_LIMIT = 19,
	SMALL_MARGIN_FALLBACK = 11,
	QUALITY_CLASSES = 4,
	PAPER_ID_CUSTOM_FIRST = 400,
	PAPER_ID_CUSTOM_COUNT = 16,
	MATCHING_NATURAL = 3,
	COMPRESSION_AUTOMATIC = 0xff,
	RANGE_COMPATIBILITY_MASK = 0xcf00007f,
	RANGE_COMPATIBLE_MODE = 0x82000000,
	JPEG_ELIGIBLE = 1,
	JPEG_RECOMMENDED_BIT = 2,
};

/* Preserve native byte widths without unaligned pointer casts. */
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

static bool text_is(const char *text, const char *choice)
{
	return text && !strcmp(text, choice);
}

static bool option_is(const char *name, const char *choice)
{
	return text_is(current_option_text(name), choice);
}

/* Adapt the output helpers to the retained callbacks' int return convention. */
static int write_job_bytes(const void *bytes, size_t length)
{
	return write_printer_bytes(bytes, length);
}

static int finish_job_transfer(const void *bytes, size_t length, unsigned int tag)
{
	(void)bytes;
	(void)length;
	(void)tag;
	return write_transfer_terminator();
}

uint32_t start_printer_job(struct job_settings *settings, struct page_state *page)
{
	struct printer_state *printer = page->printer;

	store_word(printer, STATE_JOB_FLAGS, DEFAULT_JOB_FLAGS);
	lpr_write_callback = write_job_bytes;
	lpr_transfer_end_callback = finish_job_transfer;
	store_dword(printer, STATE_PAPER_ROTATED, needs_landscape_rotation(settings));
	store_dword(printer, STATE_ENVELOPE_ROTATED, needs_envelope_rotation(settings));
	configure_printer_options(settings, page);
	configure_raster_geometry(page);
	initialize_feed_completion(printer);
	begin_protocol_job(printer);
	choose_compression_mode(printer);
	return allocate_raster_buffers(page);
}

uint32_t end_printer_job(void)
{
	end_protocol_job(lpr_printer_state);
	return 0;
}

uint32_t start_printer_page(struct job_settings *settings, struct page_state *page,
			    uint32_t page_number)
{
	struct printer_state *printer = page->printer;
	uint32_t rotated = state_dword(printer, STATE_PAPER_ROTATED);

	(void)settings;
	store_dword(printer, STATE_PAGE_NUMBER, page_number);
	if (!should_process_page(page))
		return 0;
	/* The rotation and binding combination decides which sheets turn over. */
	if ((!rotated && page->duplex == PAGE_DUPLEX_LONG_EDGE) ||
	    (rotated == 1 && page->duplex == PAGE_DUPLEX_SHORT_EDGE))
		store_byte(printer, STATE_IMAGE_ORIENTATION,
			   state_byte(printer, STATE_IMAGE_ORIENTATION) ^
			   (ORIENTATION_MIRRORED | ORIENTATION_REVERSED));
	prepare_band_descriptor(page);
	begin_protocol_page(printer, page_number);
	return 0;
}

uint32_t end_printer_page(struct page_state *page)
{
	struct printer_state *printer = page->printer;

	if (should_process_page(page)) {
		end_protocol_page(printer);
		if (!(state_byte(printer, STATE_FEED_COMPLETION_FLAGS) & FEED_BUFFERED))
			fflush(stdout);
	}
	return 0;
}

uint32_t send_printer_band(struct job_settings *settings, struct page_state *page)
{
	struct raster_band *band = page->band;
	struct printer_state *printer = page->printer;
	uint32_t status = 0;

	if (!should_process_page(page))
		return 0;
	clear_paper_margins(settings, page);
	rotate_landscape_band(settings, page);
	rotate_envelope_band(settings, page);
	if (!allocate_band_workspace(page, (uint8_t *)lpr_input_band)) {
		fputs("ERROR: Unable to allocate band workspace.\n", stderr);
		return 1;
	}
	prepare_band_descriptor(page);
	if (state_byte(printer, STATE_ENHANCEMENT_ENABLED) == 1) {
		initialize_color_enhancement(printer);
		store_dword(printer, STATE_ENHANCEMENT_ACTIVE, 1);
		collect_band_histogram(printer, 0, band->width, band, band->height, 0, band->width);
	} else {
		store_dword(printer, STATE_ENHANCEMENT_ACTIVE, 0);
	}
	if (state_dword(printer, STATE_ENHANCEMENT_ACTIVE) == 1) {
		build_enhancement_curves(printer);
		enhance_row_region(printer, 0, band->width, band, band->height, 0, band->width);
	}
	if (state_byte(printer, STATE_IMAGE_ENCODING) == ENCODING_JPEG)
		status = render_jpeg_band(printer, band);
	else
		render_halftone_band(printer, band);
	free_band_workspace();
	return status;
}

static uint16_t select_quality(int16_t media, const char *choice, uint16_t *dpi)
{
	*dpi = NORMAL_RENDER_DPI;
	switch (media) {
	case JOB_MEDIA_PLAIN:
	case JOB_MEDIA_HAGAKI_ADDRESS:
	case JOB_MEDIA_PLAIN_DUPLEX:
		if (text_is(choice, "PlainFast")) {
			*dpi = FAST_RENDER_DPI;
			return QUALITY_FAST;
		}
		if (text_is(choice, "PlainNormal"))
			return QUALITY_NORMAL;
		break;
	case JOB_MEDIA_INKJET:
	case JOB_MEDIA_BP60:
		if (text_is(choice, "Normal"))
			return QUALITY_NORMAL;
		break;
	case JOB_MEDIA_PHOTO:
	case JOB_MEDIA_VARIANT_5:
	case JOB_MEDIA_BP71:
	case JOB_MEDIA_HAGAKI_REVERSE:
		if (text_is(choice, "Fast"))
			return QUALITY_NORMAL;
		break;
	case JOB_MEDIA_VARIANT_3:
	case JOB_MEDIA_DISC_ALTERNATE:
		return QUALITY_BEST;
	case JOB_MEDIA_DISC:
		return text_is(choice, "DiscSlow") ? QUALITY_PHOTO : QUALITY_BEST;
	default:
		return QUALITY_PHOTO;
	}
	return text_is(choice, "Best") ? QUALITY_BEST : QUALITY_PHOTO;
}

static void configure_page_geometry(const struct job_settings *settings,
				    struct page_state *page)
{
	uint16_t *options = page->device_options;

	options[DEVICE_BOTTOM_MARGIN] = settings->bottom_margin;
	options[DEVICE_LEFT_MARGIN] = settings->left_margin;
	options[DEVICE_WIDTH_120DPI] = settings->width_points * PAGE_GEOMETRY_DPI / POINTS_PER_INCH;
	options[DEVICE_HEIGHT_120DPI] = settings->height_points * PAGE_GEOMETRY_DPI / POINTS_PER_INCH;
	page->paper_width_120dpi = options[DEVICE_WIDTH_120DPI];
	page->paper_height_120dpi = options[DEVICE_HEIGHT_120DPI];
	options[DEVICE_PRINTABLE_HEIGHT] = settings->image_bottom - settings->bottom_margin;
	options[DEVICE_PRINTABLE_WIDTH] = settings->image_right - settings->left_margin;
	options[DEVICE_HORIZONTAL_DPI] = POINTS_PER_INCH;
	options[DEVICE_VERTICAL_DPI] = POINTS_PER_INCH;
	memset(page->reserved_margins, 0, sizeof(page->reserved_margins));
	page->width_pixels = settings->width_pixels;
	page->raster_width = settings->width_pixels;
	page->row_bytes = settings->row_bytes;
	/* The direct filter supplies a complete page as a single band. */
	page->band_height = settings->height_pixels;
	page->band_count = 1;
	page->last_band_height = page->band_height;
	page->rotated_row_bytes = settings->height_pixels *
		((uint32_t)settings->pixel_bytes / (uint32_t)settings->sample_bytes);
}

static void configure_duplex(struct page_state *page)
{
	if (option_is("BRDuplex", "DuplexTumble"))
		page->duplex = PAGE_DUPLEX_SHORT_EDGE;
	else if (option_is("BRDuplex", "DuplexNoTumble"))
		page->duplex = PAGE_DUPLEX_LONG_EDGE;
	else
		page->duplex = PAGE_SIMPLEX;

	page->duplex_mode = DRYING_NORMAL;
	if (page->duplex != PAGE_SIMPLEX) {
		if (option_is("BRDuplexMode", "Duplex1"))
			page->duplex_mode = DRYING_SLOW;
		else if (option_is("BRDuplexMode", "Duplex2"))
			page->duplex_mode = DRYING_REDUCED_INK;
		/* The duplex path leaves the unused page-set field intact. */
		return;
	}
	page->page_set = PAGE_SET_ALL;
	if (option_is("page-set", "even"))
		page->page_set = PAGE_SET_EVEN;
	else if (option_is("page-set", "odd"))
		page->page_set = PAGE_SET_ODD;
}

static void configure_alignment(struct page_state *page)
{
	static const struct {
		const char *name;
		uint16_t flag;
	} choices[] = {
		{ "BRVertLineAlignOther", ALIGN_OTHER },
		{ "BRVertLineAlignA4Letter", ALIGN_A4_LETTER },
		{ "BRVertLineAlignA3B4Ledger", ALIGN_A3_B4_LEDGER },
		{ "BRVertLineAlignDedicated", ALIGN_DEDICATED },
	};
	size_t index;

	for (index = 0; index < sizeof(choices) / sizeof(choices[0]); index++) {
		page->alignment_flags &= ~choices[index].flag;
		if (option_is(choices[index].name, "ON"))
			page->alignment_flags |= choices[index].flag;
	}
}

static uint16_t configure_color_options(const struct job_settings *settings,
				       struct page_state *page, uint16_t flags,
				       bool vendor_matching)
{
	uint16_t *options = page->device_options;

	options[DEVICE_BRIGHTNESS_CONTRAST] = 0;
	options[DEVICE_RED_GREEN] = 0;
	options[DEVICE_BLUE_LEVELS] = 0;
	if (!vendor_matching) {
		if (page->color_mode == PAGE_COLOR)
			flags |= RENDER_NO_COLOR_MATCHING;
		return flags | RENDER_DIFFUSION;
	}
	if (option_is("ColorMatch", "Vivid"))
		flags |= RENDER_VIVID;
	else if (option_is("ColorMatch", "None"))
		flags |= RENDER_NO_COLOR_MATCHING;
	if (!option_is("HalfTone", "Dither"))
		flags |= RENDER_DIFFUSION;
	options[DEVICE_BLUE_LEVELS] = DEFAULT_BLUE_LEVELS;
	if (option_is("ColorEnhance", "ON")) {
		flags |= RENDER_ENHANCEMENT;
		/* Keep each signed adjustment in its own byte. */
		options[DEVICE_BRIGHTNESS_CONTRAST] =
			((uint8_t)settings->contrast << 8) | (uint8_t)settings->brightness;
		options[DEVICE_RED_GREEN] =
			((uint8_t)settings->green << 8) | (uint8_t)settings->red;
		options[DEVICE_BLUE_LEVELS] |= (uint8_t)settings->blue;
	}
	return flags;
}

void configure_printer_options(struct job_settings *settings, struct page_state *page)
{
	uint16_t *options = page->device_options;
	struct printer_state *printer = page->printer;
	bool vendor_matching = !option_is("AP_ColorMatchingMode", "AP_ApplicationColorMatching");
	const char *choice;
	uint16_t borderless, flags;

	configure_page_geometry(settings, page);
	options[DEVICE_MEDIA] = settings->media < 0 ? JOB_MEDIA_PLAIN : settings->media;
	choice = current_option_text("print-quality");
	if (settings->media >= 0 && choice &&
	    strtol(choice, NULL, 10) == IPP_HIGH_QUALITY)
		options[DEVICE_MEDIA] = JOB_MEDIA_PHOTO;
	/* Quality selection still uses the original media, before that override. */
	options[DEVICE_QUALITY] = select_quality(settings->media, current_option_text("BResolution"),
						&options[DEVICE_RENDER_DPI]);
	page->color_mode = option_is("ColorOrMono", "Mono") ? PAGE_MONOCHROME : PAGE_COLOR;
	page->paper_thickness = THICKNESS_REGULAR;
	if (option_is("PaperThick", "Thick"))
		page->paper_thickness = THICKNESS_THICK;
	else if (option_is("PaperThick", "Env"))
		page->paper_thickness = THICKNESS_ENVELOPE;
	page->paper_source = 0;
	if (option_is("PaperSource", "Tray1"))
		page->paper_source = SOURCE_SETTING_TRAY_1;
	else if (option_is("PaperSource", "Tray2"))
		page->paper_source = SOURCE_SETTING_TRAY_2;
	else if (option_is("PaperSource", "MPTray"))
		page->paper_source = SOURCE_SETTING_MULTIPURPOSE;
	else if (option_is("PaperSource", "Manual"))
		page->paper_source = SOURCE_SETTING_MANUAL;

	flags = RENDER_DEFAULT_FLAGS;
	if (!option_is("BiDirPrint", "OFF"))
		flags |= RENDER_BIDIRECTIONAL;
	if (option_is("BRMirror", "ON"))
		flags |= RENDER_MIRRORED;
	if (vendor_matching && option_is("SlowDrying", "ON"))
		flags |= RENDER_SLOW_DRYING;
	if (option_is("BRReverse", "ON"))
		flags |= RENDER_REVERSED;
	choice = current_option_text("JpegPrinting");
	if (!choice || text_is(choice, "Recommended"))
		page->jpeg_mode = JPEG_RECOMMENDED;
	else if (text_is(choice, "QualityPrior"))
		page->jpeg_mode = JPEG_QUALITY_PRIORITY;
	else if (text_is(choice, "SpeedPrior"))
		page->jpeg_mode = JPEG_SPEED_PRIORITY;
	/* Unknown explicit JPEG choices retain the previous selection. */
	configure_duplex(page);
	choice = current_option_text("PaperType");
	if (choice)
		strcpy(page->paper_name, choice);
	/* Preserve the renderer's height-first lookup for unnamed paper. */
	options[DEVICE_PAPER_ID] = find_paper_id((char *)choice, page->paper_height_120dpi,
						page->paper_width_120dpi, &borderless);
	options[DEVICE_PAPER_FLAGS] = PAPER_DEFAULT_FLAGS;
	if (borderless == 1)
		options[DEVICE_PAPER_FLAGS] |= PAPER_BORDERLESS_FLAG;
	options[DEVICE_RESERVED_CONTROL] = 0;
	configure_alignment(page);
	options[DEVICE_RENDER_FLAGS] = configure_color_options(settings, page, flags, vendor_matching);
	options[DEVICE_CHANNEL_LEVELS] = DEFAULT_CHANNEL_LEVELS;
	options[DEVICE_DENSITY] = DEFAULT_DENSITY;
	choice = current_option_text("com.apple.print.PrintSettings.PMTotalBeginPages..n.");
	page->first_page = choice ? strtol(choice, NULL, 10) : UNKNOWN_FIRST_PAGE;
	store_word(printer, STATE_FIRST_PAGE, page->first_page);
	if (options[DEVICE_PAPER_ID] == PAPER_ID_DISC_12CM &&
	    settings->media != JOB_MEDIA_DISC && settings->media != JOB_MEDIA_DISC_ALTERNATE) {
		options[DEVICE_MEDIA] = JOB_MEDIA_DISC;
		options[DEVICE_QUALITY] = QUALITY_BEST;
	}
	options[DEVICE_INTERFACE] = option_is("BRInterface", "WirelessLAN") ? INTERFACE_WIRELESS : 0;
	store_word(printer, STATE_DUPLEX_CONTROL, DEFAULT_DUPLEX_CONTROL);
}

uint32_t should_process_page(struct page_state *page)
{
	return page->device_options[DEVICE_PAPER_ID] != PAPER_ID_DISC_12CM ||
	       state_dword(page->printer, STATE_PAGE_NUMBER) <= 1;
}

static uint32_t renderer_media(uint16_t media)
{
	switch (media) {
	case JOB_MEDIA_INKJET:
		return MEDIA_INKJET;
	case JOB_MEDIA_PHOTO:
		return MEDIA_OTHER_PHOTO;
	case JOB_MEDIA_VARIANT_3:
		return MEDIA_VARIANT_28;
	case JOB_MEDIA_VARIANT_5:
		return MEDIA_VARIANT_17;
	case JOB_MEDIA_HAGAKI_ADDRESS:
		return MEDIA_HAGAKI_ADDRESS;
	case JOB_MEDIA_HAGAKI_REVERSE:
		return MEDIA_HAGAKI_REVERSE;
	case JOB_MEDIA_BP71:
		return MEDIA_BROTHER_BP71;
	case JOB_MEDIA_DISC:
		return MEDIA_VARIANT_1C;
	case JOB_MEDIA_DISC_ALTERNATE:
		return MEDIA_VARIANT_1D;
	case JOB_MEDIA_BP60:
		return MEDIA_BROTHER_BP60;
	default:
		return MEDIA_PLAIN;
	}
}

static bool plain_media(uint32_t media)
{
	return media == MEDIA_PLAIN || media == MEDIA_HAGAKI_ADDRESS;
}

static int scale_printable_coordinate(int value, int dpi)
{
	switch (dpi) {
	case POINTS_PER_INCH:
		/* Keep the inherited decimal approximation and asymmetric rounding. */
		return truncl((float)value * 8.333333 + 0.5);
	case FAST_RENDER_DPI:
		return value * (NORMAL_RENDER_DPI / FAST_RENDER_DPI);
	case PAPER_SIZE_DPI:
		return value * (NORMAL_RENDER_DPI / PAPER_SIZE_DPI);
	default:
		return value;
	}
}

static void configure_printable_bounds(struct page_state *page)
{
	struct printer_state *printer = page->printer;
	uint16_t *options = page->device_options;
	int width = scale_ratio((int16_t)options[DEVICE_WIDTH_120DPI],
			       NORMAL_RENDER_DPI, PAGE_GEOMETRY_DPI);
	int height = scale_ratio((int16_t)options[DEVICE_HEIGHT_120DPI],
				NORMAL_RENDER_DPI, PAGE_GEOMETRY_DPI);
	int dpi = (int16_t)options[DEVICE_HORIZONTAL_DPI];
	int left, bottom, printable_width, printable_height;
	int top_margin, bottom_margin, left_margin, right_margin, horizontal_margins;

	if (options[DEVICE_PAPER_FLAGS] & PAPER_BORDERLESS_FLAG) {
		if (!options[DEVICE_BOTTOM_MARGIN])
			options[DEVICE_BOTTOM_MARGIN] = BORDERLESS_DEFAULT_MARGIN;
		if (!options[DEVICE_LEFT_MARGIN])
			options[DEVICE_LEFT_MARGIN] = BORDERLESS_DEFAULT_MARGIN;
	}
	bottom = scale_printable_coordinate(-(int16_t)options[DEVICE_BOTTOM_MARGIN], dpi);
	left = scale_printable_coordinate(-(int16_t)options[DEVICE_LEFT_MARGIN], dpi);
	printable_width = scale_printable_coordinate((int16_t)options[DEVICE_PRINTABLE_WIDTH], dpi);
	printable_height = scale_printable_coordinate((int16_t)options[DEVICE_PRINTABLE_HEIGHT], dpi);
	store_dword(printer, STATE_PAPER_WIDTH_600DPI, width);
	store_dword(printer, STATE_PAPER_HEIGHT_600DPI, height);
	store_dword(printer, STATE_BOTTOM_ORIGIN, bottom);
	store_dword(printer, STATE_LEFT_ORIGIN, left);
	store_dword(printer, STATE_PRINTABLE_WIDTH, printable_width);
	store_dword(printer, STATE_PRINTABLE_HEIGHT, printable_height);
	/*
	 * Preserve the old axis assignment: horizontal margins are split between
	 * IMAGE_TOP_BOUND/IMAGE_BOTTOM_BOUND, and vertical ones use LEFT/RIGHT.
	 */
	left_margin = -bottom;
	right_margin = abs(bottom + height - printable_height);
	horizontal_margins = abs(left + width - printable_width);
	if (left_margin <= SMALL_MARGIN_LIMIT) {
		top_margin = SMALL_MARGIN_FALLBACK;
		bottom_margin = SMALL_MARGIN_FALLBACK;
		left_margin = SMALL_MARGIN_FALLBACK;
		right_margin = SMALL_MARGIN_FALLBACK;
	} else {
		horizontal_margins -= left;
		top_margin = horizontal_margins / 2;
		bottom_margin = horizontal_margins - top_margin;
	}
	store_dword(printer, STATE_IMAGE_TOP_BOUND, top_margin);
	store_dword(printer, STATE_IMAGE_LEFT_BOUND, left_margin);
	store_dword(printer, STATE_IMAGE_BOTTOM_BOUND, bottom_margin);
	store_dword(printer, STATE_IMAGE_RIGHT_BOUND, right_margin);
}

void configure_raster_geometry(struct page_state *page)
{
	struct printer_state *printer = page->printer;
	const uint16_t *options = page->device_options;
	uint16_t flags = options[DEVICE_RENDER_FLAGS];
	uint16_t binding = BINDING_SIMPLEX;
	uint32_t media = renderer_media(options[DEVICE_MEDIA]);
	uint8_t orientation = state_byte(printer, STATE_IMAGE_ORIENTATION);
	uint8_t bidirectional = state_byte(printer, STATE_BIDIRECTIONAL_FLAGS);
	uint8_t encoding, matching;

	store_word(printer, STATE_JOB_ACTIVE, 1);
	store_word(printer, STATE_COLOR_SELECTION, page->color_mode == PAGE_MONOCHROME ? 1 : OUTPUT_COLOR);
	store_dword(printer, STATE_MEDIA_SELECTION, media);
	store_dword(printer, STATE_REQUESTED_MEDIA, media);
	store_byte(printer, STATE_SLOW_DRYING, !!(flags & RENDER_SLOW_DRYING));
	store_dword(printer, STATE_REQUESTED_DRYING, !!(flags & RENDER_SLOW_DRYING));
	orientation &= ~(ORIENTATION_MIRRORED | ORIENTATION_REVERSED);
	if (flags & RENDER_MIRRORED)
		orientation |= ORIENTATION_MIRRORED;
	if (flags & RENDER_REVERSED)
		orientation |= ORIENTATION_REVERSED;
	store_byte(printer, STATE_IMAGE_ORIENTATION, orientation);
	store_word(printer, STATE_QUALITY, options[DEVICE_QUALITY]);
	store_word(printer, STATE_REQUESTED_DPI, options[DEVICE_RENDER_DPI]);
	store_word(printer, STATE_PAPER_ID, options[DEVICE_PAPER_ID]);
	store_dword(printer, STATE_PAPER_ID_DWORD, (int16_t)options[DEVICE_PAPER_ID]);
	store_byte(printer, STATE_BORDERLESS, !!(options[DEVICE_PAPER_FLAGS] & PAPER_BORDERLESS_FLAG));
	store_word(printer, STATE_PAPER_SOURCE,
		   page->paper_source >= SOURCE_SETTING_TRAY_1 && page->paper_source <= SOURCE_SETTING_MANUAL ?
		   page->paper_source : SOURCE_SETTING_AUTOMATIC);
	/* The printer coordinate system retains the exchanged sheet axes. */
	store_word(printer, STATE_PAPER_HEIGHT_TENTHS_MM,
		   scale_ratio(page->paper_width_120dpi, TENTHS_MM_PER_INCH, PAGE_GEOMETRY_DPI));
	store_word(printer, STATE_PAPER_WIDTH_TENTHS_MM,
		   scale_ratio(page->paper_height_120dpi, TENTHS_MM_PER_INCH, PAGE_GEOMETRY_DPI));
	store_byte(printer, STATE_THICKNESS, page->paper_thickness == THICKNESS_THICK ?
		   THICKNESS_SETTING_THICK : page->paper_thickness == THICKNESS_ENVELOPE ?
		   THICKNESS_SETTING_ENVELOPE : 0);
	store_byte(printer, STATE_ALIGNMENT_FLAGS, page->alignment_flags);
	store_word(printer, STATE_BRIGHTNESS, (int8_t)options[DEVICE_BRIGHTNESS_CONTRAST]);
	store_word(printer, STATE_CONTRAST, (int16_t)options[DEVICE_BRIGHTNESS_CONTRAST] >> 8);
	store_word(printer, STATE_RED, (int8_t)options[DEVICE_RED_GREEN]);
	store_word(printer, STATE_GREEN, (int16_t)options[DEVICE_RED_GREEN] >> 8);
	store_word(printer, STATE_BLUE, (int8_t)options[DEVICE_BLUE_LEVELS]);
	store_byte(printer, STATE_ENHANCEMENT_ENABLED, !!(flags & RENDER_ENHANCEMENT));
	store_byte(printer, STATE_DITHER_ENABLED, !(flags & RENDER_DIFFUSION));
	if (flags & RENDER_NO_COLOR_MATCHING)
		matching = MATCHING_NONE;
	else
		matching = flags & RENDER_VIVID ? MATCHING_VIVID : MATCHING_NATURAL;
	store_byte(printer, STATE_COLOR_MATCHING, matching);
	store_byte(printer, STATE_REQUESTED_COMPRESSION, COMPRESSION_AUTOMATIC);
	store_byte(printer, STATE_MULTILEVEL_ENABLED, (int8_t)options[DEVICE_DENSITY] >= 0);
	if (!(flags & RENDER_BIDIRECTIONAL) ||
	    (options[DEVICE_PAPER_FLAGS] & PAPER_BIDIRECTIONAL_MASK) != PAPER_BIDIRECTIONAL_MASK)
		bidirectional &= ~(BIDIRECTIONAL_PLAIN_MEDIA | BIDIRECTIONAL_PHOTO_MEDIA);
	else
		bidirectional |= plain_media(media) ? BIDIRECTIONAL_PLAIN_MEDIA : BIDIRECTIONAL_PHOTO_MEDIA;
	store_byte(printer, STATE_BIDIRECTIONAL_FLAGS, bidirectional);
	if (page->duplex == PAGE_DUPLEX_LONG_EDGE)
		binding = BINDING_DUPLEX_LONG_EDGE;
	else if (page->duplex == PAGE_DUPLEX_SHORT_EDGE)
		binding = BINDING_DUPLEX_SHORT_EDGE;
	store_word(printer, STATE_DUPLEX_BINDING, binding);
	if (binding == BINDING_SIMPLEX) {
		store_byte(printer, STATE_PAGE_SELECTION, page->page_set ? PAGE_SELECTION_FILTERED : 0);
		if (page->page_set)
			store_byte(printer, STATE_SELECTED_PAGE_SIDE, page->page_set);
	} else {
		store_byte(printer, STATE_PAGE_SELECTION, 2);
	}
	store_byte(printer, STATE_PAGE_FILTER_ENABLED, state_byte(printer, STATE_PAGE_SELECTION) != 0);
	store_byte(printer, STATE_DUPLEX_DRYING, page->duplex_mode);
	if (binding != BINDING_SIMPLEX && !plain_media(media)) {
		store_dword(printer, STATE_MEDIA_SELECTION, MEDIA_PLAIN);
		store_word(printer, STATE_RENDER_DPI, NORMAL_RENDER_DPI);
	}
	choose_raster_mode(printer);
	store_byte(printer, STATE_INTERLACE_FLAGS, choose_interlace_flags(printer));
	store_word(printer, STATE_INTERFACE_FLAGS, options[DEVICE_INTERFACE]);
	store_byte(printer, STATE_JPEG_PRIORITY, page->jpeg_mode);
	encoding = use_enhanced_media_mode(printer);
	store_byte(printer, STATE_IMAGE_ENCODING, encoding);
	store_byte(printer, STATE_PAGE_END_ENCODING, encoding);
	configure_printable_bounds(page);
	store_word(printer, STATE_CALIBRATION_MODE, 0);
	printer->buffers.color_cube = 0;
	store_byte(printer, STATE_HALFTONE_DITHER, 0);
}

uint32_t paper_uses_legacy_raster(struct printer_state *printer)
{
	switch (state_word(printer, STATE_PAPER_ID)) {
	case PAPER_ID_ISO_B5:
	case PAPER_ID_ENVELOPE_C5:
	case PAPER_ID_ENVELOPE_YOU4:
	case PAPER_ID_ENVELOPE_CHOU3:
	case PAPER_ID_ENVELOPE_C4:
	case PAPER_ID_ENVELOPE_10:
	case PAPER_ID_ENVELOPE_DL:
	case PAPER_ID_ENVELOPE_MONARCH:
		return 1;
	default:
		return 0;
	}
}

static unsigned int media_quality_row(uint32_t media)
{
	switch (media) {
	case MEDIA_INKJET:
	case MEDIA_BROTHER_BP60:
		return 1;
	case MEDIA_OTHER_PHOTO:
	case MEDIA_VARIANT_17:
	case MEDIA_BROTHER_BP71:
	case MEDIA_HAGAKI_REVERSE:
		return 2;
	case MEDIA_VARIANT_28:
		return 3;
	case MEDIA_VARIANT_1C:
		return 4;
	case MEDIA_VARIANT_1D:
		return 5;
	default:
		return 0;
	}
}

void choose_raster_mode(struct printer_state *printer)
{
	uint32_t media = state_dword(printer, STATE_MEDIA_SELECTION);
	uint16_t quality = state_word(printer, STATE_QUALITY);
	uint16_t dpi = state_word(printer, STATE_REQUESTED_DPI);
	uint16_t mode = state_word(printer, STATE_RASTER_MODE);
	uint16_t family;
	unsigned int quality_index = QUALITY_PHOTO - 1;
	unsigned int row = media_quality_row(media);

	if (quality >= QUALITY_FAST && quality <= QUALITY_BEST)
		quality_index = quality - QUALITY_FAST;
	store_word(printer, STATE_RENDER_DPI, dpi);
	/* Six media rows of four quality classes. */
	switch (lpr_quality_modes[row * QUALITY_CLASSES + quality_index]) {
	case 0:
		mode = dpi == PAPER_SIZE_DPI ? RASTER_PROFILE_0205 : RASTER_PROFILE_010F;
		break;
	case 1:
		mode = dpi == NORMAL_RENDER_DPI ? RASTER_PROFILE_0201 : RASTER_PROFILE_0205;
		break;
	case 2:
		mode = RASTER_PROFILE_0201;
		break;
	case 3:
		mode = plain_media(media) ? RASTER_PROFILE_0600 : RASTER_PROFILE_0608;
		break;
	case 4:
		mode = RASTER_PROFILE_0600;
		break;
	case 5:
		mode = RASTER_PROFILE_0700;
		break;
	}
	if (state_word(printer, STATE_COLOR_SELECTION) != OUTPUT_COLOR) {
		mode |= RASTER_MONOCHROME;
		if ((row != 0) || (plain_media(media) &&
		    (state_byte(printer, STATE_SLOW_DRYING) == SLOW_DRYING_ENABLED ||
		     state_word(printer, STATE_DUPLEX_BINDING) != BINDING_SIMPLEX)))
			mode |= RASTER_CHANNEL_MASK;
	}
	family = mode & RASTER_RESOLUTION_FAMILY_MASK;
	if (state_byte(printer, STATE_MULTILEVEL_ENABLED) == 1) {
		if (family == RASTER_MEDIUM_RESOLUTION_FAMILY && quality != QUALITY_FAST)
			mode |= RASTER_FIVE_LEVEL_DOTS;
		else
			mode |= RASTER_FOUR_LEVEL_DOTS;
	} else if (family >= RASTER_PROFILE_0500) {
		mode |= RASTER_FOUR_LEVEL_DOTS;
	}
	store_word(printer, STATE_RASTER_MODE, mode);
}

uint32_t use_enhanced_media_mode(struct printer_state *printer)
{
	uint8_t priority = state_byte(printer, STATE_JPEG_PRIORITY);
	uint8_t allowed = priority == JPEG_RECOMMENDED ? JPEG_RECOMMENDED_BIT :
		priority == JPEG_SPEED_PRIORITY ? JPEG_ELIGIBLE | JPEG_RECOMMENDED_BIT : 0;
	uint8_t eligible = 0;
	uint16_t mode = state_word(printer, STATE_RASTER_MODE);
	uint16_t family = mode & RASTER_RESOLUTION_FAMILY_MASK;
	uint16_t paper = state_word(printer, STATE_PAPER_ID);
	bool custom_paper = paper == PAPER_ID_CUSTOM ||
		(uint16_t)(paper - PAPER_ID_CUSTOM_FIRST) < PAPER_ID_CUSTOM_COUNT;

	switch (state_dword(printer, STATE_MEDIA_SELECTION)) {
	case MEDIA_PLAIN:
	case MEDIA_HAGAKI_ADDRESS:
		if (!paper_uses_legacy_raster(printer) &&
		    !(custom_paper && state_byte(printer, STATE_THICKNESS) == THICKNESS_SETTING_ENVELOPE))
			eligible = state_word(printer, STATE_COLOR_SELECTION) == OUTPUT_COLOR;
		break;
	case MEDIA_INKJET:
	case MEDIA_BROTHER_BP60:
		eligible = (mode & RASTER_PROFILE_MASK) == RASTER_PROFILE_0508 ?
			JPEG_ELIGIBLE | JPEG_RECOMMENDED_BIT : JPEG_ELIGIBLE;
		break;
	case MEDIA_OTHER_PHOTO:
	case MEDIA_VARIANT_17:
	case MEDIA_BROTHER_BP71:
	case MEDIA_HAGAKI_REVERSE:
		eligible = JPEG_ELIGIBLE;
		/* Job setup supplies quality 1..4; the retained table includes index 0. */
		if ((state_word(printer, STATE_INTERFACE_FLAGS) & INTERFACE_WIRELESS) &&
		    lpr_media_modes[(int16_t)state_word(printer, STATE_QUALITY)] == 3)
			eligible |= JPEG_RECOMMENDED_BIT;
		break;
	}
	if (family == RASTER_LOW_RESOLUTION_FAMILY || family == RASTER_MEDIUM_RESOLUTION_FAMILY ||
	    family == RASTER_PROFILE_0700 ||
	    state_byte(printer, STATE_PAGE_FILTER_ENABLED) == 1)
		eligible = 0;
	return (eligible & allowed) != 0;
}

void choose_compression_mode(struct printer_state *printer)
{
	uint8_t requested = state_byte(printer, STATE_REQUESTED_COMPRESSION);
	uint32_t raster_key = state_dword(printer, STATE_INTERLACE_FLAGS);

	/* This packed key spans interlace, borderless and the 16-bit raster mode. */
	if (requested == COMPRESSION_AUTOMATIC ||
	    (requested == ROW_CODEC_RANGE_CODER &&
	     (raster_key & RANGE_COMPATIBILITY_MASK) != RANGE_COMPATIBLE_MODE))
		requested = ROW_CODEC_ZERO_RUNS;
	store_byte(printer, STATE_ROW_COMPRESSION, requested);
}
