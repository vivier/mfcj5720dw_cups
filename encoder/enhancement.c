/* Histogram-based tone curves and RGB chroma adjustment before halftoning. */
#include "enhancement.h"
#include "color_space.h"
#include "data.h"
#include "protocol.h"
#include "settings.h"
#include "state.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum enhancement_constants {
	RGB_CHANNELS = 3,
	SAMPLE_COUNT = 256,
	SAMPLE_MAX = SAMPLE_COUNT - 1,
	RGB_WHITE = 0xffffff,
	MONOCHROME_MODE = 1,
	COLOR_MODE = 2,
	KERNEL_WIDTH = 5,
	KERNEL_GAIN = 64,
	NEAR_WHITE_THRESHOLD = 208,
	BALANCE_WHITE_MINIMUM = 224,
	BALANCE_WHITE_FALLBACK = 254,
	ENHANCEMENT_FAMILY_0300 = 0x0300,
};

#define PERCENT_SCALE 100.0
#define RGB_MAXIMUM 255.0

struct sample_bounds {
	int32_t minimum[2];
	int32_t maximum[2];
};

/* Keep the shared color-space block and its four-byte-aligned doubles intact. */
struct enhancement_state {
	struct sample_bounds bounds[2];
	uint8_t reserved_prefix[0x30 - 0x20];
	int32_t reserved_initial_state[2];
	float sampling_scale;
	double rgb[RGB_CHANNELS];
	double xyz[RGB_CHANNELS];
	double lab[RGB_CHANNELS];
	double hue_degrees;
	double lch_lightness;
	double chroma;
	double reference_white[RGB_CHANNELS];
	float gamma;
	double rgb_to_xyz[RGB_CHANNELS][RGB_CHANNELS];
	double xyz_to_rgb[RGB_CHANNELS][RGB_CHANNELS];
	float highlight_percent;
	float shadow_percent;
	int32_t white_point_minimum;
	int32_t black_point_maximum;
	uint32_t reserved_tone;
	float target_midpoint;
	float low_midpoint_divisor;
	float low_midpoint_pivot;
	float high_midpoint_divisor;
	float high_midpoint_pivot;
	float highlight_tail_percent;
	float highlight_reference;
	float shadow_curve_power;
	float shadow_blend_percent;
	float contrast_blend_percent;
	float blend_power;
	float highlight_curve_power;
	float reserved_powers[2];
	int32_t soft_kernel[KERNEL_WIDTH][KERNEL_WIDTH];
	int32_t medium_kernel[KERNEL_WIDTH][KERNEL_WIDTH];
	int32_t sharp_kernel[KERNEL_WIDTH][KERNEL_WIDTH];
	float sharpening_strength;
	int32_t combined_kernel[KERNEL_WIDTH][KERNEL_WIDTH];
	float chroma_percent;
	float chroma_limit;
	int32_t balance_population_weight;
	int32_t channel_white_minimum;
	int32_t near_white_threshold;
	float balance_taper_percent;
	int32_t balance_difference;
	int32_t channel_offsets[RGB_CHANNELS];
	int32_t shared_ink_offset;
} __attribute__((packed));

/* Private view of options stored in the shared numeric printer state. */
struct enhancement_settings {
	uint8_t reserved_before_color_mode[0xc - 0x0];
	int16_t color_mode;
	uint8_t reserved_before_brightness[0x20e - 0xe];
	int16_t brightness;
	int16_t contrast;
	int16_t red;
	int16_t green;
	int16_t blue;
	uint8_t reserved_before_chroma[0x21a - 0x218];
	int16_t chroma;
	int16_t white_balance;
	uint8_t reserved_before_raster_mode[0x88a - 0x21e];
	uint16_t raster_mode;
	uint8_t reserved_before_enhancement_active[0x1178 - 0x88c];
	uint32_t enhancement_active;
};

struct tone_statistics {
	uint32_t samples;
	uint32_t weighted_total;
	int32_t near_white_population;
	int32_t black_point;
	int32_t white_point;
	int32_t highlight_sample;
	float midpoint;
};

static int32_t add_count(int32_t left, int32_t right)
{
	return (uint32_t)left + (uint32_t)right;
}

