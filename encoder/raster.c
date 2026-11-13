/* Crop, rotate and feed RGB bands to the selected halftone renderer. */
#include "raster.h"
#include "color.h"
#include "data.h"
#include "dither.h"
#include "paper.h"
#include "printhead.h"
#include "protocol.h"
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

enum raster_constants {
	RGB_BYTES = 3,
	WHITE_SAMPLE = 255,
	BAND_HAS_INK = 1,
	HALFTONE_DITHER = 1,
	HEAD_CORRECTION_ACTIVE = 1,
	HEAD_ROW_MAP_SIZE = 40,
	BAND_RGB_CHANNELS = 3,
	BAND_COLOR_CHANNELS = 5,
	PLANE_COUNT = 3,
	SMALL_PLANE_BYTES = 0x23200,
	DEFAULT_PLANE_BYTES = 0x46400,
	LARGE_PLANE_BYTES = 0xafa00,
	PASS_ROWS_OFFSET = 0x6960,
	RASTER_PLANES_OFFSET = 0xd2c0,
	FIXED_WORKSPACE_BYTES = 0xe430,
};

enum raster_pass_stage {
	PASS_CONTINUE,
	PASS_START,
	PASS_FLUSH,
};

/* Private numeric view; native addresses remain in printer_buffers. */
struct raster_settings {
	uint8_t reserved_before_landscape[0x878 - 0x0];
	int32_t landscape;
	uint8_t reserved_before_raster_mode[0x88a - 0x87c];
	uint16_t raster_mode;
	uint8_t reserved_before_band_status[0x894 - 0x88c];
	uint8_t band_status;
	uint8_t reserved_before_orientation[0x89c - 0x895];
	uint8_t orientation;
	uint8_t ink_flags;
	uint8_t reserved_before_band_index[0x8a4 - 0x89e];
	uint16_t band_index;
	uint16_t band_height;
	uint16_t last_band_height;
	uint8_t reserved_before_rendered_planes[0x8c8 - 0x8aa];
	uint16_t rendered_planes;
	int16_t first_ink_column;
	int16_t end_ink_column;
	uint8_t reserved_before_reserved_pass_counter[0x8d8 - 0x8ce];
	uint32_t reserved_pass_counter;
	uint8_t reserved_before_halftone_method[0x8e6 - 0x8dc];
	uint8_t halftone_method;
	uint8_t reserved_before_pass_ready[0x8e8 - 0x8e7];
	uint16_t pass_ready;
	uint8_t reserved_before_feed_position[0x8ec - 0x8ea];
	uint32_t feed_position;
	uint8_t reserved_before_pass_stage[0x1160 - 0x8f0];
	uint16_t pass_stage;
	uint8_t swath_cursor;
	uint8_t swath_phase;
	uint8_t color_pass;
	uint8_t reserved_before_feed_schedule_index[0x1166 - 0x1165];
	uint16_t feed_schedule_index;
	uint32_t pass_initialized;
	uint8_t reserved_before_mask_phase[0x11f0 - 0x116c];
	uint8_t mask_phase;
	uint8_t reserved_before_band_count[0x11f2 - 0x11f1];
	uint16_t band_count;
	uint8_t reserved_before_image_start_x[0x11fc - 0x11f4];
	int32_t image_start_x;
	int32_t image_start_y;
	int32_t image_end_x;
	int32_t image_end_y;
	uint8_t reserved_before_reserved_band_counter[0x1228 - 0x120c];
	uint32_t reserved_band_counter;
	uint8_t reserved_before_row_correction_enabled[0x12fc - 0x122c];
	int32_t row_correction_enabled;
	uint8_t reserved_before_band_first_row[0x817c8 - 0x1300];
	int32_t band_first_row;
};

struct raster_head_state {
	uint8_t reserved[0x4a6a];
	uint8_t flags;
	uint8_t reserved_alignment;
	uint16_t row_map[HEAD_ROW_MAP_SIZE];
};

uint32_t clear_paper_margins(struct job_settings *settings, struct page_state *page)
{
	int16_t paper = identify_custom_paper(page);
	int16_t top_rows = 0, bottom_rows = 0;
	double top, bottom;

	if (paper < 0)
		return 1;
	top = lpr_custom_paper_records[paper].horizontal_margin;
	bottom = lpr_custom_paper_records[paper].vertical_margin;
	if (top > 0)
		top_rows = (int16_t)truncl((double)settings->resolution * top);
	if (bottom > 0)
		bottom_rows = (int16_t)truncl((double)settings->resolution * bottom);
	whiten_band_edges((uint8_t *)lpr_input_band, top_rows, bottom_rows, settings->height_pixels,
			  settings->row_bytes);
	return 1;
}

