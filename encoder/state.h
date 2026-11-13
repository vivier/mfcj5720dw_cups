/* Job-owned renderer state, native buffer addresses and callback interfaces. */
#ifndef BROTHER_LPR_STATE_H
#define BROTHER_LPR_STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct raster_band;
struct printer_state;

/* Byte counts preserve the numeric layouts used by the rendering modules. */
enum encoder_state_size {
	HALFTONE_NUMERIC_BYTES = 0x4ad0,
	SWATH_NUMERIC_BYTES = 0x6c,
	PRINTER_NUMERIC_BYTES = 0x81814,
};

enum encoder_state_limit {
	ENCODER_INK_CHANNELS = 4,
	PRINTER_SWATH_CAPACITY = 20,
	RASTER_ROW_POSITION_PAIRS = 26,
	RASTER_ROWS_PER_PAIR = 2,
};

/* Native views into the halftone values, error buffers and noise matrices. */
struct halftone_channel {
	int16_t *values;
	uintptr_t error_cursor;
	uintptr_t dither_matrix;
	uintptr_t photo_noise;
	uintptr_t diffusion_noise;
};

struct halftone_state {
	uint8_t numeric_state[HALFTONE_NUMERIC_BYTES];
	/* Halftone state uses K/C/M/Y order, distinct from the CMYK raster planes. */
	struct halftone_channel channels[ENCODER_INK_CHANNELS];
};

static inline struct halftone_channel *halftone_channels(uintptr_t address)
{
	return ((struct halftone_state *)address)->channels;
}

/*
 * Addresses may point inside a shared allocation; do not free these fields
 * individually: use the workspace allocation and cleanup helpers. Rendering
 * modules may adjust these addresses as a band progresses.
 */
struct printer_buffers {
	/* Input/scratch bands, halftone state and per-ink diffusion errors. */
	uintptr_t band;
	uintptr_t band_scratch;
	uintptr_t halftone;
	uintptr_t cyan_errors;
	uintptr_t magenta_errors;
	uintptr_t yellow_errors;
	uintptr_t black_errors;
	/* Color tables and pass-row scratch space. */
	uintptr_t rgb_curves;
	uintptr_t color_cube;
	uintptr_t pass_rows;
	/* Views into the image-enhancement workspace. */
	uintptr_t enhancement;
	uintptr_t histogram;
	uintptr_t channel_histograms;
	uintptr_t enhancement_curves;
	uintptr_t enhancement_history;
	/* Color calibration and transfer tables. */
	uintptr_t curve_workspace;
	uintptr_t calibration_curves;
	uintptr_t calibration_cube;
	uintptr_t transfer_curves;
	uintptr_t offset_curves;
	uintptr_t black_curve;
	uintptr_t color_workspace;
	/* Selected from a retained profile; returns a mask of rendered ink planes. */
	uint32_t (*render_band)(struct printer_state *printer, int first_row, uint32_t width,
				struct raster_band *band, uint32_t row_count, int first_column,
				int column_count);
	/* Paper-feed buffering, packed dots and row-correction workspaces. */
	uintptr_t feed_start;
	uintptr_t feed_cursor;
	uintptr_t raster_planes;
	uintptr_t correction_previous;
	uintptr_t correction_current;
	uintptr_t dither_matrix;
};

struct output_swath {
	/* Row counters and dot statistics retain their packed numeric layout. */
	uint8_t numeric_state[SWATH_NUMERIC_BYTES];
	/* Encoded data occupies [start, cursor) inside an owned raster workspace. */
	uintptr_t start;
	uintptr_t cursor;
};

/* Native working copy of a retained swath layout, extended with row buffers. */
struct raster_pass {
	/* Rendering mode and buffers used to prepare a single output row. */
	uint16_t mode;
	uint8_t compression;
	bool monochrome;
	int32_t mirrored;
	uint32_t plane_flags;
	uint8_t *encoded_row;
	uint8_t *scratch_row;
	uint32_t interlaced;
	uint16_t first_byte;
	uint16_t row_bytes;
	uint8_t *mirrored_row;
	uint8_t *raster_planes;
	uint16_t output_bytes;
	uint16_t plane_stride;
	void (*buffer_row)(struct printer_state *printer, struct raster_pass *pass);
	/* Borrowed schedules and nozzle geometry from the retained layout table. */
	uintptr_t row_schedule;
	uintptr_t feed_schedule;
	uintptr_t swath_order;
	uintptr_t padding_schedule;
	const int16_t *pass_lengths;
	uint16_t nozzle_rows;
	uint16_t feed_step;
	uint16_t row_step;
	uint16_t pass_count;
	int16_t first_row;
	uint16_t pass_rows;
	int32_t channel_offsets[ENCODER_INK_CHANNELS];
	uint16_t leading_rows;
	uint16_t trailing_rows;
	uint32_t flags;
	int32_t feed_rows;
	/* Derived encoding dimensions and correction state for the current batch. */
	uint32_t correction_flags;
	uint32_t encoded_pixels;
	uint32_t padded_row_bytes;
	uint32_t bits_per_symbol;
	uint32_t channel_count;
	uint32_t quantization_levels;
	uint32_t batch_rows;
	/* Consecutive scheduled rows, also read as pairs by the edge-mask selector. */
	uint16_t row_positions[RASTER_ROW_POSITION_PAIRS][RASTER_ROWS_PER_PAIR];
};

/* The caller supplies the numeric coder state and the indexed raster geometry. */
struct range_parameters {
	uint32_t *state;
	uint32_t pixels;
	uint32_t rows;
	uint32_t bits_per_symbol;
	uint32_t channels;
	uint32_t alphabet_size;
	uint32_t row_bytes;
};

struct range_encoder {
	struct range_parameters parameters;
	/* Integer results use nonzero for success/availability, unlike stream writes. */
	uint32_t (*capacity)(struct range_encoder *encoder, uint32_t *capacity);
	uint32_t (*set_length)(struct range_encoder *encoder, uint32_t *rows);
	uint32_t (*compress)(struct range_encoder *encoder, intptr_t input, uint8_t *output,
			     uint32_t *output_bytes);
	uint32_t (*available)(void);
	void (*finish)(void);
};

struct jpeg_encoder;

struct printer_state {
	/*
	 * Numeric offsets remain stable while each subsystem is typed. Converted
	 * pointer slots in this region are unused; native addresses live below.
	 */
	uint8_t numeric_state[PRINTER_NUMERIC_BYTES];
	struct printer_buffers buffers;
	struct output_swath swaths[PRINTER_SWATH_CAPACITY];
	struct jpeg_encoder *jpeg;
};

#endif
