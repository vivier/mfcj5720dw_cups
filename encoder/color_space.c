/* RGB chroma adjustment through CIE XYZ, Lab and cylindrical LCh. */
#include "color_space.h"

#include <math.h>
#include <stddef.h>
#include <stdint.h>

#define COLOR_COMPONENTS 3
#define RGB_MAX 255.0
#define RGB_ROUNDING_BIAS 0.5
#define PERCENT_SCALE 100.0
#define HALF_TURN_DEGREES 180.0
#define FULL_TURN_DEGREES 360.0

/*
 * CIE Lab/LCh formula reference:
 * https://www.w3.org/TR/css-color-4/#color-conversion-code
 *
 * Preserve the existing decimal approximations and evaluation precision.
 * Replacing these with exact CIE fractions, M_PI, cbrt() or hypot() can change
 * rounding at pixel boundaries. RGB uses the configured power-law gamma and
 * matrices, not the piecewise sRGB transfer function from that reference.
 */
#define LAB_LIGHTNESS_SCALE 116.0
#define LAB_LIGHTNESS_OFFSET 16.0
#define LAB_A_SCALE 500.0
#define LAB_B_SCALE 200.0
#define LAB_CUBE_THRESHOLD 0.008856
#define LAB_INVERSE_CUBE_THRESHOLD 0.2069
#define LAB_LINEAR_SCALE 7.787
#define LAB_LINEAR_OFFSET 0.13793103448275862
#define LAB_CUBE_ROOT_EXPONENT 0.3333333333333333
#define LEGACY_PI 3.141592654

enum xyz_component {
	XYZ_X,
	XYZ_Y,
	XYZ_Z,
};

/* Packed views retain the enhancement module's four-byte-aligned layout. */
struct color_vector {
	double values[COLOR_COMPONENTS];
} __attribute__((packed));

struct color_matrix {
	double values[COLOR_COMPONENTS][COLOR_COMPONENTS];
} __attribute__((packed));

struct color_adjustment {
	uint8_t reserved_prefix[0x3c];
	struct color_vector rgb;
	struct color_vector xyz;
	double lab_lightness;
	double lab_a;
	double lab_b;
	double hue_degrees;
	double lch_lightness;
	double chroma;
	struct color_vector reference_white;
	float gamma;
	struct color_matrix rgb_to_xyz;
	struct color_matrix xyz_to_rgb;
	uint8_t reserved_enhancement[0x328 - 0x148];
	float chroma_percent;
	float chroma_limit;
} __attribute__((packed));

static void transform_color(const struct color_matrix *matrix,
			    const struct color_vector *input,
			    struct color_vector *output)
{
	unsigned int row, column;

	for (row = 0; row < COLOR_COMPONENTS; row++) {
		double sum = 0.0;

		for (column = 0; column < COLOR_COMPONENTS; column++)
			sum = input->values[column] * matrix->values[row][column] + sum;
		output->values[row] = sum;
	}
}

void adjust_rgb_chroma(intptr_t address)
{
	struct color_adjustment *color = (struct color_adjustment *)address;
	double delta, limit;
	unsigned int channel;

	rgb_to_lch(address);
	delta = color->chroma * (double)color->chroma_percent / PERCENT_SCALE;
	limit = color->chroma_limit;
	/* Ordered comparisons also preserve the legacy handling of NaNs. */
	if ((delta < 0.0 && delta < limit) || (delta >= 0.0 && delta > limit))
		delta = limit;
	color->chroma = delta + color->chroma;
	lch_to_rgb(address);
	for (channel = 0; channel < COLOR_COMPONENTS; channel++) {
		double rounded = color->rgb.values[channel] + RGB_ROUNDING_BIAS;

		/* The caller truncates to an integer; only the upper end is clamped. */
		color->rgb.values[channel] = rounded > RGB_MAX ? RGB_MAX : rounded;
	}
}

void rgb_to_lch(intptr_t address)
{
	struct color_adjustment *color = (struct color_adjustment *)address;
	unsigned int channel;

	for (channel = 0; channel < COLOR_COMPONENTS; channel++)
		color->rgb.values[channel] /= RGB_MAX;
	for (channel = 0; channel < COLOR_COMPONENTS; channel++)
		color->rgb.values[channel] = pow(color->rgb.values[channel],
					       (double)color->gamma);
	transform_color(&color->rgb_to_xyz, &color->rgb, &color->xyz);
	xyz_to_lch(address);
}

