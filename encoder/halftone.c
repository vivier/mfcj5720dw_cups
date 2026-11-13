/* Select halftone callbacks, dot sizes and calibrated channel thresholds. */
#include "halftone.h"
#include "data.h"
#include "protocol.h"
#include "state.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum halftone_parameter {
	HALFTONE_DITHER = 1,
	INK_CHANNEL_COUNT = 4,
	/* Remove the color-channel and media-variant bits before lookup. */
	HALFTONE_PROFILE_MASK = 0xcf0f,
	PROFILE_SPECIAL_PAPER = 0x10,
	PROFILE_SLOW_DRYING = 0x20,
	PROFILE_PHOTO = 0x40,
	PROFILE_INKJET = 0x50,
	PROFILE_MEDIA_17 = 0x60,
	PROFILE_MEDIA_1C = 0x70,
	PROFILE_MEDIA_1D = 0x80,
	GRAY_DISABLED = 0,
	GRAY_DIRECT = 1,
	GRAY_FROM_COLOR_TABLE = 16,
	DEFAULT_NOISE_THRESHOLD = 80,
	DEFAULT_NOISE_OFFSET = 128,
	DEFAULT_NOISE_FLOOR = 2,
	DEFAULT_NOISE_SHIFT = 3,
};

/* Private views keep the shared numeric state and native buffers unchanged. */
struct drop_levels {
	uint16_t black;
	uint16_t small;
	uint16_t medium;
	uint16_t large;
};

struct channel_parameters {
	uint8_t reserved_prefix[0x18];
	struct drop_levels thresholds;
	struct drop_levels active_drops;
	struct drop_levels base_drops;
	uint8_t reserved_errors[0x58 - 0x30];
	uint16_t threshold_error;
	uint16_t drop_error;
	uint8_t reserved_tail[0x78 - 0x5c];
};

struct halftone_parameters {
	struct channel_parameters channels[INK_CHANNEL_COUNT];
	uint8_t reserved_gray[0x1e8 - 0x1e0];
	uint32_t gray_mode;
	uint8_t reserved_noise[0x204 - 0x1ec];
	uint16_t noise_threshold;
	uint16_t noise_offset;
	uint16_t noise_floor;
	uint16_t noise_shift;
	uint8_t reserved_calibration[0x4ac8 - 0x20c];
	uint8_t has_calibration;
};

struct halftone_selection {
	uint8_t reserved_output[0xc];
	uint16_t output_mode;
	uint16_t binding;
	uint32_t media;
	uint8_t reserved_drying[0xe6 - 0x14];
	uint8_t slow_drying;
	uint8_t reserved_paper[0x848 - 0xe7];
	uint16_t paper_id;
	uint8_t reserved_mode[0x88a - 0x84a];
	uint16_t raster_mode;
	uint8_t reserved_method[0x8e6 - 0x88c];
	uint8_t method;
	uint8_t reserved_scale[0x11b4 - 0x8e7];
	uint32_t pixel_scale;
	uint8_t reserved_stride[0x11d8 - 0x11b8];
	uint32_t plane_stride;
};

static bool uses_special_paper_profile(uint16_t paper_id)
{
	/* The legacy list includes ISO B5 and an unresolved paper variant. */
	switch (paper_id) {
	case PAPER_ID_ISO_B5:
	case PAPER_ID_ENVELOPE_C5:
	case PAPER_ID_ENVELOPE_YOU4:
	case PAPER_ID_ENVELOPE_CHOU3:
	case PAPER_ID_ENVELOPE_C4:
	case PAPER_ID_ENVELOPE_10:
	case PAPER_ID_ENVELOPE_DL:
	case PAPER_ID_ENVELOPE_MONARCH:
	case PAPER_ID_ENVELOPE_CHOU4:
	case PAPER_ID_ENVELOPE_KAKU2:
	case PAPER_ID_VARIANT_132:
		return true;
	default:
		return false;
	}
}

