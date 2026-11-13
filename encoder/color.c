/* Load BCM profiles, adjust RGB samples and convert them to CMYK ink levels. */
#include "color.h"
#include "data.h"
#include "protocol.h"
#include "state.h"

#include <endian.h>
#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

enum color_limits {
	RGB_CHANNELS = 3,
	INK_CHANNELS = 4,
	DROP_LEVELS = 4,
	BLACK_CHANNEL = 3,
	RGB_MAX = 255,
	RGB_SAMPLES = RGB_MAX + 1,
	RGB_TO_INK_STEP = 4,
	LUMINANCE_RED_PERCENT = 30,
	LUMINANCE_GREEN_PERCENT = 59,
	LUMINANCE_BLUE_PERCENT = 11,
	CURVE_BRIGHTNESS = 0,
	CURVE_CONTRAST = 1,
	CURVE_RED = 2,
	CURVE_COMBINED = 5,
	CURVE_MAPPED = 6,
	INK_MAX = 1023,
	INK_SAMPLES = INK_MAX + 1,
	CURVE_MAX = 65535,
	BCM_CURVE_COUNT = 9,
	BCM_BLACK_CURVE = 8,
	CUBE_EDGE = 17,
	CUBE_CELL_BITS = 4,
	CUBE_CELL_WIDTH = 1 << CUBE_CELL_BITS,
	CUBE_CELL_MASK = CUBE_CELL_WIDTH - 1,
	CUBE_CORNERS = 8,
	CUBE_NODES = CUBE_EDGE * CUBE_EDGE * CUBE_EDGE,
	CUBE_BYTES = CUBE_NODES * INK_CHANNELS * sizeof(uint16_t),
	BCM_CURVE_BYTES = BCM_CURVE_COUNT * RGB_SAMPLES * sizeof(uint16_t),
	TRANSFER_BYTES = INK_CHANNELS * INK_SAMPLES * sizeof(uint16_t),
	BCM_PATH_BYTES = 268,
	BCM_VERSION_OFFSET = 8,
	BCM_VERSION = 2,
	BCM_DIRECTORY_OFFSET = 128,
	BCM_PAYLOAD_HEADER_BYTES = 32,
	ADJUSTMENT_DIVISOR = 128,
	PERCENT_SCALE = 100,
	INK_RATIO_BITS = 8,
	COLOR_MODE_NATURAL = 1,
	COLOR_MODE_VIVID = 2,
	OUTPUT_MONOCHROME = 1,
	HALFTONE_DIRECT_GRAY = 1,
	HALFTONE_DITHER = 1,
	DEFAULT_HEAD_MODE = 5,
	HEAD_MODE_COUNT = 8,
	FALLBACK_INK_PHOTO_0600 = 1020,
	FALLBACK_INK_PHOTO = 1120,
	FALLBACK_INK_PLAIN_0200 = 1256,
	FALLBACK_INK_PLAIN = 2244,
};

/* These are BCM tag fields, not DPI values or printer command bytes. */
enum bcm_tag_field {
	BCM_TAG_CUBE = 0x8000,
	BCM_TAG_CURVES = 0xc000,
	BCM_TAG_HALFTONE = 0xe000,
	BCM_TAG_CORRECTION = 0xb000,
	BCM_TAG_DENSITY = 0x5000,
	BCM_COLOR_UNMATCHED = 0,
	BCM_COLOR_VIVID = 0x10,
	BCM_COLOR_NATURAL = 0x20,
	BCM_DITHER_OFFSET = 0x30,
	BCM_MEDIA_PLAIN = 0x000,
	BCM_MEDIA_INKJET = 0x100,
	BCM_MEDIA_VARIANT_17 = 0x300,
	BCM_MEDIA_PHOTO = 0x400,
	BCM_MEDIA_BP71 = 0x500,
	BCM_MEDIA_VARIANT_28 = 0x600,
	BCM_MEDIA_SLOW_DRYING = 0x700,
	BCM_MEDIA_DUPLEX = 0x800,
	BCM_MEDIA_DUPLEX_REDUCED_INK = 0x900,
	BCM_MEDIA_VARIANT_1C = 0xb00,
	BCM_MEDIA_VARIANT_1D = 0xc00,
	BCM_RESOLUTION_0205 = 0,
	BCM_RESOLUTION_LOW = 1,
	BCM_RESOLUTION_DEFAULT = 2,
	BCM_RESOLUTION_0500 = 3,
	BCM_RESOLUTION_0600 = 4,
	BCM_RESOLUTION_0700 = 6,
	BCM_RESOLUTION_PHOTO_VARIANT = 7,
	BCM_RESOLUTION_0608 = 8,
	RASTER_FAMILY_0300 = 0x300,
	RASTER_VARIANT_MASK = 0xf,
	RASTER_FAMILY_VARIANT_MASK = 0xf0f,
	RASTER_PHOTO_VARIANT = 8,
	RASTER_ALL_INK_CHANNELS = 0x3000,
};

/* Private views of the shared numeric state; buffer pointers live in state.h.
 */
