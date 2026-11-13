/* Calibrate nozzle thresholds, row correction and borderless ink density. */
#include "printhead.h"
#include "data.h"
#include "diffusion.h"
#include "halftone.h"
#include "protocol.h"
#include "raster.h"
#include "row_correction.h"
#include "settings.h"
#include "state.h"
#include "swath.h"

#include <endian.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

enum head_parameter {
	HEAD_CHANNELS = 4,
	HEAD_LEVELS = 4,
	HEAD_ROWS = 420,
	HEAD_SAMPLES = HEAD_ROWS + 1,
	HEAD_SAMPLE_SETS = 3,
	HEAD_GAIN_SETS = 8,
	HEAD_ROW_PHASES = 5,
	HEAD_ROW_MAP_SIZE = 40,
	HEAD_SELECTOR_COUNT = 28,
	DENSITY_SELECTOR_COUNT = 20,
	DENSITY_BANKS = 7,
	HEAD_PARAMETERS_BYTES = 893,
	HEAD_PATH_BYTES = 1024,
	HEAD_GAIN_HEADER_BYTES = HEAD_GAIN_SETS * HEAD_CHANNELS * HEAD_LEVELS,
	HEAD_ACTIVE = 0x01,
	HEAD_BUILD_REQUESTED = 0x10,
	HEAD_TABLES_BUILT = 0x20,
	DENSITY_ACTIVE = 0x01,
	DENSITY_LOADED = 0x02,
	DENSITY_NORMAL = 1000,
	DENSITY_MINIMUM = 800,
	DENSITY_MAXIMUM = 1200,
	INK_MAX = 1023,
	HEAD_GAIN_MINIMUM = 64,
	HEAD_GAIN_SCALE = 128,
	HEAD_OFFSET_THRESHOLD = 1536,
	HEAD_EXTENDED_SAMPLE_START = 248,
	HEAD_EXTENDED_SAMPLE_SCALE = 4,
	HEAD_EXTENDED_SAMPLE_OFFSET = 744,
	DITHER_THRESHOLD_SCALE = 16384,
	HALFTONE_DITHER = 1,
	HALFTONE_DIFFUSION = 2,
	MULTILEVEL_ENABLED = 1,
	QUALITY_FAST = 1,
	PROFILE_FAMILY_VARIANT_MASK = 0x0f0f,
	PROFILE_FAMILY_0300 = 0x0300,
	PHOTO_PROFILE_START = 0x0500,
	CORRECTION_BUFFER_BYTES = 0x16da0,
	PLAIN_CORRECTION_FLAGS = 3,
	CORRECTION_COLUMN_SPAN = 49,
	CORRECTION_ROW_SPAN = 15,
	CORRECTION_PAGE_ROWS = 419,
	CORRECTION_BLACK_THRESHOLD = 55,
	DEFAULT_DENSITY_LOWER = 1329,
	DEFAULT_DENSITY_UPPER = 920,
	RAMP_TOP_MARGIN = 62,
	RAMP_BOTTOM_MARGIN = 58,
	RAMP_SHORT_LENGTH = 200,
	RAMP_LONG_LENGTH = 1000,
	ROW_PHASE_PERIOD = 16,
};

enum head_level {
	HEAD_SMALL,
	HEAD_MEDIUM,
	HEAD_LARGE,
	HEAD_BLACK,
};

enum density_ramp_mode {
	RAMP_DISABLED,
	RAMP_ZERO,
	RAMP_BODY,
	RAMP_LEADING_EDGE,
	RAMP_TRAILING_EDGE,
};

/* Threshold table order differs from the renderer's black-first channel state. */
struct head_channel {
	uint8_t reserved_prefix[0x20];
	int16_t black_drop;
	int16_t small_drop;
	int16_t medium_drop;
	int16_t large_drop;
	int16_t base_black_drop;
	int16_t base_small_drop;
	int16_t base_medium_drop;
	int16_t base_large_drop;
	uint16_t black_phase[HEAD_ROW_PHASES];
	uint16_t small_phase[HEAD_ROW_PHASES];
	uint16_t medium_phase[HEAD_ROW_PHASES];
	uint16_t large_phase[HEAD_ROW_PHASES];
	uint8_t reserved_errors[4];
	uint32_t large_dither;
	uint32_t medium_dither;
	uint32_t small_dither;
	uint32_t black_dither;
	uint8_t reserved_tail[0x78 - 0x6c];
};