void collect_band_histogram(struct printer_state *printer, int first_row, uint32_t width,
			    struct raster_band *band, int row_count, int first_column,
			    int column_count)
{
	const struct enhancement_settings *settings = (void *)printer->numeric_state;
	const struct printer_buffers *buffers = &printer->buffers;
	const struct enhancement_state *state = (void *)buffers->enhancement;
	uint32_t *histogram = (void *)buffers->histogram;
	uint32_t (*channels)[RGB_CHANNELS] = (void *)buffers->channel_histograms;
	int row, column;

	(void)width;
	/* Regions supplied by the renderer have positive strides and 16-bit sizes. */
	for (row = 0; row < row_count; row++) {
		const uint8_t *pixel = band->pixels + (first_row + row) * band->row_bytes +
				       first_column * RGB_CHANNELS;

		for (column = 0; column < column_count; column++, pixel += RGB_CHANNELS) {
			uint8_t red = pixel[0], green = pixel[1], blue = pixel[2];

			histogram[red]++;
			histogram[green]++;
			histogram[blue]++;
			if (settings->color_mode == COLOR_MODE &&
			    red >= state->near_white_threshold &&
			    green >= state->near_white_threshold &&
			    blue >= state->near_white_threshold) {
				channels[red][0]++;
				channels[green][1]++;
				channels[blue][2]++;
			}
		}
	}
}

static void identity_curves(uint8_t curves[SAMPLE_COUNT][RGB_CHANNELS])
{
	unsigned int sample, channel;

	for (sample = 0; sample < SAMPLE_COUNT; sample++)
		for (channel = 0; channel < RGB_CHANNELS; channel++)
			curves[sample][channel] = sample;
}

/* Preserve the original float stores and truncations around histogram clipping. */
static void analyze_tone_range(struct tone_statistics *tone, const struct enhancement_state *state,
			       const int32_t *histogram, int contrast)
{
	float population = (float)(int32_t)tone->samples;
	int32_t highlight_budget =
	    (int)truncl(population * state->highlight_percent / PERCENT_SCALE);
	int32_t shadow_budget = (int)truncl(population * state->shadow_percent / PERCENT_SCALE);
	int32_t highlight_step = highlight_budget / (SAMPLE_COUNT - state->white_point_minimum);
	int32_t shadow_step = shadow_budget / state->black_point_maximum;
	int32_t shadow_count = 0, highlight_count = 0;
	int32_t tail_budget, midpoint_limit;

	for (tone->black_point = 0; tone->black_point < SAMPLE_MAX; tone->black_point++) {
		shadow_count = add_count(shadow_count, histogram[tone->black_point]);
		shadow_budget = (uint32_t)shadow_budget - shadow_step;
		if (shadow_budget < shadow_count)
			break;
	}
	if (tone->black_point > state->black_point_maximum)
		tone->black_point = state->black_point_maximum;
	for (tone->white_point = SAMPLE_MAX; tone->white_point > 0; tone->white_point--) {
		highlight_budget = (uint32_t)highlight_budget - highlight_step;
		highlight_count = add_count(highlight_count, histogram[tone->white_point]);
		if (highlight_budget < highlight_count)
			break;
	}
	if (tone->white_point < state->white_point_minimum)
		tone->white_point = state->white_point_minimum;
	tone->white_point = (uint32_t)tone->white_point - contrast;
	if (tone->white_point > SAMPLE_MAX)
		tone->white_point = SAMPLE_MAX;
	midpoint_limit = (uint32_t)truncl(state->target_midpoint);
	if (tone->white_point < midpoint_limit)
		tone->white_point = midpoint_limit;
	tone->black_point = add_count(tone->black_point, contrast);
	if (tone->black_point < 0)
		tone->black_point = 0;
	if (tone->black_point > midpoint_limit)
		tone->black_point = midpoint_limit;

	tail_budget =
	    (int32_t)(int64_t)truncl(population * state->highlight_tail_percent / PERCENT_SCALE);
	/* The highlight scan deliberately continues the previous cumulative count. */
	for (tone->highlight_sample = SAMPLE_MAX; tone->highlight_sample > 0;
	     tone->highlight_sample--) {
		highlight_count = add_count(highlight_count, histogram[tone->highlight_sample]);
		if (tail_budget < highlight_count)
			break;
	}
	tone->midpoint =
	    (((float)(tone->weighted_total / tone->samples) - (float)tone->black_point) * RGB_MAXIMUM) /
	    (float)(tone->white_point - tone->black_point);
	population = (tone->midpoint - state->low_midpoint_pivot) / state->low_midpoint_divisor +
		     state->low_midpoint_pivot;
	if (population > (float)tone->white_point)
		population = (float)tone->white_point;
	if (population < tone->midpoint)
		tone->midpoint = population;
	population = state->high_midpoint_pivot -
		     (state->high_midpoint_pivot - tone->midpoint) / state->high_midpoint_divisor;
	if (population < (float)tone->black_point)
		population = (float)tone->black_point;
	if (tone->midpoint < population)
		tone->midpoint = population;
}