struct color_state {
	uint8_t reserved_output_mode[0xc - 0x0];
	uint16_t output_mode;
	uint16_t binding;
	uint32_t media;
	uint8_t reserved_slow_drying[0xe6 - 0x14];
	uint8_t slow_drying;
	uint8_t reserved_brightness[0x20e - 0xe7];
	int16_t brightness;
	int16_t contrast;
	int16_t channel_gain[RGB_CHANNELS];
	uint8_t reserved_matching[0x21f - 0x218];
	uint8_t matching;
	uint8_t reserved_duplex_drying[0x5ad - 0x220];
	uint8_t duplex_drying;
	uint8_t reserved_raster_mode[0x88a - 0x5ae];
	uint16_t raster_mode;
	uint8_t reserved_cached_media[0x8dc - 0x88c];
	uint32_t cached_media;
	uint16_t cached_dpi;
	uint8_t reserved_cached_matching[0x8e3 - 0x8e2];
	uint8_t cached_matching;
	uint8_t profile_generation;
	uint8_t cached_generation;
	uint8_t halftone_method;
	uint8_t reserved_cached_raster_mode[0x1174 - 0x8e7];
	uint16_t cached_raster_mode;
	uint8_t reserved_ink_limit[0x11ac - 0x1176];
	uint16_t ink_limit;
	int16_t offset_index;
	uint16_t render_dpi;
	uint8_t reserved_cube_tag[0x11bc - 0x11b2];
	uint32_t cube_tag;
	uint32_t curve_tag;
	uint32_t halftone_tag;
	uint32_t correction_tag;
	uint32_t density_tag;
	uint8_t reserved_column_span[0x81300 - 0x11d0];
	uint16_t column_span;
	uint8_t reserved_row_span[0x81304 - 0x81302];
	uint16_t row_span;
	uint8_t reserved_correction_factors[0x8130c - 0x81306];
	double correction_factors[INK_CHANNELS];
	uint32_t black_threshold;
	uint32_t page_rows;
	uint8_t reserved_has_correction[0x81342 - 0x81334];
	uint8_t has_correction;
	uint8_t reserved_density_lower[0x8134a - 0x81343];
	uint16_t density_lower;
	uint16_t density_upper;
	uint8_t has_density;
} __attribute__((packed));

struct ink_channel_state {
	uint8_t reserved_thresholds[0x18 - 0x0];
	uint16_t thresholds[DROP_LEVELS];
	uint16_t active_drops[DROP_LEVELS];
	uint16_t base_drops[DROP_LEVELS];
	uint8_t reserved_threshold_error[0x58 - 0x30];
	uint16_t threshold_error;
	uint16_t drop_error;
	uint8_t reserved_tail[0x78 - 0x5c];
} __attribute__((packed));

struct color_halftone {
	struct ink_channel_state channels[INK_CHANNELS];
	uint8_t reserved_gray_mode[0x1e8 - 0x1e0];
	int32_t gray_mode;
	uint8_t reserved_noise_threshold[0x204 - 0x1ec];
	uint16_t noise_threshold;
	uint16_t noise_offset;
	uint16_t noise_floor;
	uint16_t noise_shift;
	int16_t head_mode;
	uint8_t reserved_media_class[0x4a6b - 0x20e];
	uint8_t media_class;
	uint8_t reserved_has_calibration[0x4ac8 - 0x4a6c];
	uint8_t has_calibration;
} __attribute__((packed));

struct density_adjustment {
	uint8_t reserved[0x350];
	int32_t percent;
} __attribute__((packed));

/* BCM directories use big-endian words; payload samples are little-endian. */
struct bcm_tag {
	uint32_t id;
	uint32_t offset;
	uint32_t length;
};

struct bcm_curve_header {
	uint8_t reserved[12];
	uint32_t ink_limit;
	uint8_t padding[16];
};

struct bcm_levels {
	uint16_t large;
	uint16_t medium;
	uint16_t small;
	uint16_t black;
	uint8_t reserved[6];
	uint16_t error_limit;
};

struct bcm_halftone {
	uint8_t reserved[8];
	uint16_t noise_threshold;
	uint16_t reserved_threshold;
	uint16_t noise_offset;
	uint16_t reserved_offset;
	uint16_t noise_floor;
	uint16_t reserved_floor;
	uint16_t noise_shift;
	uint8_t reserved_shift[10];
	int16_t head_mode;
	uint8_t padding[94];
	struct bcm_levels thresholds[4];
	struct bcm_levels drops[4];
};

struct bcm_correction {
	uint8_t reserved[8];
	uint16_t column_span;
	uint16_t reserved_column;
	uint16_t row_span;
	uint16_t reserved_row;
	uint32_t page_rows;
	uint32_t black_threshold;
	float cyan_factor;
	float magenta_factor;
	float yellow_factor;
	float black_factor;
	uint8_t padding[88];
};

struct bcm_density {
	uint8_t reserved[8];
	uint32_t lower_percent;
	uint32_t upper_percent;
	uint8_t padding[112];
};

static bool read_bcm_bytes(int descriptor, void *output, size_t length,
			   off_t offset)
{
	/* Reject truncated files instead of using uninitialized stack contents.
	 */
	return pread(descriptor, output, length, offset) == (ssize_t)length;
}

static bool find_bcm_tag(int descriptor, uint32_t count, uint32_t wanted,
			 struct bcm_tag *tag)
{
	uint32_t index;

	for (index = 0; index < count; index++) {
		if (!read_bcm_bytes(descriptor, tag, sizeof(*tag),
				    BCM_DIRECTORY_OFFSET + sizeof(uint32_t) +
					    (off_t)index * sizeof(*tag)))
			return false;
		if (be32toh(tag->id) != wanted)
			continue;
		tag->offset = be32toh(tag->offset);
		tag->length = be32toh(tag->length);
		return true;
	}
	return false;
}