struct head_state {
	struct head_channel channels[HEAD_CHANNELS];
	uint8_t reserved_samples[0x20e - 0x1e0];
	/* Each sample set has four channels and a trailing reference per channel. */
	uint8_t samples[HEAD_SAMPLE_SETS][HEAD_CHANNELS][HEAD_SAMPLES];
	uint16_t thresholds[HEAD_CHANNELS][HEAD_ROWS][HEAD_LEVELS];
	uint8_t gains[HEAD_CHANNELS][HEAD_LEVELS];
	uint8_t sample_sets[HEAD_CHANNELS][HEAD_LEVELS];
	uint8_t flags;
	uint8_t media_class;
	uint16_t row_map[HEAD_ROW_MAP_SIZE];
	uint32_t phase_count;
};

/* Private view of shared numeric fields; native buffer pointers stay in state.h. */
struct head_settings {
	uint8_t reserved_media[0x10 - 0x0];
	uint32_t media;
	uint8_t reserved_quality[0xf8 - 0x14];
	uint16_t quality;
	uint8_t reserved_bidirectional_flags[0x109 - 0xfa];
	uint8_t bidirectional_flags;
	uint8_t reserved_requested_halftone[0x21e - 0x10a];
	uint8_t requested_halftone;
	uint8_t reserved_multilevel[0x771 - 0x21f];
	uint8_t multilevel;
	uint8_t reserved_original_media[0x858 - 0x772];
	uint32_t original_media;
	uint32_t original_slow_drying;
	uint8_t page_selection;
	uint8_t reserved_interlace_flags[0x888 - 0x861];
	uint8_t interlace_flags;
	uint8_t reserved_raster_mode[0x88a - 0x889];
	uint16_t raster_mode;
	uint8_t reserved_band_initialized[0x8c8 - 0x88c];
	uint16_t band_initialized;
	uint8_t reserved_halftone_method[0x8e6 - 0x8ca];
	uint8_t halftone_method;
	uint8_t reserved_image_start_y[0x1200 - 0x8e7];
	int32_t image_start_y;
	uint8_t reserved_image_end_y[0x1208 - 0x1204];
	int32_t image_end_y;
	uint8_t reserved_row_correction_enabled[0x12fc - 0x120c];
	int32_t row_correction_enabled;
	uint8_t reserved_expanded_width[0x81334 - 0x1300];
	int32_t expanded_width;
	uint8_t reserved_correction_first_row[0x8133e - 0x81338];
	uint16_t correction_first_row;
	uint8_t reserved_color_correction_enabled[0x81344 - 0x81340];
	uint32_t color_correction_enabled;
	uint16_t color_first_row;
	uint16_t density_lower;
	uint16_t density_upper;
	uint8_t has_density_bounds;
	uint8_t reserved_plain_correction_flags[0x81350 - 0x8134f];
	uint32_t plain_correction_flags;
	uint8_t reserved_density_flags[0x81360 - 0x81354];
	uint8_t density_flags;
	uint8_t reserved_density[0x81362 - 0x81361];
	uint16_t density[HEAD_CHANNELS];
	uint16_t density_banks[DENSITY_BANKS][HEAD_CHANNELS];
	uint8_t reserved_ramp_mode[0x817c4 - 0x813a2];
	uint32_t ramp_mode;
	int32_t band_first_row;
	int32_t ramp_position;
	int32_t ramp_length;
	uint8_t reserved_head_files_requested[0x817d8 - 0x817d4];
	int32_t head_files_requested;
};

static struct head_state *head_state(struct printer_state *printer)
{
	return (void *)printer->buffers.halftone;
}

