/* Own the printer, swath and rendering workspaces for a single job. */
#include "memory.h"
#include "data.h"
#include "jpeg.h"
#include "protocol.h"
#include "settings.h"
#include "state.h"
#include "swath.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum workspace_sizes {
	DEVICE_OPTIONS_BYTES = 0x36,
	SWATH_BYTES = 0x73cd80,
	RGB_CURVES_BYTES = 0x51d0,
	ERROR_CHANNEL_BYTES = 0x16e30,
	CURVE_WORKSPACE_BYTES = 0x4800,
	NOISE_CHANNEL_BYTES = 0x20000,
	HALFTONE_CHANNEL_BYTES = 0x78,
	SHARED_RESERVED_BYTES = 0x80000,
	ENHANCEMENT_BYTES = 0x3a8a0,
	COLOR_WORKSPACE_BYTES = 0xab88,
	FEED_BUFFER_BYTES = 0x15b6980,
	CORRECTION_PLANE_BYTES = 0x2db40,
};

enum workspace_parameters {
	FEED_BUFFER_ENABLED = 1,
	STATE_LEGACY_BUFFER_LIMIT = 0x1190,
	LEGACY_BUFFER_LIMIT = 0x3cf4,
	ENHANCEMENT_HISTOGRAM_OFFSET = 0x354,
	ENHANCEMENT_CHANNEL_HISTOGRAMS_OFFSET = 0x454,
	ENHANCEMENT_CURVES_OFFSET = 0x1354,
	ENHANCEMENT_HISTORY_OFFSET = 0x1654,
};

/*
 * The shared area follows all swath buffers. Preserve its layout, including
 * reserved space: other rendering modules still rely on these table sizes.
 * Error arrays use CMYK order; halftone channel state uses its own order.
 */
struct shared_raster_workspace {
	uint8_t rgb_curves[RGB_CURVES_BYTES];
	int16_t cyan_errors[ERROR_CHANNEL_BYTES / sizeof(int16_t)];
	int16_t magenta_errors[ERROR_CHANNEL_BYTES / sizeof(int16_t)];
	int16_t yellow_errors[ERROR_CHANNEL_BYTES / sizeof(int16_t)];
	int16_t black_errors[ERROR_CHANNEL_BYTES / sizeof(int16_t)];
	uint8_t curves[CURVE_WORKSPACE_BYTES];
	uint8_t photo_noise[COLOR_CHANNELS][NOISE_CHANNEL_BYTES];
	uint8_t diffusion_noise[COLOR_CHANNELS][NOISE_CHANNEL_BYTES];
	uint8_t reserved[SHARED_RESERVED_BYTES];
};

static void assign_raster_buffers(struct printer_state *printer, unsigned int swath_count,
				 struct halftone_state *halftone)
{
	struct printer_buffers *buffers = &printer->buffers;
	uint8_t *workspace = lpr_swath_workspace;
	struct shared_raster_workspace *shared =
		(void *)(workspace + swath_count * SWATH_BYTES);
	unsigned int index;

	for (index = 0; index < swath_count; index++) {
		struct output_swath *swath = &printer->swaths[index];

		swath->start = (uintptr_t)(workspace + index * SWATH_BYTES);
		swath->cursor = swath->start;
	}
	buffers->cyan_errors = (uintptr_t)shared->cyan_errors;
	buffers->magenta_errors = (uintptr_t)shared->magenta_errors;
	buffers->yellow_errors = (uintptr_t)shared->yellow_errors;
	buffers->black_errors = (uintptr_t)shared->black_errors;
	buffers->rgb_curves = (uintptr_t)shared->rgb_curves;
	buffers->curve_workspace = (uintptr_t)shared->curves;
	buffers->halftone = (uintptr_t)halftone;
	for (index = 0; index < COLOR_CHANNELS; index++) {
		halftone->channels[index].values =
			(void *)(halftone->numeric_state + index * HALFTONE_CHANNEL_BYTES);
		halftone->channels[index].photo_noise = (uintptr_t)shared->photo_noise[index];
		halftone->channels[index].diffusion_noise = (uintptr_t)shared->diffusion_noise[index];
	}
	/* Retain this numeric setting even though current rendering code does not read it. */
	memcpy(printer->numeric_state + STATE_LEGACY_BUFFER_LIMIT,
	       &(uint16_t){LEGACY_BUFFER_LIMIT}, sizeof(uint16_t));
}

static void assign_enhancement_buffers(struct printer_buffers *buffers)
{
	uintptr_t workspace = (uintptr_t)lpr_enhancement_workspace;

	buffers->enhancement = workspace;
	buffers->histogram = workspace + ENHANCEMENT_HISTOGRAM_OFFSET;
	buffers->channel_histograms = workspace + ENHANCEMENT_CHANNEL_HISTOGRAMS_OFFSET;
	buffers->enhancement_curves = workspace + ENHANCEMENT_CURVES_OFFSET;
	buffers->enhancement_history = workspace + ENHANCEMENT_HISTORY_OFFSET;
	/* The legacy histogram regions overlap; moving them would change enhancement. */
}