/* Load the selected color cube, transfer curves and optional calibration. */
int load_bcm_file(struct printer_state *printer, char *path)
{
	struct bcm_curve_header curve;
	struct bcm_halftone halftone;
	struct bcm_correction correction;
	struct bcm_density density;
	struct bcm_tag tag;
	uint32_t version, tag_count;
	struct color_state *color = (void *)printer->numeric_state;
	struct printer_buffers *buffers = &printer->buffers;
	struct color_halftone *state = (void *)buffers->halftone;
	int descriptor, channel, status = 0;

	descriptor = open(path, O_RDONLY);
	if (descriptor < 0)
		return 0;
	if (!read_bcm_bytes(descriptor, &version, sizeof(version),
			    BCM_VERSION_OFFSET) ||
	    (uint16_t)version != BCM_VERSION ||
	    !read_bcm_bytes(descriptor, &tag_count, sizeof(tag_count),
			    BCM_DIRECTORY_OFFSET))
		goto out;
	tag_count = be32toh(tag_count);
	if (!find_bcm_tag(descriptor, tag_count, color->cube_tag, &tag) ||
	    tag.length != CUBE_BYTES + BCM_PAYLOAD_HEADER_BYTES ||
	    !read_bcm_bytes(descriptor, (void *)buffers->calibration_cube,
			    CUBE_BYTES, tag.offset + BCM_PAYLOAD_HEADER_BYTES))
		goto out;
	if (!find_bcm_tag(descriptor, tag_count, color->curve_tag, &tag) ||
	    tag.length != BCM_CURVE_BYTES + sizeof(curve) ||
	    !read_bcm_bytes(descriptor, &curve, sizeof(curve), tag.offset) ||
	    !read_bcm_bytes(descriptor, (void *)buffers->calibration_curves,
			    BCM_CURVE_BYTES, tag.offset + sizeof(curve)))
		goto out;
	color->ink_limit = curve.ink_limit;
	status = 1;

	/*
	 * Optional blocks form a chain: stop at the first missing block. Do not
	 * clear later flags here; callers retain their previous calibration.
	 */
	state->has_calibration = 0;
	if (!find_bcm_tag(descriptor, tag_count, color->halftone_tag, &tag))
		goto out;
	status = 0;
	if (tag.length != sizeof(halftone) ||
	    !read_bcm_bytes(descriptor, &halftone, sizeof(halftone),
			    tag.offset))
		goto out;
	for (channel = 0; channel < INK_CHANNELS; channel++) {
		const struct bcm_levels *limits = &halftone.thresholds[channel];
		const struct bcm_levels *drops = &halftone.drops[channel];
		struct ink_channel_state *output = &state->channels[channel];
		/* The renderer orders these levels as black, small, medium,
		 * large. */
		uint16_t thresholds[] = {
			limits->black,
			limits->small,
			limits->medium,
			limits->large,
		};
		uint16_t drop_levels[] = {
			drops->black,
			drops->small,
			drops->medium,
			drops->large,
		};

		memcpy(output->thresholds, thresholds, sizeof(thresholds));
		memcpy(output->active_drops, drop_levels, sizeof(drop_levels));
		memcpy(output->base_drops, drop_levels, sizeof(drop_levels));
		output->threshold_error = limits->error_limit;
		output->drop_error = drops->error_limit;
	}
	state->noise_threshold = halftone.noise_threshold;
	state->noise_offset = halftone.noise_offset;
	state->noise_floor = halftone.noise_floor;
	state->noise_shift = halftone.noise_shift;
	state->head_mode = (uint16_t)(halftone.head_mode - 1) < HEAD_MODE_COUNT
				   ? halftone.head_mode
				   : DEFAULT_HEAD_MODE;
	state->has_calibration = 1;
	status = 1;

	color->has_correction = 0;
	if (!find_bcm_tag(descriptor, tag_count, color->correction_tag, &tag))
		goto out;
	status = 0;
	if (tag.length != sizeof(correction) ||
	    !read_bcm_bytes(descriptor, &correction, sizeof(correction),
			    tag.offset))
		goto out;
	color->column_span = correction.column_span;
	color->row_span = correction.row_span;
	color->page_rows = correction.page_rows;
	color->black_threshold = correction.black_threshold;
	color->correction_factors[0] = correction.cyan_factor;
	color->correction_factors[1] = correction.magenta_factor;
	color->correction_factors[2] = correction.yellow_factor;
	color->correction_factors[3] = correction.black_factor;
	color->has_correction = 1;
	status = 1;

	color->has_density = 0;
	if (!find_bcm_tag(descriptor, tag_count, color->density_tag, &tag))
		goto out;
	status = 0;
	if (tag.length != sizeof(density) ||
	    !read_bcm_bytes(descriptor, &density, sizeof(density), tag.offset))
		goto out;
	color->density_lower = density.lower_percent * INK_MAX / PERCENT_SCALE;
	color->density_upper = density.upper_percent * INK_MAX / PERCENT_SCALE;
	color->has_density = 1;
	status = 1;
out:
	close(descriptor);
	return status;
}