static const struct calibration_selector *
find_selector(const struct calibration_selector *selectors, size_t count,
	      uint8_t media_class, uint16_t mode, size_t fallback)
{
	size_t index;

	/* Duplicate selectors retain first-match precedence. */
	for (index = 0; index < count; index++)
		if (selectors[index].media_class == media_class &&
		    selectors[index].profile ==
			    (mode & PROFILE_FAMILY_VARIANT_MASK))
			return &selectors[index];
	return &selectors[fallback];
}

static bool head_profile_path(char path[HEAD_PATH_BYTES], const char *name)
{
	int length = snprintf(path, HEAD_PATH_BYTES, "%s%s",
			      lpr_color_table_directory, name);

	return length >= 0 && length < HEAD_PATH_BYTES;
}

void load_printhead_parameters(struct printer_state *printer)
{
	static const char *const filenames[] = {
		"brheadparan0.bin",
		"brheadparan1.bin",
		"brheadparan2.bin",
		"brheadparaw.bin",
	};
	struct head_settings *settings = (void *)printer->numeric_state;
	struct head_state *head = head_state(printer);
	int descriptors[] = {-1, -1, -1, -1};
	uint8_t parameters[HEAD_PARAMETERS_BYTES];
	char path[HEAD_PATH_BYTES];
	unsigned int index;
	uint16_t family;
	bool opened = false;

	settings->head_files_requested = 0;
	if (!(head->flags & HEAD_BUILD_REQUESTED)) {
		settings->head_files_requested = 1;
		/* Open the complete set before changing any calibration samples. */
		for (index = 0; index < HEAD_SAMPLE_SETS + 1; index++) {
			if (!head_profile_path(path, filenames[index]))
				goto close_files;
			descriptors[index] = open(path, O_RDONLY);
			if (descriptors[index] < 0)
				goto close_files;
		}
		opened = true;
		for (index = 0; index < HEAD_SAMPLE_SETS; index++) {
			ssize_t count =
				read(descriptors[index], head->samples[index],
				     sizeof(head->samples[index]));

			/*
			 * Retain the legacy build flag on read failure. Short reads
			 * now follow that path too, keeping unread samples unchanged.
			 */
			if (count != sizeof(head->samples[index]))
				head->flags = HEAD_BUILD_REQUESTED;
		}
		if (read(descriptors[HEAD_SAMPLE_SETS], parameters,
			 sizeof(parameters)) == sizeof(parameters))
			decode_head_parameters(printer, (intptr_t)parameters);
		else
			set_default_head_parameters(printer);
	close_files:
		/* Include descriptor zero and files opened before a later failure. */
		for (index = 0; index < HEAD_SAMPLE_SETS + 1; index++)
			if (descriptors[index] >= 0)
				close(descriptors[index]);
		if (!opened)
			return;
	}
	head->flags &= ~HEAD_ACTIVE;
	if (!(head->flags & HEAD_BUILD_REQUESTED))
		return;
	family = settings->raster_mode & RASTER_RESOLUTION_FAMILY_MASK;
	switch (family) {
	case RASTER_MEDIUM_RESOLUTION_FAMILY:
	case PROFILE_FAMILY_0300:
	case RASTER_PROFILE_0500:
	case RASTER_PROFILE_0600:
	case RASTER_PROFILE_0700:
		break;
	default:
		return;
	}
	head->flags |= HEAD_ACTIVE;
	if (!(head->flags & HEAD_TABLES_BUILT)) {
		if (settings->halftone_method == HALFTONE_DIFFUSION)
			build_diffusion_head_tables(printer);
		else
			build_dither_head_tables(printer);
		head->flags |= HEAD_TABLES_BUILT;
	}
}

