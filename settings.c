/* Resolve marked CUPS options and validate the raster supplied by CUPS. */
#include "driver.h"
#include "raster.h"
#include "encoder/config.h"
#include "encoder/settings.h"

#include <errno.h>
#include <math.h>
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
	POINTS_PER_INCH = 72,
	FAST_RASTER_DPI = 150,
	FAST_RASTER_SCALE = PAPER_TABLE_DPI / FAST_RASTER_DPI,
	MAX_RASTER_WIDTH = 8000,
	MAX_RASTER_HEIGHT = 11000,
	MAX_ROW_PADDING = 16,
	PAPER_ROUNDING_TOLERANCE = 8,
};

/* The largest supported stock enlargement is 1.0595 (3.5 x 5 inches). */
#define MAX_BORDERLESS_SCALE 1.06f
#define MAX_BORDERLESS_DOWNWARD_MM 2.0f
#define PAGE_POINT_TOLERANCE 1.0f

/* Brother's user-defined range: 88.9 x 127 through 287 x 431.8 mm. */
#define CUSTOM_MIN_WIDTH 252.0f
#define CUSTOM_MIN_HEIGHT 360.0f
#define CUSTOM_MAX_WIDTH (287.0f * POINTS_PER_INCH / 25.4f)
#define CUSTOM_MAX_HEIGHT 1224.0f
#define CUSTOM_MARGIN 9
#define CUSTOM_POINT_TOLERANCE 0.01f

