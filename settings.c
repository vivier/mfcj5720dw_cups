/* Resolve marked CUPS options and validate the raster supplied by CUPS. */
#include "driver.h"
#include "encoder/config.h"
#include "encoder/settings.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum raster_format {
	RGB_CHANNELS = 3,
	GRAY_CHANNELS = 1,
	BITS_PER_SAMPLE = 8,
	BYTES_PER_SAMPLE = 1,
	PAPER_TABLE_DPI = 600,
	FAST_RASTER_DPI = 150,
	FAST_RASTER_SCALE = PAPER_TABLE_DPI / FAST_RASTER_DPI,
	MAX_RASTER_WIDTH = 8000,
	MAX_RASTER_HEIGHT = 11000,
	MAX_ROW_PADDING = 16,
	PAPER_ROUNDING_TOLERANCE = 8,
};

/* Configuration codes, distinct from the encoder's internal mode identifiers. */
enum job_option_value {
	QUALITY_PLAIN_FAST = 0,
	DUPLEX_SIMPLEX = 0,
	DUPLEX_SHORT_EDGE = 2,
	MIN_COLOR_ADJUSTMENT = -20,
	MAX_COLOR_ADJUSTMENT = 20,
};

/* Keep PPD keywords tied to named encoder options rather than table positions. */
static const char *const ppd_keywords[OPTION_COUNT] = {
	[OPTION_MEDIA] = "BRMediaType",
	[OPTION_RESOLUTION] = "BRResolution",
	[OPTION_BIDIRECTIONAL] = "BRBiDir",
	[OPTION_PAPER_SIZE] = "PageSize",
	[OPTION_PAPER_THICKNESS] = "BRColorPaperThick",
	[OPTION_PAPER_SOURCE] = "BRInputSlot",
	[OPTION_COLOR_MODE] = "BRMonoColor",
	[OPTION_COLOR_MATCHING] = "BRColorMatching",
	[OPTION_JPEG_PRIORITY] = "BRJpeg",
	[OPTION_HALFTONE] = "BRHalfTonePattern",
	[OPTION_COLOR_ENHANCEMENT] = "BRColorEnhancement",
	[OPTION_SLOW_DRYING] = "BRSlowDrying",
	[OPTION_ALIGN_OTHER] = "BRVertLineAlignOther",
	[OPTION_ALIGN_A4_LETTER] = "BRVertLineAlignA4Letter",
	[OPTION_ALIGN_A3_B4_LEDGER] = "BRVertLineAlignA3B4Ledger",
	[OPTION_MIRROR] = "BRMirror",
	[OPTION_DUPLEX_BINDING] = "Duplex",
	[OPTION_DUPLEX_DRYING] = "BRDuplexMode",
	[OPTION_BRIGHTNESS] = "BRBrightness",
	[OPTION_CONTRAST] = "BRContrast",
	[OPTION_RED] = "BRRed",
	[OPTION_GREEN] = "BRGreen",
	[OPTION_BLUE] = "BRBlue",
};

static int read_marked_option(ppd_file_t *ppd, struct configuration *config, int index)
{
	ppd_choice_t *choice = ppdFindMarkedChoice(ppd, ppd_keywords[index]);
	char *end;
	long adjustment;

	/* Mirroring is handled upstream by CUPS and is absent from this PPD. */
	if (index == OPTION_MIRROR) {
		set_choice_option(index, "OFF", config, false);
		return 0;
	}
	if (!choice)
		return 1;
	if (index < CHOICE_OPTION_COUNT)
		return set_choice_option(index, choice->choice, config, false);

	/* Preserve strtol's signed decimal syntax; trailing characters are invalid. */
	errno = 0;
	adjustment = strtol(choice->choice, &end, 10);
	if (errno || end == choice->choice || *end || adjustment < MIN_COLOR_ADJUSTMENT ||
	    adjustment > MAX_COLOR_ADJUSTMENT)
		return 1;
	config->options[index].value = adjustment;
	return 0;
}

static void copy_job_options(struct configuration *config, struct job_settings *settings)
{
	settings->media = get_option_code(OPTION_MEDIA, config);
	settings->resolution = get_option_code(OPTION_RESOLUTION, config);
	settings->bidirectional = get_option_code(OPTION_BIDIRECTIONAL, config);
	settings->paper_type = get_option_code(OPTION_PAPER_SIZE, config);
	settings->thickness = get_option_code(OPTION_PAPER_THICKNESS, config);
	settings->paper_source = get_option_code(OPTION_PAPER_SOURCE, config);
	settings->color_mode = get_option_code(OPTION_COLOR_MODE, config);
	settings->color_matching = get_option_code(OPTION_COLOR_MATCHING, config);
	settings->halftone = get_option_code(OPTION_HALFTONE, config);
	settings->enhancement = get_option_code(OPTION_COLOR_ENHANCEMENT, config);
	settings->slow_drying = get_option_code(OPTION_SLOW_DRYING, config);
	settings->align_other = get_option_code(OPTION_ALIGN_OTHER, config);
	settings->align_a4_letter = get_option_code(OPTION_ALIGN_A4_LETTER, config);
	settings->align_a3_ledger = get_option_code(OPTION_ALIGN_A3_B4_LEDGER, config);
	settings->duplex = get_option_code(OPTION_DUPLEX_BINDING, config);
	settings->duplex_mode = get_option_code(OPTION_DUPLEX_DRYING, config);
	settings->brightness = config->options[OPTION_BRIGHTNESS].value;
	settings->contrast = config->options[OPTION_CONTRAST].value;
	settings->red = config->options[OPTION_RED].value;
	settings->green = config->options[OPTION_GREEN].value;
	settings->blue = config->options[OPTION_BLUE].value;
}