static void white_balance_scales(const struct enhancement_state *state,
				 const struct tone_statistics *tone,
				 const int32_t histogram[SAMPLE_COUNT][RGB_CHANNELS],
				 double scales[RGB_CHANNELS])
{
	int32_t target = (int32_t)(tone->samples * (uint32_t)state->balance_population_weight) /
			 (RGB_CHANNELS * 100);
	int step = (int)truncl(((state->balance_taper_percent / PERCENT_SCALE) * (float)target) /
			       (256.0 - (float)state->channel_white_minimum));
	int white[RGB_CHANNELS];
	int channel, highest, lowest;
	double reference;

	if (tone->near_white_population < target)
		target = tone->near_white_population;
	for (channel = 0; channel < RGB_CHANNELS; channel++) {
		int32_t remaining = target, count = 0;
		int sample;

		for (sample = SAMPLE_MAX; sample > 0; sample--) {
			count = add_count(count, histogram[sample][channel]);
			remaining = (uint32_t)remaining - step;
			if (remaining < count)
				break;
		}
		white[channel] = sample;
		if (white[channel] < state->channel_white_minimum)
			white[channel] = state->channel_white_minimum;
	}
	highest = lowest = white[0];
	for (channel = 1; channel < RGB_CHANNELS; channel++) {
		if (white[channel] > highest)
			highest = white[channel];
		if (white[channel] < lowest)
			lowest = white[channel];
	}
	if (highest == lowest) {
		/* Preserve the existing red-only adjustment when all white points tie. */
		white[0] -= state->balance_difference;
	} else {
		for (channel = 0; channel < RGB_CHANNELS; channel++)
			white[channel] -=
			    (int)truncl(((long double)highest - (long double)white[channel]) *
					(long double)state->balance_difference /
					(long double)(highest - lowest));
	}
	reference = (double)white[0] - (double)state->channel_offsets[0];
	for (channel = 1; channel < RGB_CHANNELS; channel++) {
		if (reference < (double)(white[channel] - state->channel_offsets[channel]))
			reference =
			    (double)white[channel] - (double)state->channel_offsets[channel];
	}
	for (channel = 0; channel < RGB_CHANNELS; channel++)
		scales[channel] =
		    reference / (double)(white[channel] - state->channel_offsets[channel]);
}

static double tone_blend_weight(const struct enhancement_state *state,
				const struct tone_statistics *tone)
{
	float highlight = state->highlight_reference;
	float midpoint = state->target_midpoint;

	if (highlight <= (float)tone->highlight_sample)
		highlight = 0.0;
	else
		highlight = (highlight - (float)tone->highlight_sample) / highlight;
	if (tone->midpoint < midpoint)
		midpoint = (midpoint - tone->midpoint) / midpoint;
	else
		midpoint = 0.0;
	return pow((double)(highlight * midpoint), (double)state->blend_power);
}