/* Configuration codes, distinct from the encoder's internal mode identifiers. */
enum job_option_value {
	QUALITY_PLAIN_FAST = 0,
	DUPLEX_SIMPLEX = 0,
	DUPLEX_SHORT_EDGE = 2,
	MIN_COLOR_ADJUSTMENT = -20,
	MAX_COLOR_ADJUSTMENT = 20,
	MIN_ENHANCEMENT_LEVEL = -2,
	MAX_ENHANCEMENT_LEVEL = 2,
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

static int read_enhancement_level(ppd_file_t *ppd, const char *keyword, int16_t *value)
{
	ppd_choice_t *choice;
	char *end;
	long level;

	/* Older queue PPDs keep the original enhancement settings. */
	*value = 0;
	if (!ppdFindOption(ppd, keyword))
		return 0;
	choice = ppdFindMarkedChoice(ppd, keyword);
	if (!choice)
		goto invalid;
	errno = 0;
	level = strtol(choice->choice, &end, 10);
	if (errno || end == choice->choice || *end || level < MIN_ENHANCEMENT_LEVEL ||
	    level > MAX_ENHANCEMENT_LEVEL)
		goto invalid;
	*value = level;
	return 0;

invalid:
	fprintf(stderr, "ERROR: Missing or invalid PPD option %s.\n", keyword);
	return 1;
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

static int configure_custom_paper(ppd_file_t *ppd, struct job_settings *settings)
{
	ppd_size_t *paper = ppdPageSize(ppd, NULL);
	int resolution = settings->resolution == QUALITY_PLAIN_FAST ?
		FAST_RASTER_DPI : PAPER_TABLE_DPI;
	const char *source = get_option_text("PaperSource", &lpr_configuration);

	/* Do not trust limits or constraints in an old or edited queue PPD. */
	if (!paper || !isfinite(paper->width) || !isfinite(paper->length) ||
	    paper->width < CUSTOM_MIN_WIDTH - CUSTOM_POINT_TOLERANCE ||
	    paper->width > CUSTOM_MAX_WIDTH + CUSTOM_POINT_TOLERANCE ||
	    paper->length < CUSTOM_MIN_HEIGHT - CUSTOM_POINT_TOLERANCE ||
	    paper->length > CUSTOM_MAX_HEIGHT + CUSTOM_POINT_TOLERANCE) {
		fputs("ERROR: Custom paper must be 88.9-287 mm wide and 127-431.8 mm long.\n",
		      stderr);
		return 1;
	}
	if (settings->duplex != DUPLEX_SIMPLEX ||
	    (strcmp(source, "AutoSelect") && strcmp(source, "MPTray"))) {
		fputs("ERROR: Custom paper requires simplex and Auto Select or MP Tray.\n", stderr);
		return 1;
	}
	strcpy(settings->paper_name, "Custom");
	settings->paper_width = paper->width;
	settings->paper_height = paper->length;
	settings->width_points = paper->width + 0.5f;
	settings->height_points = paper->length + 0.5f;
	settings->left_margin = settings->bottom_margin = CUSTOM_MARGIN;
	settings->image_right = settings->width_points - CUSTOM_MARGIN;
	settings->image_bottom = settings->height_points - CUSTOM_MARGIN;
	settings->width_pixels = (paper->width - 2 * CUSTOM_MARGIN) * resolution /
		POINTS_PER_INCH + 0.5f;
	settings->height_pixels = (paper->length - 2 * CUSTOM_MARGIN) * resolution /
		POINTS_PER_INCH + 0.5f;
	settings->row_bytes = settings->width_pixels * RGB_CHANNELS;
	settings->sample_bytes = BYTES_PER_SAMPLE;
	settings->pixel_bytes = RGB_CHANNELS * BYTES_PER_SAMPLE;
	return 0;
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
	if (!strcmp(get_option_text("PaperType", config), "Custom")) {
		if (configure_custom_paper(ppd, settings))
			return 1;
	} else {
		paper = find_paper(get_option_text("PaperType", config));
		if (!paper) {
			fputs("ERROR: Unsupported paper size.\n", stderr);
			return 1;
		}
		copy_paper_geometry(paper, settings);
	}
	if (read_enhancement_level(ppd, "BRColorDensity", &settings->color_density) ||
	    read_enhancement_level(ppd, "BRWhiteBalance", &settings->white_balance))
		return 1;
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

static bool valid_borderless_geometry(const cups_page_header2_t *header,
				     const struct job_settings *settings)
{
	float scale = header->cupsBorderlessScalingFactor;
	unsigned int axis;

	if (!settings->borderless || !isfinite(scale) || scale < 1.0f ||
	    scale > MAX_BORDERLESS_SCALE)
		return false;
	/* Stock borderless media use the whole sheet, with no raster margins. */
	for (axis = 0; axis < 2; axis++) {
		float physical = header->cupsPageSize[axis];
		float nominal = axis ? settings->height_points : settings->width_points;
		unsigned int pixels = axis ? header->cupsHeight : header->cupsWidth;
		float image_edge = header->cupsImagingBBox[axis + 2];
		float scaled_points = physical * scale;

		if (!isfinite(physical) || fabsf(physical - nominal) > PAGE_POINT_TOLERANCE ||
		    !isfinite(image_edge) || fabsf(image_edge - physical) > PAGE_POINT_TOLERANCE ||
		    header->cupsImagingBBox[axis] != 0.0f || header->Margins[axis] ||
		    header->ImagingBoundingBox[axis] ||
		    header->ImagingBoundingBox[axis + 2] != header->PageSize[axis] ||
		    fabsf(header->PageSize[axis] - scaled_points) > PAGE_POINT_TOLERANCE ||
		    fabsf(pixels - scaled_points * header->HWResolution[axis] / POINTS_PER_INCH) >
			PAPER_ROUNDING_TOLERANCE)
			return false;
	}
	return true;
}

static bool valid_custom_geometry(const cups_page_header2_t *header,
				  const struct job_settings *settings)
{
	unsigned int axis;

	for (axis = 0; axis < 2; axis++) {
		float physical = axis ? settings->paper_height : settings->paper_width;
		float size = header->cupsPageSize[axis];
		float lower = header->cupsImagingBBox[axis];
		float upper = header->cupsImagingBBox[axis + 2];
		unsigned int pixels = axis ? header->cupsHeight : header->cupsWidth;
		float expected = (physical - 2 * CUSTOM_MARGIN) *
			header->HWResolution[axis] / POINTS_PER_INCH;

		/* Fractional sheet dimensions and the printable area must agree. */
		if (!isfinite(size) || fabsf(size - physical) > CUSTOM_POINT_TOLERANCE ||
		    fabsf(header->PageSize[axis] - physical) > PAGE_POINT_TOLERANCE ||
		    !isfinite(lower) || fabsf(lower - CUSTOM_MARGIN) > PAGE_POINT_TOLERANCE ||
		    !isfinite(upper) || fabsf(upper - physical + CUSTOM_MARGIN) > PAGE_POINT_TOLERANCE ||
		    header->Margins[axis] != CUSTOM_MARGIN ||
		    fabsf(pixels - expected) > 1.0f)
			return false;
	}
	return true;
}

int validate_raster_header(const cups_page_header2_t *header, const struct job_settings *settings)
{
	unsigned int resolution =
		settings->resolution == QUALITY_PLAIN_FAST ? FAST_RASTER_DPI : PAPER_TABLE_DPI;
	unsigned int channels = raster_channels(header->cupsColorSpace);
	float scale = header->cupsBorderlessScalingFactor;
	float enlargement = header->cupsReal[BORDERLESS_SCALE_REAL];
	float downward_mm = header->cupsReal[BORDERLESS_DOWNWARD_MM_REAL];

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
	if (header->HWResolution[0] != resolution || header->HWResolution[1] != resolution)
		goto invalid;
	/* cupsReal0 requests local expansion without altering the upstream matrix.
	 * Never enlarge a raster that the upstream renderer has already enlarged.
	 */
	if (!isfinite(enlargement) || enlargement < 0.0f ||
	    (enlargement != 0.0f && enlargement < 1.0f) || enlargement > MAX_BORDERLESS_SCALE ||
	    (enlargement > 1.0f && scale != 1.0f))
		goto invalid;
	if (!isfinite(downward_mm) || downward_mm < 0.0f ||
	    downward_mm > MAX_BORDERLESS_DOWNWARD_MM ||
	    (downward_mm != 0.0f && enlargement <= 1.0f))
		goto invalid;
	if (scale > 1.0f || enlargement > 1.0f) {
		if (!valid_borderless_geometry(header, settings))
			goto invalid;
	} else {
		/* Zero is also accepted for legacy headers without the v2 scale field. */
		if (scale != 0.0f && scale != 1.0f)
			goto invalid;
		if (!strcmp(settings->paper_name, "Custom")) {
			if (!valid_custom_geometry(header, settings))
				goto invalid;
		} else if (header->PageSize[0] != (unsigned int)settings->width_points ||
			   header->PageSize[1] != (unsigned int)settings->height_points ||
			   abs((int)header->cupsWidth - settings->width_pixels) > PAPER_ROUNDING_TOLERANCE ||
			   abs((int)header->cupsHeight - settings->height_pixels) > PAPER_ROUNDING_TOLERANCE)
			goto invalid;
	}
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

int configure_raster_page(const cups_page_header2_t *header,
			  const struct job_settings *job, struct job_settings *page)
{
	if (validate_raster_header(header, job))
		return 1;
	*page = *job;
	if (!strcmp(job->paper_name, "Custom")) {
		/* Keep CUPS' pixel rounding without cropping fractional custom sizes. */
		page->width_pixels = header->cupsWidth;
		page->height_pixels = header->cupsHeight;
		page->row_bytes = header->cupsWidth * RGB_CHANNELS;
	}
	if (header->cupsReal[BORDERLESS_SCALE_REAL] > 1.0f) {
		float scale = header->cupsReal[BORDERLESS_SCALE_REAL];

		page->borderless_expansion = 1;
		page->width_points = header->cupsPageSize[0] * scale + 0.5f;
		page->height_points = header->cupsPageSize[1] * scale + 0.5f;
		page->width_pixels = header->cupsWidth * scale + 0.5f;
		page->height_pixels = header->cupsHeight * scale + 0.5f;
	} else if (header->cupsBorderlessScalingFactor > 1.0f) {
		/* Keep physical paper_width/height; retain every already-enlarged pixel. */
		page->borderless_expansion = 1;
		page->width_points = header->PageSize[0];
		page->height_points = header->PageSize[1];
		page->width_pixels = header->cupsWidth;
		page->height_pixels = header->cupsHeight;
	}
	if (page->borderless_expansion) {
		page->row_bytes = page->width_pixels * RGB_CHANNELS;
		page->left_margin = 0;
		page->bottom_margin = 0;
		page->image_right = page->width_points;
		page->image_bottom = page->height_points;
	}
	return 0;
}