void lch_to_rgb(intptr_t address)
{
	struct color_adjustment *color = (struct color_adjustment *)address;
	unsigned int channel;

	lch_to_xyz(address);
	transform_color(&color->xyz_to_rgb, &color->xyz, &color->rgb);
	for (channel = 0; channel < COLOR_COMPONENTS; channel++)
		color->rgb.values[channel] = pow(color->rgb.values[channel],
					       (double)(1.0 / color->gamma));
	for (channel = 0; channel < COLOR_COMPONENTS; channel++)
		color->rgb.values[channel] *= RGB_MAX;
}

void xyz_to_lch(intptr_t address)
{
	struct color_adjustment *color = (struct color_adjustment *)address;
	long double a, b;
	double hue;

	xyz_to_lab(address);
	a = color->lab_a;
	b = color->lab_b;
	hue = (double)atan2l(b, a) * HALF_TURN_DEGREES / LEGACY_PI;
	color->hue_degrees = hue < 0.0 ? hue + FULL_TURN_DEGREES : hue;
	color->lch_lightness = color->lab_lightness;
	color->chroma = (double)sqrtl(a * a + b * b);
}

void lch_to_xyz(intptr_t address)
{
	struct color_adjustment *color = (struct color_adjustment *)address;
	double radians = color->hue_degrees * LEGACY_PI / HALF_TURN_DEGREES;
	double projection;

	color->lab_lightness = color->lch_lightness;
	projection = cos(radians);
	color->lab_a = projection * color->chroma;
	projection = sin(radians);
	color->lab_b = projection * color->chroma;
	lab_to_xyz(address);
}

void xyz_to_lab(intptr_t address)
{
	struct color_adjustment *color = (struct color_adjustment *)address;
	long double transferred_x, transferred_z;
	double transferred_y;

	transferred_x = lab_transfer(color->xyz.values[XYZ_X] /
				     color->reference_white.values[XYZ_X]);
	transferred_y = (double)lab_transfer(color->xyz.values[XYZ_Y] /
					   color->reference_white.values[XYZ_Y]);
	transferred_z = lab_transfer(color->xyz.values[XYZ_Z] /
				     color->reference_white.values[XYZ_Z]);
	color->lab_lightness = transferred_y * LAB_LIGHTNESS_SCALE - LAB_LIGHTNESS_OFFSET;
	color->lab_a = ((double)transferred_x - transferred_y) * LAB_A_SCALE;
	color->lab_b = (transferred_y - (double)transferred_z) * LAB_B_SCALE;
}

void lab_to_xyz(intptr_t address)
{
	struct color_adjustment *color = (struct color_adjustment *)address;
	double normalized_lightness = (color->lab_lightness + LAB_LIGHTNESS_OFFSET) /
				      LAB_LIGHTNESS_SCALE;
	long double component;

	component = inverse_lab_transfer(color->lab_a / LAB_A_SCALE + normalized_lightness);
	color->xyz.values[XYZ_X] = (double)(component *
					   (long double)color->reference_white.values[XYZ_X]);
	component = inverse_lab_transfer(normalized_lightness);
	color->xyz.values[XYZ_Y] = (double)(component *
					   (long double)color->reference_white.values[XYZ_Y]);
	component = inverse_lab_transfer(normalized_lightness - color->lab_b / LAB_B_SCALE);
	color->xyz.values[XYZ_Z] = (double)(component *
					   (long double)color->reference_white.values[XYZ_Z]);
}

long double lab_transfer(double value)
{
	if (value > LAB_CUBE_THRESHOLD)
		return (long double)pow(value, LAB_CUBE_ROOT_EXPONENT);
	/* Promote the rounded double constants, rather than using L literals. */
	return (long double)value * (long double)LAB_LINEAR_SCALE +
	       (long double)LAB_LINEAR_OFFSET;
}

long double inverse_lab_transfer(double value)
{
	if (value > LAB_INVERSE_CUBE_THRESHOLD)
		return (long double)pow(value, 3.0);
	return ((long double)value - (long double)LAB_LINEAR_OFFSET) /
	       (long double)LAB_LINEAR_SCALE;
}