/* Missing profiles deliberately select the embedded fallback calibration. */
static const char *const bcm_filenames[] = {
	"BRPRI14A.BCM",	 "BRPRI14AA.BCM", "BRPRI14AB.BCM", "BRPRI14AC.BCM",
	"BRPRI14AD.BCM", "BRPRI14AE.BCM", "BRPRI14AF.BCM", "BRPRI14AG.BCM",
	"BRPRI14AH.BCM", "BRPRI14AI.BCM", "BRPRI14AJ.BCM", "BRPRI14AK.BCM",
	"BRPRI14AL.BCM", "BRPRI14AM.BCM",
};

uint32_t load_bcm_table(struct printer_state *printer, int table_index)
{
	struct color_state *color = (void *)printer->numeric_state;
	char path[BCM_PATH_BYTES];
	int length;

	if ((int16_t)color->cube_tag >= 0 || table_index < 0 ||
	    (size_t)table_index >=
		    sizeof(bcm_filenames) / sizeof(bcm_filenames[0]))
		return 0;
	length = snprintf(path, sizeof(path), "%s%s",
			  (char *)lpr_color_table_directory,
			  bcm_filenames[table_index]);
	if (length < 0 || (size_t)length >= sizeof(path))
		return 0;
	return load_bcm_file(printer, path);
}

static int profile_file_index(const struct color_state *color)
{
	switch (color->media) {
	case MEDIA_INKJET:
	case MEDIA_BROTHER_BP60:
		return 1;
	case MEDIA_OTHER_PHOTO:
	case MEDIA_HAGAKI_REVERSE:
		return 4;
	case MEDIA_VARIANT_17:
		return 3;
	case MEDIA_BROTHER_BP71:
		return 5;
	case MEDIA_VARIANT_1C:
		return 12;
	case MEDIA_VARIANT_1D:
		return 13;
	case MEDIA_VARIANT_28:
		return 6;
	default:
		if (color->binding == BINDING_SIMPLEX)
			return color->slow_drying == SLOW_DRYING_ENABLED ? 7
									 : 0;
		return color->duplex_drying == DRYING_REDUCED_INK ? 9 : 8;
	}
}

uintptr_t select_color_tables(struct printer_state *printer, short color_mode)
{
	struct color_state *color = (void *)printer->numeric_state;
	struct printer_buffers *buffers = &printer->buffers;

	choose_color_table_tags(printer, color_mode);
	if ((int16_t)color->cube_tag < 0) {
		buffers->calibration_curves = buffers->color_workspace;
		buffers->calibration_cube =
			buffers->color_workspace + BCM_CURVE_BYTES;
		buffers->color_cube = buffers->calibration_cube;
		if (!load_bcm_table(printer, profile_file_index(color)))
			select_fallback_color_tables(printer, color_mode);
	}
	buffers->transfer_curves = buffers->curve_workspace;
	buffers->offset_curves = buffers->curve_workspace + TRANSFER_BYTES;
	buffers->black_curve = buffers->curve_workspace + 2 * TRANSFER_BYTES;
	build_ink_transfer_curves(printer);
	color->cached_media = color->media;
	color->cached_dpi = color->render_dpi;
	color->cached_matching = color->matching;
	color->cached_raster_mode = color->raster_mode;
	return buffers->color_cube;
}

static uint8_t clamp_rgb(int16_t value)
{
	if (value < 0)
		return 0;
	return value > RGB_MAX ? RGB_MAX : value;
}

void build_rgb_adjustment_tables(struct printer_state *printer)
{
	struct color_state *color = (void *)printer->numeric_state;
	uint8_t (*curves)[RGB_SAMPLES] = (void *)printer->buffers.rgb_curves;
	uint8_t *brightness = curves[CURVE_BRIGHTNESS],
		*contrast = curves[CURVE_CONTRAST];
	uint8_t *combined = curves[CURVE_COMBINED],
		*mapped = curves[CURVE_MAPPED];
	const uint8_t *channel_map = lpr_color_channel_map;
	int sample, channel, adjusted;

	/* Integer division truncates negative adjustments toward zero. */
	for (sample = 0; sample < RGB_SAMPLES; sample++) {
		adjusted = color->brightness * RGB_MAX / ADJUSTMENT_DIVISOR +
			   sample;
		brightness[sample] = clamp_rgb((int16_t)adjusted);
		adjusted = (sample - ADJUSTMENT_DIVISOR) * color->contrast /
				   ADJUSTMENT_DIVISOR +
			   sample;
		contrast[sample] = clamp_rgb((int16_t)adjusted);
	}
	for (channel = 0; channel < RGB_CHANNELS; channel++) {
		for (sample = 0; sample < RGB_SAMPLES; sample++) {
			adjusted = brightness[sample] *
					   color->channel_gain[channel] /
					   ADJUSTMENT_DIVISOR +
				   brightness[sample];
			curves[CURVE_RED + channel][sample] =
				contrast[clamp_rgb((int16_t)adjusted)];
		}
	}
	for (sample = 0; sample < RGB_SAMPLES; sample++)
		combined[sample] = contrast[brightness[sample]];
	for (sample = 0; sample < RGB_SAMPLES; sample++)
		mapped[sample] = combined[channel_map[sample]];
}

