/* Enlarge a complete borderless image with bilinear interpolation. */
#include "raster.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MILLIMETERS_PER_INCH 25.4

enum {
	RGB_CHANNELS = 3,
	FRACTION_ONE = 65536,
	FRACTION_HALF = FRACTION_ONE / 2,
};

struct sample_position {
	unsigned int first;
	unsigned int second;
	unsigned int fraction;
};

/* Map pixel centers; extend the edge samples rather than adding a white rim. */
static struct sample_position sample_position(unsigned int index, unsigned int source,
					     unsigned int destination)
{
	double position = ((double)index + 0.5) * source / destination - 0.5;
	struct sample_position sample;

	if (position < 0.0)
		position = 0.0;
	if (position > source - 1)
		position = source - 1;
	sample.first = position;
	sample.second = sample.first + (sample.first + 1 < source);
	sample.fraction = (position - sample.first) * FRACTION_ONE;
	return sample;
}

static unsigned int interpolate(unsigned int first, unsigned int second,
				unsigned int fraction)
{
	return (first * (FRACTION_ONE - fraction) + second * fraction + FRACTION_HALF) /
		FRACTION_ONE;
}

/*
 * Move image content within the existing overspray canvas, before transport
 * rotation. Repeating its first row extends the edge without another resize.
 */
static int position_borderless_page(unsigned char *page, unsigned int width,
				    unsigned int height, unsigned int shift)
{
	size_t row_bytes = (size_t)width * RGB_CHANNELS;

	if (!shift)
		return 0;
	if (shift >= height) {
		fputs("ERROR: Borderless position exceeds the raster height.\n", stderr);
		return 1;
	}
	memmove(page + shift * row_bytes, page, (height - shift) * row_bytes);
	for (unsigned int row = 1; row < shift; row++)
		memcpy(page + row * row_bytes, page, row_bytes);
	return 0;
}

/*
 * Geometry and channel count have already been checked by settings.c. Only two
 * input rows are retained; the encoder owns the complete RGB destination page.
 */
int enlarge_raster_rgb(cups_raster_t *raster, const cups_page_header2_t *header,
		       unsigned char *output, unsigned int width, unsigned int height,
		       const volatile sig_atomic_t *canceled)
{
	struct sample_position *columns = malloc(width * sizeof(*columns));
	unsigned char *rows = malloc((size_t)header->cupsBytesPerLine * 2);
	unsigned char *page = output;
	unsigned int shift = header->cupsReal[BORDERLESS_DOWNWARD_MM_REAL] *
		header->HWResolution[1] / MILLIMETERS_PER_INCH + 0.5;
	unsigned int channels = header->cupsNumColors;
	unsigned int next_row = 0;
	int result = 1;

	if (!columns || !rows) {
		fputs("ERROR: Cannot allocate borderless interpolation rows.\n", stderr);
		goto out;
	}
	for (unsigned int x = 0; x < width; x++)
		columns[x] = sample_position(x, header->cupsWidth, width);
	for (unsigned int y = 0; y < height; y++) {
		struct sample_position vertical = sample_position(y, header->cupsHeight, height);
		const unsigned char *first, *second;

		if (*canceled)
			goto out;
		while (next_row <= vertical.second) {
			unsigned char *row = rows + (next_row % 2) * header->cupsBytesPerLine;

			if (cupsRasterReadPixels(raster, row, header->cupsBytesPerLine) !=
			    header->cupsBytesPerLine) {
				fputs("ERROR: Truncated raster page.\n", stderr);
				goto out;
			}
			next_row++;
		}
		first = rows + (vertical.first % 2) * header->cupsBytesPerLine;
		second = rows + (vertical.second % 2) * header->cupsBytesPerLine;
		for (unsigned int x = 0; x < width; x++) {
			struct sample_position horizontal = columns[x];

			for (unsigned int color = 0; color < RGB_CHANNELS; color++) {
				unsigned int channel = channels == RGB_CHANNELS ? color : 0;
				unsigned int left = horizontal.first * channels + channel;
				unsigned int right = horizontal.second * channels + channel;
				unsigned int top = interpolate(first[left], first[right], horizontal.fraction);
				unsigned int bottom = interpolate(second[left], second[right], horizontal.fraction);

				*output++ = interpolate(top, bottom, vertical.fraction);
			}
		}
	}
	result = position_borderless_page(page, width, height, shift);
out:
	free(rows);
	free(columns);
	return result;
}