static uint8_t tone_curve_sample(const struct enhancement_state *state,
				const struct tone_statistics *tone, double sample, double blend)
{
	double black = tone->black_point;
	double white = tone->white_point;
	double midpoint = tone->midpoint;
	double span = (double)(tone->white_point - tone->black_point);
	double target, slope, distance_to_white, curve_power, contrast_weight;
	long double adjusted;
	float output;

	/* Keep ordered comparisons: a NaN sample follows the zero-output path. */
	if (!(black < sample))
		return 0;
	if (!(sample < white))
		return SAMPLE_MAX;

	target = (double)state->target_midpoint;
	if (target < midpoint) {
		curve_power = pow((sample - black) / span, (double)state->highlight_curve_power);
		adjusted = (long double)(((sample - black) * target) / (midpoint - black)) +
			   (long double)(RGB_MAXIMUM - (target * span) / (midpoint - black)) *
				(long double)curve_power;
	} else {
		slope = (RGB_MAXIMUM - target) / (white - midpoint);
		distance_to_white = white - sample;
		curve_power = pow(distance_to_white / span, (double)state->shadow_curve_power);
		sample = pow((sample - black) / span, (double)state->shadow_blend_percent / PERCENT_SCALE);
		contrast_weight = pow(1.0 - blend, (double)state->contrast_blend_percent / PERCENT_SCALE);
		adjusted = ((long double)1.0 - (long double)contrast_weight) *
				(long double)(sample * RGB_MAXIMUM) +
			   (long double)(RGB_MAXIMUM - ((RGB_MAXIMUM - span * slope) * curve_power +
						 slope * distance_to_white)) *
				(long double)contrast_weight;
	}
	/* The float store is part of the calibrated rounding before truncation. */
	output = (float)adjusted;
	return (uint8_t)(int16_t)truncl(output);
}

void build_enhancement_curves(struct printer_state *printer)
{
	const struct enhancement_settings *settings = (void *)printer->numeric_state;
	const struct printer_buffers *buffers = &printer->buffers;
	const struct enhancement_state *state = (void *)buffers->enhancement;
	const int32_t *histogram = (void *)buffers->histogram;
	const int32_t (*channels)[RGB_CHANNELS] = (void *)buffers->channel_histograms;
	uint8_t (*curves)[RGB_CHANNELS] = (void *)buffers->enhancement_curves;
	struct tone_statistics tone = {0};
	double scales[RGB_CHANNELS], blend;
	int sample, channel;

	configure_color_enhancement(printer);
	for (sample = 0; sample < SAMPLE_COUNT; sample++) {
		tone.samples += (uint32_t)histogram[sample];
		tone.weighted_total += (uint32_t)histogram[sample] * sample;
		tone.near_white_population =
		    add_count(tone.near_white_population, channels[sample][0]);
	}
	if (!tone.samples) {
		identity_curves(curves);
		return;
	}
	analyze_tone_range(&tone, state, histogram, settings->contrast);
	if (settings->color_mode != MONOCHROME_MODE)
		white_balance_scales(state, &tone, channels, scales);
	blend = tone_blend_weight(state, &tone);
	for (sample = 0; sample < SAMPLE_COUNT; sample++) {
		for (channel = 0; channel < RGB_CHANNELS; channel++) {
			double input = (double)sample;

			if (settings->color_mode != MONOCHROME_MODE)
				input *= scales[channel];
			curves[sample][channel] = tone_curve_sample(state, &tone, input, blend);
		}
	}
}

void apply_row_enhancement(struct printer_state *printer, int first_row, uint32_t width,
			   struct raster_band *band, int row_count, int first_column,
			   int column_count)
{
	const struct enhancement_settings *settings = (void *)printer->numeric_state;
	const struct printer_buffers *buffers = &printer->buffers;
	struct enhancement_state *state = (void *)buffers->enhancement;
	const uint8_t (*curves)[RGB_CHANNELS] = (void *)buffers->enhancement_curves;
	uint32_t previous_rgb = RGB_WHITE;
	uint8_t output[RGB_CHANNELS] = {SAMPLE_MAX, SAMPLE_MAX, SAMPLE_MAX};
	int row, column, channel;

	(void)width;
	/* The cache spans rows. Initial white pixels intentionally bypass curves. */
	for (row = 0; row < row_count; row++) {
		uint8_t *pixel = band->pixels + (first_row + row) * band->row_bytes +
				 first_column * RGB_CHANNELS;

		for (column = 0; column < column_count; column++, pixel += RGB_CHANNELS) {
			/* Read exactly three bytes, even for the last pixel in the buffer. */
			uint32_t rgb = (uint32_t)pixel[0] | (uint32_t)pixel[1] << 8 |
				       (uint32_t)pixel[2] << 16;

			if (rgb != previous_rgb) {
				if (rgb == 0 || rgb == RGB_WHITE) {
					for (channel = 0; channel < RGB_CHANNELS; channel++)
						output[channel] = curves[pixel[channel]][channel];
				} else {
					for (channel = 0; channel < RGB_CHANNELS; channel++)
						state->rgb[channel] = pixel[channel];
					if (settings->chroma)
						adjust_rgb_chroma((intptr_t)state);
					for (channel = 0; channel < RGB_CHANNELS; channel++) {
						uint8_t value =
						    (uint8_t)(int16_t)truncl(state->rgb[channel]);

						output[channel] = curves[value][channel];
					}
				}
			}
			memcpy(pixel, output, RGB_CHANNELS);
			previous_rgb = rgb;
		}
	}
}