void update_color_tables(struct printer_state *printer)
{
	struct color_state *color = (void *)printer->numeric_state;
	struct printer_buffers *buffers = &printer->buffers;
	short color_mode;

	if (!buffers->color_cube ||
	    color->profile_generation != color->cached_generation) {
		color_mode = color->matching == MATCHING_VIVID
				     ? COLOR_MODE_VIVID
				     : COLOR_MODE_NATURAL;
		buffers->color_cube = select_color_tables(printer, color_mode);
		color->cached_generation = color->profile_generation;
	}
	build_rgb_adjustment_tables(printer);
}

static bool has_color_matching(const struct color_state *color)
{
	return color->matching != MATCHING_NONE &&
	       color->output_mode != OUTPUT_MONOCHROME;
}

/* BCM media banks and halftone selectors are distinct from wire media codes. */
struct media_profile {
	uint32_t media;
	uint16_t bank;
	uint8_t halftone_class;
};

static const struct media_profile photo_profiles[] = {
	{MEDIA_INKJET, BCM_MEDIA_INKJET, 2},
	{MEDIA_BROTHER_BP60, BCM_MEDIA_INKJET, 1},
	{MEDIA_OTHER_PHOTO, BCM_MEDIA_PHOTO, 5},
	{MEDIA_HAGAKI_REVERSE, BCM_MEDIA_PHOTO, 5},
	{MEDIA_VARIANT_17, BCM_MEDIA_VARIANT_17, 4},
	{MEDIA_BROTHER_BP71, BCM_MEDIA_BP71, 3},
	{MEDIA_VARIANT_1C, BCM_MEDIA_VARIANT_1C, 5},
	{MEDIA_VARIANT_1D, BCM_MEDIA_VARIANT_1D, 5},
};

static uint16_t plain_media_bank(const struct color_state *color)
{
	int16_t media_class = color->media;

	if (color->media == MEDIA_PLAIN ||
	    color->media == MEDIA_HAGAKI_ADDRESS) {
		if (color->binding == BINDING_SIMPLEX) {
			if (color->slow_drying == SLOW_DRYING_ENABLED)
				media_class = 1;
		} else {
			media_class = color->duplex_drying == DRYING_REDUCED_INK
					      ? 4
					      : 2;
		}
	}
	switch (media_class) {
	case 1:
		return BCM_MEDIA_SLOW_DRYING;
	case 2:
		return BCM_MEDIA_DUPLEX;
	case 4:
		return BCM_MEDIA_DUPLEX_REDUCED_INK;
	default:
		return BCM_MEDIA_PLAIN;
	}
}

void choose_color_table_tags(struct printer_state *printer, short color_mode)
{
	struct color_state *color = (void *)printer->numeric_state;
	struct color_halftone *halftone = (void *)printer->buffers.halftone;
	uint16_t family = color->raster_mode & RASTER_RESOLUTION_FAMILY_MASK;
	uint16_t variant = color->raster_mode & RASTER_FAMILY_VARIANT_MASK;
	uint32_t selection, bank = plain_media_bank(color);
	uint16_t resolution = BCM_RESOLUTION_DEFAULT,
		 matching = BCM_COLOR_UNMATCHED;
	bool low_resolution = true, photo = false;
	size_t index;

	if (has_color_matching(color))
		matching = color_mode == COLOR_MODE_NATURAL ? BCM_COLOR_NATURAL
							    : BCM_COLOR_VIVID;
	halftone->media_class = 0;
	for (index = 0;
	     index < sizeof(photo_profiles) / sizeof(photo_profiles[0]);
	     index++) {
		if (photo_profiles[index].media != color->media)
			continue;
		bank = photo_profiles[index].bank;
		halftone->media_class = photo_profiles[index].halftone_class;
		photo = true;
		break;
	}
	if (photo) {
		low_resolution = family == RASTER_LOW_RESOLUTION_FAMILY ||
				 family == RASTER_MEDIUM_RESOLUTION_FAMILY ||
				 family == RASTER_FAMILY_0300;
		if (!low_resolution) {
			if (family == RASTER_PROFILE_0700)
				resolution = BCM_RESOLUTION_0700;
			else if (family == RASTER_PROFILE_0600)
				resolution = BCM_RESOLUTION_0600;
			else if ((color->raster_mode & RASTER_VARIANT_MASK) ==
				 RASTER_PHOTO_VARIANT)
				resolution = BCM_RESOLUTION_PHOTO_VARIANT;
			else
				resolution = BCM_RESOLUTION_0500;
		}
	} else if (color->media == MEDIA_VARIANT_28) {
		bank = BCM_MEDIA_VARIANT_28;
		halftone->media_class = 6;
		if (family == RASTER_PROFILE_0600 ||
		    family == RASTER_PROFILE_0700) {
			resolution = BCM_RESOLUTION_0600;
			low_resolution = false;
		} else if (family == RASTER_PROFILE_0500) {
			resolution = BCM_RESOLUTION_0500;
			low_resolution = false;
		}
		/* This media's vivid table has no separate dither variant. */
		if (matching == BCM_COLOR_VIVID)
			low_resolution = false;
	} else {
		switch (family) {
		case RASTER_LOW_RESOLUTION_FAMILY:
		case RASTER_MEDIUM_RESOLUTION_FAMILY:
			resolution = variant == RASTER_PROFILE_0205
					     ? BCM_RESOLUTION_0205
					     : BCM_RESOLUTION_LOW;
			break;
		case RASTER_PROFILE_0500:
			resolution = BCM_RESOLUTION_0500;
			low_resolution = false;
			break;
		case RASTER_PROFILE_0600:
			resolution = variant == RASTER_PROFILE_0608
					     ? BCM_RESOLUTION_0608
					     : BCM_RESOLUTION_0600;
			low_resolution = false;
			break;
		case RASTER_PROFILE_0700:
			resolution = BCM_RESOLUTION_0700;
			low_resolution = false;
			break;
		}
	}
	selection = bank + matching + resolution;
	color->curve_tag = BCM_TAG_CURVES | selection;
	if (low_resolution && color->halftone_method == HALFTONE_DITHER)
		selection += BCM_DITHER_OFFSET;
	color->cube_tag = BCM_TAG_CUBE | selection;
	color->halftone_tag = BCM_TAG_HALFTONE | selection;
	color->correction_tag = BCM_TAG_CORRECTION | selection;
	color->density_tag = BCM_TAG_DENSITY | selection;
}