void apply_head_density_calibration(struct printer_state *printer)
{
	struct head_settings *settings = (void *)printer->numeric_state;
	struct head_state *head = head_state(printer);
	bool plain_normal = (settings->raster_mode & RASTER_PROFILE_MASK) ==
			    RASTER_PROFILE_0201;
	unsigned int channel;

	if (!(settings->density_flags & DENSITY_ACTIVE))
		return;
	for (channel = 0; channel < HEAD_CHANNELS; channel++) {
		struct head_channel *output = &head->channels[channel];
		int density = settings->density[channel];
		int base = plain_normal ? output->base_black_drop
					: output->base_small_drop;
		int scaled = base * DENSITY_NORMAL;
		int threshold = scaled / density;

		/* The selected density is clamped before this function is called. */
		if (threshold < INK_MAX) {
			threshold = INK_MAX;
			density = scaled / INK_MAX;
			if (density < 1)
				density = DENSITY_NORMAL;
			settings->density[channel] = density;
		}
		if (plain_normal) {
			output->black_drop = threshold;
			output->small_drop = output->base_small_drop *
					     DENSITY_NORMAL / density;
		} else {
			/* Other profiles leave the black threshold untouched. */
			output->small_drop = threshold;
		}
		output->medium_drop =
			output->base_medium_drop * DENSITY_NORMAL / density;
		output->large_drop =
			output->base_large_drop * DENSITY_NORMAL / density;
	}
}

static unsigned int band_width_multiplier(uint16_t profile)
{
	switch (profile) {
	case RASTER_PROFILE_010F:
		return 4;
	case RASTER_PROFILE_0205:
	case RASTER_PROFILE_0215:
	case RASTER_PROFILE_0500:
	case RASTER_PROFILE_0510:
	case RASTER_PROFILE_0600:
	case RASTER_PROFILE_0610:
	case RASTER_PROFILE_0700:
	case RASTER_PROFILE_0710:
		return 2;
	default:
		return 1;
	}
}

void prepare_halftone_band(struct printer_state *printer, struct raster_band *band)
{
	struct head_settings *settings = (void *)printer->numeric_state;
	struct printer_buffers *buffers = &printer->buffers;
	uint16_t mode = settings->raster_mode;
	uint16_t family = mode & RASTER_RESOLUTION_FAMILY_MASK;
	uint16_t depth = RASTER_BINARY_DOTS;
	uint16_t profile;
	int32_t width;

	settings->halftone_method =
		settings->requested_halftone == HALFTONE_DITHER ?
		HALFTONE_DITHER : HALFTONE_DIFFUSION;
	if ((mode & RASTER_PROFILE_MASK) >= PHOTO_PROFILE_START)
		settings->halftone_method = HALFTONE_DIFFUSION;
	if (settings->multilevel == MULTILEVEL_ENABLED) {
		depth = RASTER_FOUR_LEVEL_DOTS;
		if (family == RASTER_MEDIUM_RESOLUTION_FAMILY &&
		    settings->quality != QUALITY_FAST)
			depth = RASTER_FIVE_LEVEL_DOTS;
	} else if (family >= PHOTO_PROFILE_START) {
		depth = RASTER_FOUR_LEVEL_DOTS;
	}
	settings->raster_mode = (mode & ~RASTER_DEPTH_MASK) | depth;
	select_raster_color_mode(printer);
	if (!settings->band_initialized) {
		configure_halftone_channels(printer);
		profile = settings->raster_mode & RASTER_PROFILE_MASK;
		width = (uint32_t)band->width * band_width_multiplier(profile);
		head_state(printer)->phase_count =
			(profile == RASTER_PROFILE_0600 ||
			 profile == RASTER_PROFILE_0610) ? 2 : 1;
		if (settings->halftone_method == HALFTONE_DIFFUSION)
			initialize_diffusion_buffers(printer, HEAD_CHANNELS,
						     width);
		settings->expanded_width = width;
		settings->plain_correction_flags = 0;
		if (is_plain_normal_raster(printer)) {
			settings->plain_correction_flags =
				PLAIN_CORRECTION_FLAGS;
			memset((void *)buffers->correction_previous, 0,
			       CORRECTION_BUFFER_BYTES);
			memset((void *)buffers->correction_current, 0,
			       CORRECTION_BUFFER_BYTES);
		}
	}
	load_head_density_calibration(printer);
	if ((settings->density_flags & DENSITY_LOADED) &&
	    supports_head_density_calibration(printer))
		settings->density_flags |= DENSITY_ACTIVE;
	else
		settings->density_flags &= ~DENSITY_ACTIVE;
}