uint32_t allocate_raster_buffers(struct page_state *page)
{
	struct printer_state *printer = page->printer;
	struct halftone_state *halftone;
	unsigned int swath_count;
	size_t bytes;

	if (lpr_driver_family == DRIVER_FAMILY_UNKNOWN) {
		fputs("ERROR: Unknown printer model.\n", stderr);
		return 1;
	}
	swath_count = get_swath_count(printer);
	if (!swath_count || swath_count > sizeof(printer->swaths) / sizeof(printer->swaths[0])) {
		fputs("ERROR: Invalid raster swath count.\n", stderr);
		return 1;
	}
	bytes = (size_t)swath_count * SWATH_BYTES + sizeof(struct shared_raster_workspace);
	lpr_swath_workspace = calloc(1, bytes);
	if (!lpr_swath_workspace)
		goto allocation_failed;
	halftone = calloc(1, sizeof(*halftone));
	if (!halftone)
		goto allocation_failed;
	assign_raster_buffers(printer, swath_count, halftone);

	lpr_enhancement_workspace = calloc(1, ENHANCEMENT_BYTES);
	if (!lpr_enhancement_workspace)
		goto allocation_failed;
	assign_enhancement_buffers(&printer->buffers);
	lpr_color_workspace = calloc(1, COLOR_WORKSPACE_BYTES);
	if (!lpr_color_workspace)
		goto allocation_failed;
	printer->buffers.color_workspace = (uintptr_t)lpr_color_workspace;
	if (!allocate_feed_buffer(printer) || !allocate_row_correction_buffers(printer))
		goto allocation_failed;
	return 0;

allocation_failed:
	/* All successful allocations remain owned until free_printer_state(). */
	fputs("ERROR: Cannot allocate raster workspaces.\n", stderr);
	return 1;
}

uint32_t allocate_printer_state(struct page_state *page)
{
	if (lpr_driver_family == DRIVER_FAMILY_UNKNOWN) {
		fputs("ERROR: Unknown printer model.\n", stderr);
		return 1;
	}
	if (lpr_driver_family == DRIVER_FAMILY_CALLER_OWNED)
		return 0;

	lpr_device_options = malloc(DEVICE_OPTIONS_BYTES);
	if (!lpr_device_options)
		return 1;
	page->device_options = lpr_device_options;
	lpr_printer_state = calloc(1, sizeof(struct printer_state));
	if (!lpr_printer_state)
		return 1;
	page->printer = lpr_printer_state;
	lpr_band_descriptor = malloc(sizeof(struct raster_band));
	if (!lpr_band_descriptor)
		return 1;
	page->band = lpr_band_descriptor;
	return 0;
}

void free_printer_state(void)
{
	struct printer_state *printer = lpr_printer_state;

	if (lpr_driver_family == DRIVER_FAMILY_CALLER_OWNED) {
		/* Preserve the legacy non-owning branch's two cleared workspace slots. */
		lpr_swath_workspace = NULL;
		lpr_color_workspace = NULL;
		return;
	}
	if (printer) {
		free_jpeg_encoder(printer);
		free((void *)printer->buffers.halftone);
	}
	free_feed_buffer();
	free_row_correction_buffers();
	free(lpr_device_options);
	free(lpr_printer_state);
	free(lpr_band_descriptor);
	free(lpr_color_workspace);
	free(lpr_swath_workspace);
	free(lpr_enhancement_workspace);
	/* Clear every owner so partial initialization and repeated cleanup are safe. */
	lpr_device_options = NULL;
	lpr_printer_state = NULL;
	lpr_band_descriptor = NULL;
	lpr_color_workspace = NULL;
	lpr_swath_workspace = NULL;
	lpr_enhancement_workspace = NULL;
}

bool allocate_feed_buffer(struct printer_state *printer)
{
	struct printer_buffers *buffers = &printer->buffers;

	free_feed_buffer();
	buffers->feed_start = 0;
	buffers->feed_cursor = 0;
	if (!(printer->numeric_state[STATE_FEED_COMPLETION_FLAGS] & FEED_BUFFER_ENABLED))
		return true;
	lpr_feed_workspace = calloc(1, FEED_BUFFER_BYTES);
	if (!lpr_feed_workspace)
		return false;
	buffers->feed_start = (uintptr_t)lpr_feed_workspace;
	buffers->feed_cursor = buffers->feed_start;
	return true;
}

void free_feed_buffer(void)
{
	struct printer_state *printer = lpr_printer_state;

	free(lpr_feed_workspace);
	lpr_feed_workspace = NULL;
	if (printer) {
		printer->buffers.feed_start = 0;
		printer->buffers.feed_cursor = 0;
	}
}

bool allocate_row_correction_buffers(struct printer_state *printer)
{
	struct printer_buffers *buffers = &printer->buffers;

	free_row_correction_buffers();
	buffers->correction_previous = 0;
	buffers->correction_current = 0;
	lpr_correction_workspace = calloc(2, CORRECTION_PLANE_BYTES);
	if (!lpr_correction_workspace)
		return false;
	buffers->correction_previous = (uintptr_t)lpr_correction_workspace;
	buffers->correction_current = buffers->correction_previous + CORRECTION_PLANE_BYTES;
	return true;
}

void free_row_correction_buffers(void)
{
	struct printer_state *printer = lpr_printer_state;

	free(lpr_correction_workspace);
	lpr_correction_workspace = NULL;
	if (printer) {
		printer->buffers.correction_previous = 0;
		printer->buffers.correction_current = 0;
	}
}