void select_fallback_color_tables(struct printer_state *printer, short color_mode)
{
	static const void *const cubes[] = {
		lpr_fallback_color_bank_0, lpr_fallback_color_bank_1,
		lpr_fallback_color_bank_2, lpr_fallback_color_bank_3,
		lpr_fallback_color_bank_4, lpr_fallback_color_bank_5,
		lpr_fallback_color_bank_6, lpr_fallback_color_bank_7,
		lpr_fallback_color_bank_8,
	};
	struct color_state *color = (void *)printer->numeric_state;
	struct printer_buffers *buffers = &printer->buffers;
	uint16_t family = color->raster_mode & RASTER_RESOLUTION_FAMILY_MASK;
	unsigned int variant =
		has_color_matching(color)
			? (color_mode == COLOR_MODE_NATURAL ? 0 : 1)
			: 2;

	if (family == RASTER_PROFILE_0500 || family == RASTER_PROFILE_0600 ||
	    family == RASTER_PROFILE_0700) {
		buffers->color_cube = (uintptr_t)cubes[6 + variant];
		buffers->calibration_curves =
			(uintptr_t)lpr_fallback_photo_curves;
		color->ink_limit = family == RASTER_PROFILE_0600
					   ? FALLBACK_INK_PHOTO_0600
					   : FALLBACK_INK_PHOTO;
	} else {
		if (color->halftone_method == HALFTONE_DITHER)
			variant += 3;
		buffers->color_cube = (uintptr_t)cubes[variant];
		buffers->calibration_curves =
			(uintptr_t)lpr_fallback_plain_curves;
		color->ink_limit = family == RASTER_MEDIUM_RESOLUTION_FAMILY
					   ? FALLBACK_INK_PLAIN_0200
					   : FALLBACK_INK_PLAIN;
	}
}

/*
 * Keep the float assignment boundaries and double constants: the renderer's
 * x87 evaluation and truncation affect the final integer ink levels.
 */
static float sample_curve(const uint16_t *curve, uint16_t count, uint16_t input,
			  bool signed_samples)
{
	float fraction = ((float)input / (double)INK_MAX) * (float)(count - 1);
	unsigned int index = (unsigned int)truncl(fraction);

	fraction -= (float)index;
	/*
	 * Exact grid points retain the legacy unsigned interpretation, even
	 * for signed offset curves. Interpolated offsets use signed samples.
	 */
	if (fraction == 0.0 || count <= index + 1)
		return (float)curve[index];
	if (signed_samples)
		return (float)(int16_t)curve[index + 1] * fraction +
		       (float)(int16_t)curve[index] * (1.0 - fraction);
	return (float)curve[index + 1] * fraction +
	       (1.0 - fraction) * (float)curve[index];
}

uint32_t interpolate_scaled_10bit_curve(uint16_t count, intptr_t table,
					uint16_t input, uint16_t *output,
					float scale)
{
	float value;

	if (!count) {
		*output = input;
		return 1;
	}
	if (!table)
		return 0;
	value = sample_curve((const uint16_t *)table, count, input, false);
	*output = (uint16_t)(int)truncl(value * scale * (double)INK_MAX /
					CURVE_MAX);
	return 1;
}

uint32_t interpolate_10bit_curve(uint16_t count, intptr_t table, uint16_t input,
				 uint16_t *output)
{
	float value;

	if (!count) {
		*output = input;
		return 1;
	}
	if (!table)
		return 0;
	value = sample_curve((const uint16_t *)table, count, input, false);
	*output = (uint16_t)(int)truncl(value * (double)INK_MAX / CURVE_MAX);
	return 1;
}

uint32_t interpolate_signed_10bit_curve(uint16_t count, intptr_t table,
					uint16_t input, uint16_t *output)
{
	float value;

	if (!count) {
		*output = input;
		return 1;
	}
	if (!table)
		return 0;
	value = sample_curve((const uint16_t *)table, count, input, true);
	*output = (uint16_t)(int)truncl(value * (double)INK_MAX / CURVE_MAX);
	return 1;
}

/* Eight-bit inputs index a 256-entry curve directly, without interpolation. */
uint32_t interpolate_scaled_8bit_curve(short count, intptr_t table,
				       uint16_t input, uint16_t *output,
				       float scale)
{
	const uint16_t *curve = (const uint16_t *)table;

	if (!count) {
		*output = input;
		return 1;
	}
	if (!curve)
		return 0;
	*output = (uint16_t)(int)truncl((float)curve[input] * scale *
					(double)INK_MAX / CURVE_MAX);
	return 1;
}