uint32_t enhance_row_region(struct printer_state *printer, uint32_t first_row, uint32_t width,
			    struct raster_band *band, uint32_t row_count, uint32_t first_column,
			    uint32_t column_count)
{
	apply_row_enhancement(printer, first_row, width, band, row_count, first_column,
			      column_count);
	return 1;
}

void initialize_color_enhancement(struct printer_state *printer)
{
	static const double white[RGB_CHANNELS] = {0.95046, 1.0, 1.08906};
	static const double forward[RGB_CHANNELS][RGB_CHANNELS] = {
	    {0.41239, 0.35758, 0.18048},
	    {0.21264, 0.71517, 0.07219},
	    {0.01933, 0.11919, 0.95053},
	};
	static const double inverse[RGB_CHANNELS][RGB_CHANNELS] = {
	    {3.241, -1.5374, -0.4986},
	    {-0.9692, 1.876, 0.0416},
	    {0.0556, -0.204, 1.057},
	};
	struct enhancement_settings *settings = (void *)printer->numeric_state;
	const struct printer_buffers *buffers = &printer->buffers;
	struct enhancement_state *state = (void *)buffers->enhancement;
	uint32_t *histogram = (void *)buffers->histogram;
	uint32_t (*channels)[RGB_CHANNELS] = (void *)buffers->channel_histograms;
	uint8_t (*curves)[RGB_CHANNELS] = (void *)buffers->enhancement_curves;
	int sample, channel;

	settings->enhancement_active = 1;
	state->sampling_scale = 1.0f;
	state->reserved_initial_state[0] = 0;
	state->reserved_initial_state[1] = 0;
	/* Preserve write order: the legacy workspace's two histogram regions overlap. */
	for (sample = 0; sample < SAMPLE_COUNT; sample++) {
		histogram[sample] = 0;
		for (channel = 0; channel < RGB_CHANNELS; channel++)
			channels[sample][channel] = 0;
		for (channel = 0; channel < RGB_CHANNELS; channel++)
			curves[sample][channel] = sample;
	}
	for (sample = 0; sample < 2; sample++) {
		for (channel = 0; channel < 2; channel++) {
			state->bounds[sample].minimum[channel] = INT16_MAX;
			state->bounds[sample].maximum[channel] = 0;
		}
	}
	memcpy(state->reference_white, white, sizeof(white));
	state->gamma = 2.2f;
	memcpy(state->rgb_to_xyz, forward, sizeof(forward));
	memcpy(state->xyz_to_rgb, inverse, sizeof(inverse));
	state->highlight_percent = 1.0f;
	state->shadow_percent = 2.0f;
	state->white_point_minimum = 236;
	state->black_point_maximum = 24;
	state->target_midpoint = 128.0f;
	state->low_midpoint_divisor = 25.0f;
	state->low_midpoint_pivot = 120.0f;
	state->high_midpoint_divisor = 25.0f;
	state->high_midpoint_pivot = 112.0f;
	state->highlight_tail_percent = 6.0f;
	state->highlight_reference = 208.0f;
	state->shadow_curve_power = 3.0f;
	state->shadow_blend_percent = 3.0f;
	state->contrast_blend_percent = 3.0f;
	state->blend_power = 3.0f;
	state->highlight_curve_power = 3.0f;
	state->reserved_powers[0] = 3.0f;
	state->reserved_powers[1] = 3.0f;
	memcpy(state->soft_kernel, lpr_enhancement_kernel_soft, sizeof(state->soft_kernel));
	memcpy(state->medium_kernel, lpr_enhancement_kernel_medium, sizeof(state->medium_kernel));
	memcpy(state->sharp_kernel, lpr_enhancement_kernel_sharp, sizeof(state->sharp_kernel));
	state->sharpening_strength = 10.0f;
	state->chroma_percent = 20.0f;
	state->chroma_limit = 3.0f;
	state->balance_population_weight = 1;
	state->channel_white_minimum = BALANCE_WHITE_MINIMUM;
	state->near_white_threshold = NEAR_WHITE_THRESHOLD;
	state->balance_taper_percent = 100.0f;
	memset(state->channel_offsets, 0, sizeof(state->channel_offsets));
}