static uint64_t double_bits(double value)
{
	uint64_t bits;

	memcpy(&bits, &value, sizeof(bits));
	return bits;
}

void configure_row_correction(struct printer_state *printer)
{
	/* Preserve the exact IEEE-754 values of the calibrated defaults. */
	/* Coverage limits: edge CMY, edge dark CMY, interior CMY, interior dark CMY. */
	static const double regular_limits[] = {
		0.2356,
		0.0367,
		0.2913,
		0.0784,
	};
	static const double slow_drying_limits[] = {
		0.1501,
		0.0674,
		0.2256,
		0.1933,
	};
	struct head_settings *settings = (void *)printer->numeric_state;
	const double *limits = regular_limits;
	uint64_t edge_dark_bits;

	settings->row_correction_enabled =
		needs_bidirectional_correction(printer);
	if (settings->row_correction_enabled) {
		if (settings->original_slow_drying ||
		    ((settings->original_media == MEDIA_PLAIN ||
		      settings->original_media == MEDIA_HAGAKI_ADDRESS) &&
		     settings->page_selection))
			limits = slow_drying_limits;
		/* The correction module still receives double values as bit patterns. */
		edge_dark_bits = double_bits(limits[1]);
		initialize_row_correction(
			&settings->row_correction_enabled,
			CORRECTION_COLUMN_SPAN, CORRECTION_ROW_SPAN,
			double_bits(limits[0]), (uint32_t)edge_dark_bits,
			edge_dark_bits >> 32, double_bits(limits[2]),
			double_bits(limits[3]), CORRECTION_PAGE_ROWS,
			CORRECTION_BLACK_THRESHOLD, settings->expanded_width);
		settings->correction_first_row =
			get_first_swath_offset(printer);
	}
	settings->color_correction_enabled =
		needs_color_row_correction(printer);
	if (settings->color_correction_enabled) {
		settings->color_first_row = get_first_swath_offset(printer);
		if (!settings->has_density_bounds) {
			/* These calibration bounds are intentionally in descending order. */
			settings->density_lower = DEFAULT_DENSITY_LOWER;
			settings->density_upper = DEFAULT_DENSITY_UPPER;
		}
	}
}

uint32_t needs_color_row_correction(struct printer_state *printer)
{
	const struct head_settings *settings = (void *)printer->numeric_state;
	uint16_t channels = settings->raster_mode & RASTER_CHANNEL_MASK;

	return (channels == 0 || channels == RASTER_CHANNEL_MASK) &&
	       (settings->raster_mode & RASTER_RESOLUTION_FAMILY_MASK) ==
		       RASTER_MEDIUM_RESOLUTION_FAMILY;
}

uint32_t needs_bidirectional_correction(struct printer_state *printer)
{
	const struct head_settings *settings = (void *)printer->numeric_state;
	uint8_t flag = BIDIRECTIONAL_PLAIN_MEDIA;

	if (!needs_color_row_correction(printer))
		return 0;
	switch (settings->media) {
	case MEDIA_INKJET:
	case MEDIA_OTHER_PHOTO:
	case MEDIA_VARIANT_17:
	case MEDIA_BROTHER_BP71:
	case MEDIA_HAGAKI_REVERSE:
	case MEDIA_VARIANT_1C:
	case MEDIA_VARIANT_1D:
	case MEDIA_BROTHER_BP60:
	case MEDIA_VARIANT_28:
		flag = BIDIRECTIONAL_PHOTO_MEDIA;
		break;
	}
	return (settings->bidirectional_flags & flag) != 0;
}

