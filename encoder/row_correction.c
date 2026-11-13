/* Measure CMY coverage to select the printing direction of a swath. */
#include "row_correction.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum coverage_geometry {
	COVERAGE_COLUMNS = 512,
	COVERAGE_ROWS = 32,
	CMY_CHANNELS = 3,
	INK_MAXIMUM = 1023,
	CMY_MAXIMUM = CMY_CHANNELS * INK_MAXIMUM,
};

enum correction_decision {
	CORRECTION_NOT_NEEDED,
	CORRECTION_NEEDED,
	CORRECTION_PENDING,
};

struct ink_coverage {
	int32_t cyan;
	int32_t magenta;
	int32_t yellow;
	int32_t black;
};

struct coverage_cell {
	struct ink_coverage light;
	struct ink_coverage dark;
};

/* The shared printer state aligns these doubles to four bytes, not eight. */
struct coverage_limits {
	double edge_chroma;
	double edge_dark_chroma;
	double interior_chroma;
	double interior_dark_chroma;
} __attribute__((packed));

/* Private view of the numeric state shared with the raster and BCM modules. */
struct row_correction_state {
	int32_t enabled;
	/* Column-major storage, with a fixed row stride regardless of row_count. */
	struct coverage_cell cells[COVERAGE_COLUMNS * COVERAGE_ROWS];
	uint16_t column_span;
	uint16_t last_column_span;
	uint16_t row_span;
	uint16_t last_row_span;
	uint16_t column_count;
	uint16_t row_count;
	struct coverage_limits limits;
	int32_t page_rows;
	int32_t black_threshold;
	uint32_t expanded_width;
	uint16_t current_row;
	uint16_t decision;
	uint16_t rows_to_evaluate;
	uint16_t first_swath_row;
	uint16_t row_cell;
	uint8_t has_calibration;
};

/*
 * Geometry supplied by the renderer/BCM profile must have positive dimensions.
 * Keep the original 16-bit counts and span-based remainder calculation: rounding
 * the count up or using count as the divisor would change the final cell area.
 */
static uint16_t coverage_cell_count(int length, uint16_t span, uint16_t maximum)
{
	uint16_t count = length / span;

	if (count > maximum)
		count = maximum;
	if (length < span)
		count = 1;
	return count;
}

void initialize_row_correction(int *correction, uint16_t column_span, uint16_t row_span,
			       uint64_t edge_chroma_bits, int edge_dark_low, int edge_dark_high,
			       uint64_t interior_chroma_bits, uint64_t interior_dark_bits,
			       int page_rows, int black_threshold, int page_width)
{
	struct row_correction_state *state = (void *)correction;
	uint64_t edge_dark_bits = (uint32_t)edge_dark_low |
				 ((uint64_t)(uint32_t)edge_dark_high << 32);

	if (!state->enabled)
		return;

	if (!state->has_calibration) {
		state->column_span = column_span;
		state->row_span = row_span;
		state->page_rows = page_rows;
		state->black_threshold = black_threshold;
		/* Retain the calling convention and exact calibration bit patterns. */
		memcpy(&state->limits.edge_chroma, &edge_chroma_bits, sizeof(double));
		memcpy(&state->limits.edge_dark_chroma, &edge_dark_bits, sizeof(double));
		memcpy(&state->limits.interior_chroma, &interior_chroma_bits, sizeof(double));
		memcpy(&state->limits.interior_dark_chroma, &interior_dark_bits, sizeof(double));
	}

	state->column_count = coverage_cell_count(page_width, state->column_span, COVERAGE_COLUMNS);
	state->column_span = page_width / state->column_count;
	state->last_column_span = state->column_span + page_width % state->column_span;
	state->row_count = coverage_cell_count(state->page_rows, state->row_span, COVERAGE_ROWS);
	state->row_span = state->page_rows / state->row_count;
	state->last_row_span = state->row_span + state->page_rows % state->row_span;
	state->current_row = 0;
	state->decision = CORRECTION_PENDING;
	memset(state->cells, 0, sizeof(state->cells));
}

void reset_correction_row(intptr_t correction)
{
	struct row_correction_state *state = (void *)correction;

	state->current_row = 0;
}

void advance_correction_row(intptr_t correction)
{
	struct row_correction_state *state = (void *)correction;
	uint16_t row;

	state->current_row++;
	row = state->current_row / state->row_span;
	/* Preserve the inclusive limit used for trailing partial cells. */
	if (row > state->row_count)
		row = state->row_count;
	state->row_cell = row;
}