uint32_t interpolate_8bit_curve(short count, intptr_t table, uint16_t input,
				uint16_t *output)
{
	const uint16_t *curve = (const uint16_t *)table;

	if (!count) {
		*output = input;
		return 1;
	}
	if (!curve)
		return 0;
	*output = (uint16_t)(int)truncl((float)curve[input] * (double)INK_MAX /
					CURVE_MAX);
	return 1;
}

void build_ink_transfer_curves(struct printer_state *printer)
{
	struct color_state *color = (void *)printer->numeric_state;
	struct printer_buffers *buffers = &printer->buffers;
	const uint16_t (*curves)[RGB_SAMPLES] =
		(void *)buffers->calibration_curves;
	const struct density_adjustment *density = (void *)buffers->enhancement;
	uint16_t (*transfer)[INK_SAMPLES] = (void *)buffers->transfer_curves;
	uint16_t (*offset)[INK_SAMPLES] = (void *)buffers->offset_curves;
	uint16_t *black = (void *)buffers->black_curve;
	float scale = (float)density->percent / (double)PERCENT_SCALE + 1.0;
	bool black_only = color->output_mode != OUTPUT_COLOR &&
			  (color->raster_mode & RASTER_CHANNEL_MASK) !=
				  RASTER_ALL_INK_CHANNELS;
	unsigned int channel, sample;

	/* The caller supplies all nine source curves and the expanded buffers.
	 */
	if (!black_only) {
		for (channel = 0; channel < INK_CHANNELS; channel++) {
			for (sample = 0; sample < INK_SAMPLES; sample++) {
				if (scale == 1.0)
					interpolate_10bit_curve(
						RGB_SAMPLES,
						(intptr_t)curves[channel],
						sample,
						&transfer[channel][sample]);
				else
					interpolate_scaled_10bit_curve(
						RGB_SAMPLES,
						(intptr_t)curves[channel],
						sample,
						&transfer[channel][sample],
						scale);
				interpolate_signed_10bit_curve(
					RGB_SAMPLES,
					(intptr_t)
						curves[INK_CHANNELS + channel],
					sample, &offset[channel][sample]);
			}
		}
	}
	for (sample = 0; sample < RGB_SAMPLES; sample++) {
		if (scale == 1.0)
			interpolate_8bit_curve(
				RGB_SAMPLES, (intptr_t)curves[BCM_BLACK_CURVE],
				sample, &black[sample]);
		else
			interpolate_scaled_8bit_curve(
				RGB_SAMPLES, (intptr_t)curves[BCM_BLACK_CURVE],
				sample, &black[sample], scale);
	}
}

uint16_t *apply_ink_limit(short *input, uint16_t *output, struct printer_state *printer)
{
	struct color_state *color = (void *)printer->numeric_state;
	struct printer_buffers *buffers = &printer->buffers;
	const uint16_t (*transfer)[INK_SAMPLES] =
		(void *)buffers->transfer_curves;
	const int16_t (*offset)[INK_SAMPLES] = (void *)buffers->offset_curves;
	uint16_t ink[INK_CHANNELS], limit = color->ink_limit;
	uint32_t total = 0, ratio, black;
	unsigned int channel;

	for (channel = 0; channel < INK_CHANNELS; channel++) {
		/* Preserve 16-bit addition before clamping the transfer index.
		 */
		int16_t sample =
			input[channel] + offset[channel][color->offset_index];

		if (sample < 0)
			sample = 0;
		if (sample > INK_MAX)
			sample = INK_MAX;
		ink[channel] = transfer[channel][sample];
		total += ink[channel];
	}
	black = ink[BLACK_CHANNEL];
	if ((uint16_t)total > limit) {
		/* Preserve black first; reduce C, M and Y in the same
		 * proportion. */
		if ((int16_t)black < (int16_t)limit) {
			ratio = ((limit - black) << INK_RATIO_BITS) /
				((uint16_t)total - black);
			for (channel = 0; channel < BLACK_CHANNEL; channel++)
				ink[channel] = (uint32_t)ink[channel] * ratio >>
					       INK_RATIO_BITS;
		} else {
			for (channel = 0; channel < BLACK_CHANNEL; channel++)
				ink[channel] = 0;
			ink[BLACK_CHANNEL] = limit;
		}
	}
	memcpy(output, ink, sizeof(ink));
	return output;
}

static const void *const interpolation_banks[CUBE_EDGE] = {
	lpr_interpolation_bank_0,  lpr_interpolation_bank_1,
	lpr_interpolation_bank_2,  lpr_interpolation_bank_3,
	lpr_interpolation_bank_4,  lpr_interpolation_bank_5,
	lpr_interpolation_bank_6,  lpr_interpolation_bank_7,
	lpr_interpolation_bank_8,  lpr_interpolation_bank_9,
	lpr_interpolation_bank_10, lpr_interpolation_bank_11,
	lpr_interpolation_bank_12, lpr_interpolation_bank_13,
	lpr_interpolation_bank_14, lpr_interpolation_bank_15,
	lpr_interpolation_bank_16,
};

static unsigned int cube_fraction(unsigned int sample)
{
	/* 255 reaches the far face of the last cell rather than fraction 15. */
	return sample == RGB_MAX ? CUBE_CELL_WIDTH : sample & CUBE_CELL_MASK;
}