uint32_t whiten_band_edges(uint8_t *pixels, short top_rows, short bottom_rows, short page_height,
			   int row_bytes)
{
	/* The page setup supplies nonnegative margins contained in the image. */
	memset(pixels, WHITE_SAMPLE, (uint32_t)(top_rows * row_bytes));
	memset(pixels + (int16_t)(page_height - bottom_rows) * row_bytes, WHITE_SAMPLE,
	       (uint32_t)(bottom_rows * row_bytes));
	return 0;
}

uint32_t rotate_envelope_band(struct job_settings *settings, struct page_state *page)
{
	const uint8_t *source = (const uint8_t *)lpr_input_band;
	int rows = page->band_height;
	int row_bytes = settings->row_bytes;
	size_t length = rows * row_bytes;
	uint8_t *rotated;
	int row, column;

	if (!needs_envelope_rotation(settings))
		return 0;
	rotated = malloc(length);
	if (!rotated)
		return 0;
	memset(rotated, 0, length);
	fprintf(stderr, "DEBUG: cupsBitsPerPixel = %u\n", (uint32_t)settings->pixel_bytes);
	fprintf(stderr, "DEBUG: bytesPerPage = %zu\n", length);
	fprintf(stderr, "DEBUG: cupsBytesPerLine = %u\n", (uint32_t)row_bytes);
	/* Input pixels are packed RGB; reverse them into destination row strides. */
	for (row = 0; row < rows; row++) {
		for (column = 0; column < settings->width_pixels; column++) {
			size_t destination = (rows - row) * row_bytes - (column + 1) * RGB_BYTES;

			memcpy(rotated + destination, source, RGB_BYTES);
			source += RGB_BYTES;
		}
	}
	memcpy(lpr_input_band, rotated, length);
	free(rotated);
	return 0;
}

uint32_t rotate_landscape_band(struct job_settings *settings, struct page_state *page)
{
	const uint8_t *source = (const uint8_t *)lpr_input_band;
	int rows = page->band_height;
	int height = settings->height_pixels;
	size_t length = rows * settings->row_bytes;
	uint8_t *rotated;
	int row, column;

	if (!needs_landscape_rotation(settings))
		return 0;
	rotated = malloc(length);
	if (!rotated)
		return 0;
	memset(rotated, 0, length);
	/* Whole-page clockwise rotation: (x, y) becomes (height - 1 - y, x). */
	for (row = 0; row < rows; row++) {
		for (column = 0; column < settings->width_pixels; column++) {
			size_t destination = ((column + 1) * height - row - 1) * RGB_BYTES;

			memcpy(rotated + destination, source, RGB_BYTES);
			source += RGB_BYTES;
		}
	}
	memcpy(lpr_input_band, rotated, length);
	free(rotated);
	return 0;
}

static size_t raster_plane_bytes(uint16_t mode)
{
	switch (mode & RASTER_PROFILE_MASK) {
	case RASTER_PROFILE_0500:
	case RASTER_PROFILE_0510:
	case RASTER_PROFILE_0600:
	case RASTER_PROFILE_0610:
		return SMALL_PLANE_BYTES;
	case RASTER_PROFILE_0700:
	case RASTER_PROFILE_0710:
		return LARGE_PLANE_BYTES;
	default:
		return DEFAULT_PLANE_BYTES;
	}
}

bool allocate_band_workspace(struct page_state *page, uint8_t *pixels)
{
	const struct raster_settings *settings = (void *)page->printer->numeric_state;
	struct printer_buffers *buffers = &page->printer->buffers;
	size_t plane_bytes = PLANE_COUNT * raster_plane_bytes(settings->raster_mode);
	size_t bytes = plane_bytes + FIXED_WORKSPACE_BYTES;
	uint8_t *workspace;

	page->pixels = pixels;
	workspace = malloc(bytes);
	lpr_band_workspace = workspace;
	if (!workspace)
		return false;
	memset(workspace, 0, bytes);
	page->workspace = workspace;
	buffers->band = (uintptr_t)workspace;
	buffers->pass_rows = (uintptr_t)(workspace + PASS_ROWS_OFFSET);
	buffers->raster_planes = (uintptr_t)(workspace + RASTER_PLANES_OFFSET);
	buffers->band_scratch = (uintptr_t)(workspace + RASTER_PLANES_OFFSET + plane_bytes);
	return true;
}

void free_band_workspace(void)
{
	free(lpr_band_workspace);
	lpr_band_workspace = NULL;
}