/* Accumulators use the renderer's modulo-2^32 arithmetic, including overflow. */
static int32_t add_ink(int32_t sum, int16_t ink)
{
	return (uint32_t)sum + (uint32_t)ink;
}

void accumulate_ink_coverage(int *correction, int cyan_magenta, int yellow_black, uint32_t reserved,
			     uint32_t column)
{
	struct row_correction_state *state = (void *)correction;
	int16_t cyan = cyan_magenta;
	int16_t magenta = (uint32_t)cyan_magenta >> 16;
	int16_t yellow = yellow_black;
	int16_t black = (uint32_t)yellow_black >> 16;
	struct coverage_cell *cell;
	struct ink_coverage *sums;
	uint32_t column_cell;

	(void)reserved;
	/* Pure black pixels do not contribute to the chroma coverage decision. */
	if (!state->enabled || (!cyan && !magenta && !yellow))
		return;

	column_cell = column / state->column_span;
	if ((int32_t)column_cell > state->column_count)
		column_cell = state->column_count;
	/*
	 * As for rows, keep the inclusive column limit and fixed storage stride.
	 * The raster scheduler must keep the resulting index inside the grid.
	 */
	cell = &state->cells[column_cell * COVERAGE_ROWS + state->row_cell];
	sums = black > state->black_threshold ? &cell->dark : &cell->light;
	sums->cyan = add_ink(sums->cyan, cyan);
	sums->magenta = add_ink(sums->magenta, magenta);
	sums->yellow = add_ink(sums->yellow, yellow);
	sums->black = add_ink(sums->black, black);
}

static uint32_t chroma_sum(const struct ink_coverage *ink)
{
	return (uint32_t)ink->cyan + (uint32_t)ink->magenta + (uint32_t)ink->yellow;
}

static bool cell_needs_correction(const struct row_correction_state *state,
				  const struct coverage_cell *cell, unsigned int row,
				  uint32_t width, uint32_t height)
{
	uint32_t dark_sum = chroma_sum(&cell->dark);
	uint32_t total_sum = chroma_sum(&cell->light) + dark_sum;
	int32_t capacity = width * height * CMY_MAXIMUM;
	double total_coverage = 0.0;
	double dark_coverage = 0.0;

	/* Store ratios as doubles before comparing, including in x87 builds. */
	if (total_sum)
		total_coverage = (double)total_sum / (double)capacity;
	if (dark_sum)
		dark_coverage = (double)dark_sum / (double)capacity;

	/*
	 * Only the first and last configured rows use the edge thresholds.
	 * A partial swath's last row can still be an interior row. Preserve the
	 * distinct ordered comparisons, including their behavior with NaNs.
	 */
	if (row == 0 || (int)row == state->row_count - 1) {
		if (total_coverage <= state->limits.edge_chroma)
			return state->limits.edge_dark_chroma < dark_coverage;
		return true;
	}
	return state->limits.interior_chroma < total_coverage ||
	       state->limits.interior_dark_chroma < dark_coverage;
}

int evaluate_row_correction(int *correction)
{
	struct row_correction_state *state = (void *)correction;
	uint32_t rows, last_height, column, row;
	bool needed = false;

	if (!state->enabled)
		return 0;

	rows = state->rows_to_evaluate / state->row_span;
	last_height = state->rows_to_evaluate;
	if (last_height == (uint32_t)state->page_rows)
		last_height = state->last_row_span;
	else if (last_height < state->row_span)
		rows = 1;
	else
		last_height -= state->row_span * (rows - 1);

	for (column = 0; column < state->column_count && !needed; column++) {
		uint32_t width = state->column_span;

		if (column == (uint32_t)state->column_count - 1)
			width = state->last_column_span;
		for (row = 0; row < rows && !needed; row++) {
			uint32_t height = state->row_span;
			const struct coverage_cell *cell;

			if (row == rows - 1)
				height = last_height;
			cell = &state->cells[column * COVERAGE_ROWS + row];
			needed = cell_needs_correction(state, cell, row, width, height);
		}
	}

	state->current_row = 0;
	memset(state->cells, 0, sizeof(state->cells));
	state->decision = needed ? CORRECTION_NEEDED : CORRECTION_NOT_NEEDED;
	return needed;
}

bool needs_row_correction(int *correction)
{
	struct row_correction_state *state = (void *)correction;
	bool needed;

	if (!state->enabled)
		return false;
	if (state->decision == CORRECTION_PENDING)
		evaluate_row_correction(correction);
	needed = state->decision == CORRECTION_NEEDED;
	/* Consume a cached decision once; the next request reevaluates coverage. */
	state->decision = CORRECTION_PENDING;
	return needed;
}