static void interpolate_cube(const uint16_t (*cube)[INK_CHANNELS],
			     const unsigned int rgb[RGB_CHANNELS],
			     short *channels)
{
	/* Blue varies fastest in the cube. Weight order is G, B, then R. */
	static const unsigned int corners[CUBE_CORNERS] = {
		0,
		CUBE_EDGE,
		1,
		CUBE_EDGE + 1,
		CUBE_EDGE * CUBE_EDGE,
		CUBE_EDGE * CUBE_EDGE + CUBE_EDGE,
		CUBE_EDGE * CUBE_EDGE + 1,
		CUBE_EDGE * CUBE_EDGE + CUBE_EDGE + 1,
	};
	const struct cube_weights *bank =
		interpolation_banks[cube_fraction(rgb[2])];
	const struct cube_weights *weights =
		&bank[cube_fraction(rgb[1]) * CUBE_EDGE +
		      cube_fraction(rgb[0])];
	unsigned int base = ((rgb[0] >> CUBE_CELL_BITS) * CUBE_EDGE +
			     (rgb[1] >> CUBE_CELL_BITS)) *
				    CUBE_EDGE +
			    (rgb[2] >> CUBE_CELL_BITS);
	uint32_t sum[INK_CHANNELS] = {0};
	unsigned int corner, channel;

	/* Retain the quantized calibration weights, including their divisor. */
	for (corner = 0; corner < CUBE_CORNERS; corner++) {
		uint32_t weight = weights->corner[corner];

		if (!weight)
			continue;
		for (channel = 0; channel < INK_CHANNELS; channel++)
			sum[channel] +=
				cube[base + corners[corner]][channel] * weight;
	}
	for (channel = 0; channel < INK_CHANNELS; channel++)
		channels[channel] = sum[channel] / weights->total;
}

uint32_t *interpolate_rgb_to_ink(uint32_t *result, uint8_t *input, short *channels,
				 struct printer_state *printer)
{
	struct color_state *color = (void *)printer->numeric_state;
	struct printer_buffers *buffers = &printer->buffers;
	const struct color_halftone *halftone = (void *)buffers->halftone;
	const uint16_t (*cube)[INK_CHANNELS] = (void *)buffers->color_cube;
	unsigned int rgb[RGB_CHANNELS], gray, maximum, channel, sum;

	if (color->output_mode == OUTPUT_MONOCHROME) {
		/* Legacy integer luminance weights: R 30%, G 59%, B 11%. */
		gray = (input[0] * LUMINANCE_RED_PERCENT +
			input[1] * LUMINANCE_GREEN_PERCENT +
			input[2] * LUMINANCE_BLUE_PERCENT) /
		       PERCENT_SCALE;
		if (halftone->gray_mode == HALFTONE_DIRECT_GRAY) {
			for (channel = 0; channel < INK_CHANNELS; channel++)
				channels[channel] = RGB_MAX - gray;
			/* Direct gray leaves the previous offset index
			 * untouched. */
			goto done;
		}
		for (channel = 0; channel < RGB_CHANNELS; channel++)
			rgb[channel] = gray;
	} else {
		/* Only the color path maps 254 to the white endpoint. */
		for (channel = 0; channel < RGB_CHANNELS; channel++)
			rgb[channel] = input[channel] == RGB_MAX - 1
					       ? RGB_MAX
					       : input[channel];
	}
	maximum = rgb[0];
	for (channel = 1; channel < RGB_CHANNELS; channel++)
		if (rgb[channel] > maximum)
			maximum = rgb[channel];
	gray = (RGB_MAX - maximum) * RGB_TO_INK_STEP;
	color->offset_index = maximum == 0 ? INK_MAX : gray;
	sum = rgb[0] + rgb[1] + rgb[2];
	if (!sum || sum == RGB_CHANNELS * RGB_MAX) {
		unsigned int node = sum ? CUBE_NODES - 1 : 0;

		memcpy(channels, cube[node], INK_CHANNELS * sizeof(*channels));
	} else {
		interpolate_cube(cube, rgb, channels);
	}
done:
	memmove(result, channels, INK_CHANNELS * sizeof(*channels));
	return result;
}

void apply_row_ink_limit(short *channels, uint32_t reserved,
			 uint32_t packed_limit, uint16_t lower_threshold,
			 uint16_t upper_threshold)
{
	uint16_t total = channels[0] + channels[1] + channels[2] + channels[3];
	uint16_t target = packed_limit >> 16, limit = upper_threshold;
	int16_t black = channels[BLACK_CHANNEL];
	long double slope, scale;
	unsigned int channel;

	(void)reserved;
	if (total > upper_threshold) {
		limit = target;
	} else if (total > lower_threshold) {
		slope = ((long double)target - lower_threshold) /
			(int)((uint32_t)upper_threshold - lower_threshold);
		limit = (uint16_t)(int)truncl(
			(target - slope * upper_threshold) + slope * total);
	}
	if (limit >= total)
		return;
	/* This limiter retains black even when black alone exceeds the limit.
	 */
	if (limit < black) {
		for (channel = 0; channel < BLACK_CHANNEL; channel++)
			channels[channel] = 0;
		return;
	}
	scale = (long double)(uint16_t)(limit - black) /
		(uint16_t)(total - black);
	for (channel = 0; channel < BLACK_CHANNEL; channel++)
		channels[channel] =
			(short)truncl((long double)channels[channel] * scale);
}
