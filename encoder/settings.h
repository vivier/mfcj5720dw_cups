/* Native option records, resolved job settings and raster-page bookkeeping. */
#ifndef BROTHER_LPR_SETTINGS_H
#define BROTHER_LPR_SETTINGS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

struct printer_state;

/* Storage capacities include the terminating NUL for text fields. */
enum configuration_capacity {
	CONFIGURATION_NAME_BYTES = 32,
	PRINTER_MODEL_NAME_BYTES = 32,
	JOB_PAPER_NAME_BYTES = 64,
	OPTION_ENABLE_CAPACITY = 50,
};

/* Stable indices shared with the CUPS-to-encoder option mapping. */
enum config_option_index {
	OPTION_MEDIA,
	OPTION_RESOLUTION,
	OPTION_BIDIRECTIONAL,
	OPTION_PAPER_SIZE,
	OPTION_PAPER_THICKNESS,
	OPTION_PAPER_SOURCE,
	OPTION_COLOR_MODE,
	OPTION_COLOR_MATCHING,
	OPTION_JPEG_PRIORITY,
	OPTION_HALFTONE,
	OPTION_COLOR_ENHANCEMENT,
	OPTION_SLOW_DRYING,
	OPTION_ALIGN_OTHER,
	OPTION_ALIGN_A4_LETTER,
	OPTION_ALIGN_A3_B4_LEDGER,
	OPTION_MIRROR,
	OPTION_DUPLEX_BINDING,
	OPTION_DUPLEX_DRYING,
	OPTION_BRIGHTNESS,
	OPTION_CONTRAST,
	OPTION_RED,
	OPTION_GREEN,
	OPTION_BLUE,
	OPTION_COUNT,
	/* Choice options precede the five numeric color adjustments. */
	CHOICE_OPTION_COUNT = OPTION_BRIGHTNESS,
};

/*
 * These are process-local records, not wire formats or serialized data tables.
 * Keep member order, widths and reserved storage when clarifying their names:
 * the encoder and differential fixtures share these native layouts.
 */
struct config_option {
	/* Internal option keyword and its legacy command-line alias. */
	char *name;
	char *alias;
	int16_t choice_count;
	uint16_t reserved;
	/* Parallel lookup arrays, indexed by selected rather than by numeric code. */
	char *const *labels;
	char *const *choices;
	const int16_t *codes;
	/* Validation accepts only 1; unvalidated lookup ignores these flags. */
	int16_t enabled[OPTION_ENABLE_CAPACITY];
	/* Numeric-option metadata and value, separate from named-choice selection. */
	int16_t maximum;
	int16_t minimum;
	int16_t value;
	/* Zero-based choice index; -1 means no choice is selected. */
	int16_t selected;
};

struct configuration {
	char name[CONFIGURATION_NAME_BYTES];
	char model[PRINTER_MODEL_NAME_BYTES];
	int option_count;
	struct config_option *options;
	/* Legacy constraint strings; the direct filter relies on PPD constraints. */
	int16_t default_constraints;
	int16_t constraint_count;
	char **constraints;
};

extern struct configuration lpr_configuration;

/* CUPS resolves choices and paper geometry before starting the encoder job. */
struct job_settings {
	/* Legacy input/path slots; the direct CUPS filter leaves these unset. */
	FILE *input;
	char *paper_path;
	char *config_path;
	char model[PRINTER_MODEL_NAME_BYTES];
	/* Configuration codes from options.c, not printer command values. */
	int16_t media;
	/* Quality choice code in the direct filter, not a resolution in dpi. */
	int16_t resolution;
	int16_t bidirectional;
	uint8_t reserved_options[4];
	int16_t borderless;
	int16_t paper_type;
	/* Rendered page dimensions in points, including borderless enlargement. */
	int16_t width_points;
	int16_t height_points;
	/* Input raster dimensions at the selected raster resolution. */
	int16_t width_pixels;
	int16_t height_pixels;
	/* Image bounds and margins are also expressed in points. */
	int16_t image_bottom;
	int16_t image_right;
	int16_t bottom_margin;
	int16_t left_margin;
	/* The input image is enlarged; use the corresponding edge placement. */
	uint16_t borderless_expansion;
	int row_bytes;
	/* Media handling, ink processing and line-alignment option codes. */
	int16_t thickness;
	int16_t paper_source;
	int16_t color_mode;
	int16_t color_matching;
	int16_t halftone;
	int16_t enhancement;
	int16_t slow_drying;
	int16_t align_other;
	int16_t align_a4_letter;
	int16_t align_a3_ledger;
	/* Signed color adjustments; the direct filter accepts -20 through +20. */
	int16_t brightness;
	int16_t contrast;
	int16_t red;
	int16_t green;
	int16_t blue;
	uint8_t reserved_color[4];
	/* Binding selection and duplex drying mode, respectively. */
	int16_t duplex;
	int16_t duplex_mode;
	uint16_t reserved_duplex;
	/* Input sample/pixel sizes in bytes; CUPS supplies expanded 8-bit RGB. */
	int sample_bytes;
	int pixel_bytes;
	char paper_name[JOB_PAPER_NAME_BYTES];
	/* Physical sheet dimensions in points, before borderless enlargement. */
	float paper_width;
	float paper_height;
};

/* A view of the prepared RGB band; its pointers do not own the pixel storage. */
struct raster_band {
	int32_t width;
	int32_t height;
	int32_t pixel_count;
	uint8_t *start;
	uint8_t *pixels;
	int32_t row_bytes;
	/* Renderer channel selector (3 or 5), not the RGB input pixel stride. */
	uint32_t channels;
	uint16_t flags;
	uint16_t reserved;
};

/* Derived encoder selections and per-page buffers, prepared from job_settings. */
struct page_state {
	/* Physical sheet dimensions; the renderer later exchanges the two axes. */
	int16_t paper_width_120dpi;
	int16_t paper_height_120dpi;
	int16_t borderless;
	int16_t width_pixels;
	/* Band heights and raster width are in pixels; indices are zero-based. */
	int16_t band_count;
	int16_t last_band_height;
	int16_t band_height;
	int16_t reserved_band;
	int16_t reserved_line;
	int16_t band_index;
	int16_t row_bytes;
	int16_t reserved_margins[4];
	int16_t color_mode;
	int16_t raster_width;
	int16_t reserved_width;
	/* Borrowed input pixels and scratch storage owned by the band workspace. */
	uint8_t *pixels;
	uint8_t *workspace;
	int16_t paper_source;
	int16_t reserved_source;
	/* Allocated and released together through memory.c's job-state helpers. */
	struct printer_state *printer;
	struct raster_band *band;
	uint16_t *device_options;
	void *reserved_state;
	/* JPEG priority, duplex binding, page subset and duplex drying mode. */
	int16_t jpeg_mode;
	int16_t duplex;
	int16_t page_set;
	int16_t duplex_mode;
	int16_t alignment_flags;
	char paper_name[JOB_PAPER_NAME_BYTES];
	/* Uses THICKNESS_* values, distinct from job_settings.thickness codes. */
	int16_t paper_thickness;
	int16_t first_page;
	int16_t rotated_row_bytes;
};

#endif