void prepare_band_descriptor(struct page_state *page)
{
	struct raster_settings *settings = (void *)page->printer->numeric_state;
	struct raster_band *band = page->band;
	int width = page->raster_width;
	int height = page->band_height;

	settings->band_status = 0;
	settings->band_count = page->band_count;
	settings->band_index = page->band_index;
	settings->band_height = page->band_height;
	settings->image_start_x = 0;
	settings->image_start_y = 0;
	if ((uint32_t)settings->band_index == settings->band_count - 1U)
		height = page->last_band_height;
	if (settings->landscape == 1) {
		band->width = height;
		band->height = width;
		band->row_bytes = page->rotated_row_bytes;
		settings->last_band_height = width;
	} else {
		band->width = width;
		band->height = height;
		band->row_bytes = page->row_bytes;
		settings->last_band_height = page->last_band_height;
	}
	settings->image_end_x = band->width;
	settings->image_end_y = band->height;
	band->pixel_count = (uint32_t)band->height * (uint32_t)band->width;
	band->flags = 0;
	band->pixels = page->pixels;
	band->start = page->pixels;
	band->channels = page->color_mode ? BAND_COLOR_CHANNELS : BAND_RGB_CHANNELS;
}

static unsigned int rows_per_raster_pass(uint16_t mode, uint8_t orientation)
{
	uint16_t profile = mode & RASTER_PROFILE_MASK;

	if (profile == RASTER_PROFILE_0201 || profile == RASTER_PROFILE_0211)
		return 2;
	/* Reverse traversal otherwise renders one source row at a time. */
	if (orientation & ORIENTATION_REVERSED)
		return 1;
	switch (profile) {
	case RASTER_PROFILE_0205:
	case RASTER_PROFILE_0215:
		return 1;
	case RASTER_PROFILE_0500:
	case RASTER_PROFILE_0508:
	case RASTER_PROFILE_0510:
	case RASTER_PROFILE_0518:
	case RASTER_PROFILE_0600:
	case RASTER_PROFILE_0608:
	case RASTER_PROFILE_0610:
	case RASTER_PROFILE_0618:
	case RASTER_PROFILE_0700:
	case RASTER_PROFILE_0710:
		return 4;
	case RASTER_PROFILE_010F:
		return 8;
	default:
		return 16;
	}
}

static void start_raster_page(struct printer_state *printer, struct raster_settings *settings)
{
	settings->color_pass = (settings->raster_mode & RASTER_CHANNEL_MASK) != RASTER_MONOCHROME;
	settings->pass_ready = 0;
	settings->feed_position = 0;
	settings->feed_schedule_index = 0;
	settings->swath_cursor = 0;
	settings->swath_phase = 0;
	settings->pass_stage = PASS_START;
	settings->pass_initialized = 1;
	initialize_swath_buffers(printer);
	settings->rendered_planes = 0;
	settings->reserved_pass_counter = 0;
	settings->mask_phase = 0;
	settings->reserved_band_counter = 0;
	settings->band_index = 0;
	settings->band_first_row = 0;
}