void load_head_density_calibration(struct printer_state *printer)
{
	struct head_settings *settings = (void *)printer->numeric_state;
	uint16_t densities[HEAD_CHANNELS];
	char path[HEAD_PATH_BYTES];
	ssize_t count;
	int descriptor;
	unsigned int channel;

	if (settings->density_flags & DENSITY_LOADED)
		return;
	if (!head_profile_path(path, "brheadparadc.bin"))
		return;
	descriptor = open(path, O_RDONLY);
	if (descriptor < 0)
		return;
	count = read(descriptor, densities, sizeof(densities));
	close(descriptor);
	/* A short file must not publish uninitialized channel densities. */
	if (count != sizeof(densities))
		return;
	for (channel = 0; channel < HEAD_CHANNELS; channel++)
		settings->density[channel] = be16toh(densities[channel]);
	settings->density_flags |= DENSITY_LOADED;
}

uint8_t supports_head_density_calibration(struct printer_state *printer)
{
	const struct head_settings *settings = (void *)printer->numeric_state;

	return (settings->raster_mode & RASTER_PROFILE_MASK) !=
	       RASTER_PROFILE_0205;
}

bool needs_borderless_density_ramp(struct printer_state *printer)
{
	const struct head_settings *settings = (void *)printer->numeric_state;

	switch (settings->raster_mode & RASTER_PROFILE_MASK) {
	case RASTER_PROFILE_0600:
	case RASTER_PROFILE_0610:
	case RASTER_PROFILE_0700:
	case RASTER_PROFILE_0710:
		return (settings->interlace_flags & INTERLACE_SPECIAL_BOUNDS) !=
		       0;
	default:
		return false;
	}
}

void select_borderless_density_ramp(struct printer_state *printer, int first_row, int row_count,
				    int enabled)
{
	struct head_settings *settings = (void *)printer->numeric_state;
	int32_t end_row, height;
	int length;

	if (!enabled) {
		settings->ramp_mode = RAMP_DISABLED;
		return;
	}
	first_row = (uint32_t)first_row + settings->band_first_row;
	end_row = (uint32_t)first_row + row_count;
	height = (uint32_t)settings->image_end_y - settings->image_start_y;
	length = (settings->raster_mode & RASTER_RESOLUTION_FAMILY_MASK) ==
				 RASTER_PROFILE_0700
			 ? RAMP_LONG_LENGTH
			 : RAMP_SHORT_LENGTH;
	settings->ramp_length = length;
	if (first_row < RAMP_TOP_MARGIN) {
		settings->ramp_mode = RAMP_ZERO;
	} else if (first_row < length + RAMP_TOP_MARGIN) {
		settings->ramp_mode = RAMP_LEADING_EDGE;
		settings->ramp_position = first_row - RAMP_TOP_MARGIN;
	} else if ((int32_t)((uint32_t)height - RAMP_BOTTOM_MARGIN) <=
		   end_row) {
		settings->ramp_mode = RAMP_ZERO;
	} else if (end_row <
		   (int32_t)((uint32_t)height - length - RAMP_BOTTOM_MARGIN)) {
		settings->ramp_mode = RAMP_BODY;
	} else {
		settings->ramp_mode = RAMP_TRAILING_EDGE;
		settings->ramp_position =
			(uint32_t)height - end_row - RAMP_BOTTOM_MARGIN;
	}
}

void decode_head_parameters(struct printer_state *printer, intptr_t parameters)
{
	const struct head_settings *settings = (void *)printer->numeric_state;
	struct head_state *head = head_state(printer);
	const uint8_t *bytes = (const void *)parameters;
	const struct calibration_selector *selector =
		find_selector(lpr_head_parameter_selectors,
			      HEAD_SELECTOR_COUNT, head->media_class,
			      settings->raster_mode, 0);
	uint16_t offset = selector->index + HEAD_GAIN_HEADER_BYTES;
	unsigned int gain_set = bytes[offset];

	if (gain_set >= HEAD_GAIN_SETS)
		gain_set = HEAD_GAIN_SETS - 1;
	memcpy(head->gains, bytes + gain_set * sizeof(head->gains),
	       sizeof(head->gains));
	memcpy(head->sample_sets, bytes + offset + 1,
	       sizeof(head->sample_sets));
}