void configure_color_enhancement(struct printer_state *printer)
{
	const struct enhancement_settings *settings = (void *)printer->numeric_state;
	struct enhancement_state *state = (void *)printer->buffers.enhancement;
	uint16_t family = settings->raster_mode & RASTER_RESOLUTION_FAMILY_MASK;
	float resolution, scale = state->sampling_scale, weight;
	int row, column, chroma_steps, minimum_offset;

	switch (family) {
	case RASTER_MEDIUM_RESOLUTION_FAMILY:
	case ENHANCEMENT_FAMILY_0300:
	case RASTER_PROFILE_0500:
	case RASTER_PROFILE_0600:
	case RASTER_PROFILE_0700:
		resolution = 600.0f;
		break;
	default:
		resolution = 300.0f;
	}
	/* Equality includes both signed zeros; NaN follows the division path. */
	if (scale == 0.0f)
		scale = resolution;
	else
		scale = resolution / scale;
	weight = 3600.0 / scale + 24.0;
	if (weight < 0.0f)
		weight = 0.0f;
	state->medium_kernel[KERNEL_WIDTH / 2][KERNEL_WIDTH / 2] = (int)truncl(weight);
	scale = -scale / 750.0 + ((400.0 - scale) * 0.0) / 400.0 + 0.3;
	if (scale < 0.0f)
		scale = 0.0f;
	state->sharpening_strength = scale;
	for (row = 0; row < KERNEL_WIDTH; row++) {
		for (column = 0; column < KERNEL_WIDTH; column++) {
			/* The shared renderer expects a transposed, fixed-point kernel. */
			state->combined_kernel[column][row] =
			    (uint32_t)state->medium_kernel[row][column] * KERNEL_GAIN +
			    (uint32_t)state->sharp_kernel[row][column] * KERNEL_GAIN;
		}
	}
	chroma_steps = 0;
	if (settings->chroma >= -2 && settings->chroma <= 2)
		chroma_steps = settings->chroma < 0 ? settings->chroma : settings->chroma + 1;
	state->chroma_percent = chroma_steps * 20.0f;
	state->chroma_limit = chroma_steps * 3.0f;
	state->balance_difference = 0;
	state->channel_white_minimum = BALANCE_WHITE_FALLBACK;
	if (settings->white_balance >= -2 && settings->white_balance <= 2) {
		state->balance_difference = settings->white_balance * 10;
		state->channel_white_minimum = BALANCE_WHITE_MINIMUM;
	}
	state->balance_taper_percent = 100.0f;
	state->channel_offsets[0] = settings->red;
	state->channel_offsets[1] = settings->green;
	state->channel_offsets[2] = settings->blue;
	minimum_offset = settings->red;
	if (settings->green < minimum_offset)
		minimum_offset = settings->green;
	if (settings->blue < minimum_offset)
		minimum_offset = settings->blue;
	state->shared_ink_offset = minimum_offset > 0 ? -minimum_offset : 0;
	/* The shared offset is computed before monochrome clears channel offsets. */
	if (settings->color_mode == MONOCHROME_MODE)
		memset(state->channel_offsets, 0, sizeof(state->channel_offsets));
	/* Reconfiguration deliberately adds brightness to the existing midpoint. */
	state->target_midpoint += (float)settings->brightness;
}