static uint32_t diffusion_variant(const struct halftone_selection *selection)
{
	switch (selection->media) {
	case MEDIA_BROTHER_BP71:
	case MEDIA_OTHER_PHOTO:
	case MEDIA_HAGAKI_REVERSE:
	case MEDIA_VARIANT_28:
		return PROFILE_PHOTO;
	case MEDIA_INKJET:
	case MEDIA_BROTHER_BP60:
		return PROFILE_INKJET;
	case MEDIA_VARIANT_17:
		return PROFILE_MEDIA_17;
	case MEDIA_VARIANT_1C:
		return PROFILE_MEDIA_1C;
	case MEDIA_VARIANT_1D:
		return PROFILE_MEDIA_1D;
	case MEDIA_PLAIN:
	case MEDIA_HAGAKI_ADDRESS:
		/* Drying and duplex override the special-paper selection. */
		if (selection->slow_drying == SLOW_DRYING_ENABLED ||
		    selection->binding != BINDING_SIMPLEX)
			return PROFILE_SLOW_DRYING;
		if (uses_special_paper_profile(selection->paper_id))
			return PROFILE_SPECIAL_PAPER;
		return 0;
	default:
		return 0;
	}
}

static const struct halftone_profile *
find_profile(const struct halftone_profile *profiles, size_t count,
	     uint32_t key)
{
	size_t index;

	/* Duplicate keys use the first match; unknown keys use entry zero. */
	for (index = 0; index < count; index++)
		if (profiles[index].key == key)
			return &profiles[index];
	return &profiles[0];
}

static void apply_profile_channels(struct halftone_parameters *halftone,
				   const struct halftone_profile *profile,
				   bool dither)
{
	unsigned int channel;

	for (channel = 0; channel < INK_CHANNEL_COUNT; channel++) {
		const struct profile_channel *input =
			&profile->channels[channel];
		struct channel_parameters *output =
			&halftone->channels[channel];

		output->thresholds = (struct drop_levels) {
			.black = input->black_threshold,
			.small = input->small_threshold,
			.medium = input->medium_threshold,
			.large = input->large_threshold,
		};
		output->base_drops = (struct drop_levels) {
			.black = input->black_drop,
			.small = input->small_drop,
			.medium = input->medium_drop,
			.large = input->large_drop,
		};
		output->active_drops = output->base_drops;
		/* Dither preserves the previous diffusion error limits. */
		if (!dither) {
			output->threshold_error = input->threshold_error;
			output->drop_error = input->drop_error;
		}
	}
	if (!dither) {
		halftone->noise_threshold = DEFAULT_NOISE_THRESHOLD;
		halftone->noise_offset = DEFAULT_NOISE_OFFSET;
		halftone->noise_floor = DEFAULT_NOISE_FLOOR;
		halftone->noise_shift = DEFAULT_NOISE_SHIFT;
	}
}

void configure_halftone_channels(struct printer_state *printer)
{
	struct halftone_selection *selection = (void *)printer->numeric_state;
	struct printer_buffers *buffers = &printer->buffers;
	struct halftone_parameters *halftone = (void *)buffers->halftone;
	bool dither = selection->method == HALFTONE_DITHER;
	uint32_t key = selection->raster_mode & HALFTONE_PROFILE_MASK;
	const struct halftone_profile *profile;

	if (dither) {
		profile = find_profile(lpr_dither_profiles,
				       DITHER_PROFILE_COUNT, key);
	} else {
		key += diffusion_variant(selection);
		profile = find_profile(lpr_diffusion_profiles,
				       DIFFUSION_PROFILE_COUNT, key);
	}
	buffers->render_band = profile->render_band;
	selection->plane_stride = profile->plane_stride;
	selection->pixel_scale = profile->pixel_scale;
	/* BCM thresholds, drop levels and noise settings take precedence. */
	if (!halftone->has_calibration)
		apply_profile_channels(halftone, profile, dither);
	if (selection->output_mode == OUTPUT_COLOR)
		halftone->gray_mode = GRAY_DISABLED;
	else if ((selection->raster_mode & RASTER_CHANNEL_MASK) ==
		 RASTER_CHANNEL_MASK)
		halftone->gray_mode = GRAY_FROM_COLOR_TABLE;
	else
		halftone->gray_mode = GRAY_DIRECT;
}