void set_default_head_parameters(struct printer_state *printer)
{
	static const uint8_t gains[HEAD_CHANNELS][HEAD_LEVELS] = {
		{ 80, 90, 100, 50 },
		{ 60, 90, 90, 60 },
		{ 70, 90, 100, 70 },
		{ 70, 90, 100, 70 },
	};
	static const uint8_t mapping[HEAD_CHANNELS][HEAD_LEVELS] = {
		{ 1, 0, 0, 1 },
		{ 1, 0, 0, 1 },
		{ 1, 0, 0, 1 },
		{ 1, 0, 0, 1 },
	};
	struct head_state *head = head_state(printer);

	memcpy(head->gains, gains, sizeof(gains));
	memcpy(head->sample_sets, mapping, sizeof(mapping));
}

static void build_head_tables(struct printer_state *printer, bool diffusion)
{
	const struct head_settings *settings = (void *)printer->numeric_state;
	struct head_state *head = head_state(printer);
	unsigned int anchor =
		(settings->raster_mode & RASTER_DEPTH_MASK) == RASTER_FIVE_LEVEL_DOTS ?
		HEAD_BLACK : HEAD_SMALL;
	unsigned int channel, level, row;

	for (channel = 0; channel < HEAD_CHANNELS; channel++) {
		const struct head_channel *input = &head->channels[channel];
		const uint8_t *samples[HEAD_LEVELS];
		int thresholds[HEAD_LEVELS] = {
			input->small_drop,
			input->medium_drop,
			input->large_drop,
			input->black_drop,
		};
		uint32_t reference[HEAD_LEVELS];
		int gain = head->gains[channel][anchor];
		int budget, reduction = 0;

		for (level = 0; level < HEAD_LEVELS; level++) {
			unsigned int set = head->sample_sets[channel][level];

			if (set >= HEAD_SAMPLE_SETS)
				set = HEAD_SAMPLE_SETS - 1;
			samples[level] = head->samples[set][channel];
		}
		if (gain < HEAD_GAIN_MINIMUM)
			gain = HEAD_GAIN_MINIMUM;
		budget =
			(thresholds[anchor] - INK_MAX) * HEAD_GAIN_SCALE / gain;
		if (thresholds[anchor] >= HEAD_OFFSET_THRESHOLD && budget > 0 &&
		    samples[anchor][HEAD_ROWS] > budget)
			reduction = samples[anchor][HEAD_ROWS] - budget;
		for (level = 0; level < HEAD_LEVELS; level++)
			reference[level] =
				(thresholds[anchor] < HEAD_OFFSET_THRESHOLD || budget < 1) ?
				0 : (uint32_t)samples[level][HEAD_ROWS] - reduction;
		for (row = 0; row < HEAD_ROWS; row++) {
			for (level = 0; level < HEAD_LEVELS; level++) {
				uint32_t sample = samples[level][row];
				int32_t delta, value;
				int base =
					thresholds[diffusion ? level : anchor];

				if (sample >= HEAD_EXTENDED_SAMPLE_START)
					sample = sample * HEAD_EXTENDED_SAMPLE_SCALE -
						 HEAD_EXTENDED_SAMPLE_OFFSET;
				/*
				 * Preserve the unsigned subtraction and signed result;
				 * division truncates negative adjustments toward zero.
				 */
				delta = (uint32_t)head->gains[channel][level] *
					(sample - reference[level]);
				value = (delta / HEAD_GAIN_SCALE + INK_MAX) *
					base / INK_MAX;
				if (level == HEAD_BLACK && value < INK_MAX)
					value = INK_MAX;
				head->thresholds[channel][row][level] = value;
			}
		}
	}
}

void build_diffusion_head_tables(struct printer_state *printer)
{
	build_head_tables(printer, true);
}

void build_dither_head_tables(struct printer_state *printer)
{
	/* Dither applies the anchor threshold to every level, unlike diffusion. */
	build_head_tables(printer, false);
}