static void copy_paper_geometry(const struct paper_geometry *paper, struct job_settings *settings)
{
	strcpy(settings->paper_name, paper->name);
	settings->borderless = strstr(paper->name, "_B") != NULL;
	settings->width_points = paper->width_points;
	settings->height_points = paper->height_points;
	settings->paper_width = paper->width_points;
	settings->paper_height = paper->height_points;
	settings->width_pixels = paper->width_pixels;
	settings->height_pixels = paper->height_pixels;
	settings->left_margin = paper->left;
	settings->bottom_margin = paper->height_points - paper->bottom;
	settings->image_right = paper->width_points - paper->left;
	settings->image_bottom = paper->height_points - paper->top;
	/* Paper dimensions are tabulated at 600 dpi; Plain Fast supplies 150 dpi. */
	if (settings->resolution == QUALITY_PLAIN_FAST) {
		settings->width_pixels /= FAST_RASTER_SCALE;
		settings->height_pixels /= FAST_RASTER_SCALE;
	}
	settings->row_bytes = settings->width_pixels * RGB_CHANNELS;
	settings->sample_bytes = BYTES_PER_SAMPLE;
	settings->pixel_bytes = RGB_CHANNELS * BYTES_PER_SAMPLE;
}

int configure_job(ppd_file_t *ppd, struct job_settings *settings)
{
	struct configuration *config = &lpr_configuration;
	const struct paper_geometry *paper;
	int index;

	memset(settings, 0, sizeof(*settings));
	strcpy(config->model, "mfcj5720dw");
	strcpy(settings->model, config->model);
	for (index = 0; index < OPTION_COUNT; index++) {
		if (read_marked_option(ppd, config, index)) {
			fprintf(stderr, "ERROR: Missing or invalid PPD option %s.\n",
				ppd_keywords[index]);
			return 1;
		}
	}
	copy_job_options(config, settings);
	paper = find_paper(get_option_text("PaperType", config));
	if (!paper) {
		fputs("ERROR: Unsupported paper size.\n", stderr);
		return 1;
	}
	copy_paper_geometry(paper, settings);
	return 0;
}

static unsigned int raster_channels(cups_cspace_t color_space)
{
	switch (color_space) {
	case CUPS_CSPACE_RGB:
	case CUPS_CSPACE_SRGB:
		return RGB_CHANNELS;
	case CUPS_CSPACE_W:
	case CUPS_CSPACE_SW:
		return GRAY_CHANNELS;
	default:
		return 0;
	}
}

int validate_raster_header(const cups_page_header2_t *header, const struct job_settings *settings)
{
	unsigned int resolution =
		settings->resolution == QUALITY_PLAIN_FAST ? FAST_RASTER_DPI : PAPER_TABLE_DPI;
	unsigned int channels = raster_channels(header->cupsColorSpace);

	/* Bound dimensions before row-size arithmetic, signed comparisons or allocation. */
	if (!channels || !header->cupsWidth || header->cupsWidth > MAX_RASTER_WIDTH ||
	    !header->cupsHeight || header->cupsHeight > MAX_RASTER_HEIGHT)
		goto invalid;
	if (header->cupsColorOrder != CUPS_ORDER_CHUNKED ||
	    header->cupsBitsPerColor != BITS_PER_SAMPLE || header->cupsNumColors != channels ||
	    header->cupsBitsPerPixel != channels * BITS_PER_SAMPLE ||
	    header->cupsBytesPerLine < header->cupsWidth * channels ||
	    header->cupsBytesPerLine > header->cupsWidth * channels + MAX_ROW_PADDING)
		goto invalid;
	/* Small rounding differences are cropped/padded by the raster reader. */
	if (header->HWResolution[0] != resolution || header->HWResolution[1] != resolution ||
	    header->PageSize[0] != (unsigned int)settings->width_points ||
	    header->PageSize[1] != (unsigned int)settings->height_points ||
	    abs((int)header->cupsWidth - settings->width_pixels) > PAPER_ROUNDING_TOLERANCE ||
	    abs((int)header->cupsHeight - settings->height_pixels) > PAPER_ROUNDING_TOLERANCE)
		goto invalid;
	/*
	 * CUPS has already expanded copies and applied rotation/mirroring.
	 * Copy counts of zero and one are accepted.
	 */
	if (header->NumCopies > 1 || header->Orientation || header->MirrorPrint ||
	    header->NegativePrint || header->Duplex != (settings->duplex != DUPLEX_SIMPLEX) ||
	    (header->Duplex && header->Tumble != (settings->duplex == DUPLEX_SHORT_EDGE)))
		goto invalid;
	return 0;

invalid:
	fputs("ERROR: Unsupported raster format or page settings; expected uniform "
	      "8-bit RGB/gray pages with copies expanded by CUPS.\n",
	      stderr);
	return 1;
}