void render_halftone_band(struct printer_state *printer, struct raster_band *band)
{
	struct raster_settings *settings = (void *)printer->numeric_state;
	struct printer_buffers *buffers = &printer->buffers;
	int first_column, end_column = 0;
	int height = band->height;
	uint32_t width = band->width;
	unsigned int batch_rows;
	int first_row, row_count;
	bool density_ramp;

	if (settings->band_index == 0)
		start_raster_page(printer, settings);
	settings->ink_flags = 0;
	first_column = find_first_ink_column(printer, band);
	if (settings->ink_flags & BAND_HAS_INK)
		end_column = find_last_ink_column(printer, band);
	settings->first_ink_column = first_column;
	settings->end_ink_column = end_column;
	prepare_halftone_band(printer, band);
	update_color_tables(printer);
	configure_row_correction(printer);
	if (settings->halftone_method == HALFTONE_DITHER) {
		select_dither_matrix(printer);
		assign_channel_dither_matrices(printer);
	}
	select_head_density_calibration(printer);
	apply_head_density_calibration(printer);
	set_dither_density_factors(printer);
	load_printhead_parameters(printer);
	batch_rows = rows_per_raster_pass(settings->raster_mode, settings->orientation);
	density_ramp = needs_borderless_density_ramp(printer);
	clear_halftone_planes(printer);

	for (first_row = 0; first_row < height; first_row += row_count) {
		uint32_t planes = 0;
		int source_row;

		row_count = height - first_row;
		if ((unsigned int)row_count > batch_rows)
			row_count = batch_rows;
		source_row = first_row;
		if (settings->orientation & ORIENTATION_REVERSED)
			source_row = height - row_count - first_row;
		if (settings->ink_flags & BAND_HAS_INK) {
			struct raster_head_state *head = (void *)buffers->halftone;

			if ((head->flags & HEAD_CORRECTION_ACTIVE) ||
			    settings->row_correction_enabled)
				build_row_schedule(printer, (intptr_t)head->row_map, row_count);
			select_borderless_density_ramp(printer, first_row, row_count, density_ramp);
			planes = buffers->render_band(
			    printer, source_row, width, band, row_count, first_column,
			    end_column - first_column);
		}
		process_raster_pass(printer, row_count, band, planes);
		settings->pass_stage = PASS_CONTINUE;
		settings->pass_ready = 1;
		if (planes) {
			settings->rendered_planes |= (uint16_t)planes;
			if (band->channels == BAND_COLOR_CHANNELS &&
			    settings->halftone_method == HALFTONE_DITHER &&
			    (settings->raster_mode & RASTER_RESOLUTION_FAMILY_MASK) <
				RASTER_PROFILE_0500)
				clear_halftone_planes(printer);
		}
		batch_rows = row_count;
	}
	if ((uint32_t)settings->band_index == settings->band_count - 1U) {
		settings->pass_stage = PASS_FLUSH;
		process_raster_pass(printer, 0, band, 1);
		settings->pass_ready = 0;
		settings->pass_stage = PASS_START;
	}
	settings->band_first_row = (uint32_t)settings->band_first_row + (uint32_t)height;
}

void clear_halftone_planes(struct printer_state *printer)
{
	const struct raster_settings *settings = (void *)printer->numeric_state;

	memset((void *)printer->buffers.raster_planes, 0,
	       PLANE_COUNT * raster_plane_bytes(settings->raster_mode));
}

static bool rgb_is_white(const uint8_t *pixel)
{
	return pixel[0] == WHITE_SAMPLE && pixel[1] == WHITE_SAMPLE && pixel[2] == WHITE_SAMPLE;
}

static void set_band_has_ink(struct raster_settings *settings, bool found)
{
	if (found)
		settings->ink_flags |= BAND_HAS_INK;
	else
		settings->ink_flags &= ~BAND_HAS_INK;
}

int find_first_ink_column(struct printer_state *printer, struct raster_band *band)
{
	struct raster_settings *settings = (void *)printer->numeric_state;
	int column, row;

	/* Keep the existing exclusion of the final row and final column. */
	for (column = 0; column < band->width - 1; column++) {
		for (row = 0; row < band->height - 1; row++) {
			const uint8_t *pixel =
			    band->pixels + row * band->row_bytes + column * RGB_BYTES;

			if (!rgb_is_white(pixel)) {
				set_band_has_ink(settings, true);
				return column;
			}
		}
	}
	set_band_has_ink(settings, false);
	return band->width > 0 ? band->width : 0;
}

int find_last_ink_column(struct printer_state *printer, struct raster_band *band)
{
	struct raster_settings *settings = (void *)printer->numeric_state;
	size_t bytes = (size_t)band->height * band->row_bytes;
	int column, row;

	/*
	 * The result is an exclusive bound. Preserve the original one-byte
	 * lookahead except at the final-row penultimate column. Read RGB as
	 * bytes so unaligned pixels are valid. If lookahead leaves the band,
	 * conservatively retain the column: narrowing the rendered region can
	 * change error diffusion, even when the omitted pixels are white.
	 */
	for (column = band->width; column > 0; column--) {
		for (row = 0; row < band->height; row++) {
			size_t offset = (size_t)row * band->row_bytes + (column - 1) * RGB_BYTES;
			const uint8_t *pixel = band->pixels + offset;
			bool has_ink = !rgb_is_white(pixel);

			if (row != band->height - 1 || column != band->width - 1) {
				if (offset + RGB_BYTES >= bytes || pixel[RGB_BYTES] != WHITE_SAMPLE)
					has_ink = true;
			}
			if (has_ink) {
				set_band_has_ink(settings, true);
				return column;
			}
		}
	}
	set_band_has_ink(settings, false);
	return band->width < 0 ? band->width : 0;
}

bool is_plain_normal_raster(struct printer_state *printer)
{
	const struct raster_settings *settings = (void *)printer->numeric_state;

	return (settings->raster_mode & RASTER_PROFILE_MASK) == RASTER_PROFILE_0201;
}