void select_diffusion_row_thresholds(struct printer_state *printer, int row)
{
	struct head_state *head = head_state(printer);
	unsigned int channel;

	if (!(head->flags & HEAD_ACTIVE))
		return;
	for (channel = 0; channel < HEAD_CHANNELS; channel++) {
		const uint16_t *values =
			head->thresholds[channel][head->row_map[row]];
		struct head_channel *output = &head->channels[channel];

		output->small_drop = values[HEAD_SMALL];
		output->medium_drop = values[HEAD_MEDIUM];
		output->large_drop = values[HEAD_LARGE];
		output->black_drop = values[HEAD_BLACK];
	}
}

void select_scaled_row_thresholds(struct printer_state *printer, int row, int row_phase)
{
	struct head_state *head = head_state(printer);
	unsigned int phase, channel, offset, count = head->phase_count;

	if (!(head->flags & HEAD_ACTIVE))
		return;
	/* Zero and one both select phase zero; callers provide at most five. */
	if (count < 2)
		count = 1;
	for (phase = 0; phase < count; phase++) {
		if (head->phase_count < 2)
			offset = 0;
		else if (count == HEAD_ROW_PHASES)
			offset = (phase * 2) % HEAD_ROW_PHASES;
		else
			offset = (phase + row_phase / ROW_PHASE_PERIOD + row_phase) & 1;
		for (channel = 0; channel < HEAD_CHANNELS; channel++) {
			const uint16_t *values =
				head->thresholds[channel][head->row_map[row + offset]];
			struct head_channel *output = &head->channels[channel];

			output->small_phase[phase] = values[HEAD_SMALL];
			output->medium_phase[phase] = values[HEAD_MEDIUM];
			output->large_phase[phase] = values[HEAD_LARGE];
			output->black_phase[phase] = values[HEAD_BLACK];
		}
	}
}

void select_dither_row_thresholds(struct printer_state *printer, int row)
{
	struct head_state *head = head_state(printer);
	unsigned int channel;

	if (!(head->flags & HEAD_ACTIVE))
		return;
	for (channel = 0; channel < HEAD_CHANNELS; channel++) {
		const uint16_t *values =
			head->thresholds[channel][head->row_map[row]];
		struct head_channel *output = &head->channels[channel];

		/* Exact division replaces the compiler's reciprocal-multiply sequence. */
		output->small_dither = (uint32_t)values[HEAD_SMALL] *
				       DITHER_THRESHOLD_SCALE / INK_MAX;
		output->medium_dither = (uint32_t)values[HEAD_MEDIUM] *
					DITHER_THRESHOLD_SCALE / INK_MAX;
		output->large_dither = (uint32_t)values[HEAD_LARGE] *
				       DITHER_THRESHOLD_SCALE / INK_MAX;
		output->black_dither = (uint32_t)values[HEAD_BLACK] *
				       DITHER_THRESHOLD_SCALE / INK_MAX;
	}
}

void select_head_density_calibration(struct printer_state *printer)
{
	struct head_settings *settings = (void *)printer->numeric_state;
	const struct head_state *head = head_state(printer);
	const struct calibration_selector *selector;
	unsigned int channel;

	if (!(settings->density_flags & DENSITY_ACTIVE)) {
		for (channel = 0; channel < HEAD_CHANNELS; channel++)
			settings->density[channel] = DENSITY_NORMAL;
		return;
	}
	/* The legacy fallback uses the second selector's density-bank index. */
	selector = find_selector(lpr_density_selectors,
				 DENSITY_SELECTOR_COUNT, head->media_class,
				 settings->raster_mode, 1);
	for (channel = 0; channel < HEAD_CHANNELS; channel++) {
		uint16_t density =
			settings->density_banks[selector->index][channel];

		if (density < DENSITY_MINIMUM)
			density = DENSITY_MINIMUM;
		if (density > DENSITY_MAXIMUM)
			density = DENSITY_MAXIMUM;
		settings->density[channel] = density;
	}
}
