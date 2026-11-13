/* Native job globals, calibration tables and printhead schedule records. */
#ifndef BROTHER_LPR_DATA_H
#define BROTHER_LPR_DATA_H

#include <stddef.h>
#include <stdint.h>

struct printer_state;
struct raster_band;

/* Preserve the legacy allocation policy values used by memory.c. */
enum driver_family {
	DRIVER_FAMILY_UNKNOWN = 0,
	DRIVER_FAMILY_MFCJ5720DW = 4,
	DRIVER_FAMILY_CALLER_OWNED = 10,
};

/* Native job state, defined in globals.c. */
extern uint32_t lpr_driver_family;
extern uint16_t *lpr_input_band;
extern char *lpr_jpeg_job_identifier;

/* Borrowed byte spans; callbacks return zero on success, nonzero on failure. */
extern int (*lpr_write_callback)(const void *bytes, size_t length);
extern int (*lpr_transfer_end_callback)(const void *bytes, size_t length, unsigned int tag);

/* Workspace owners; fields in printer_buffers may point inside these blocks. */
extern void *lpr_swath_workspace;
extern void *lpr_color_workspace;
extern void *lpr_band_workspace;
extern void *lpr_feed_workspace;
extern void *lpr_correction_workspace;
extern void *lpr_enhancement_workspace;
extern struct printer_state *lpr_printer_state;
extern uint16_t *lpr_device_options;
extern struct raster_band *lpr_band_descriptor;

/* Native calibration data. Array order and numeric values match the reference. */
enum native_table_limits {
	SWATH_LAYOUT_COUNT = 50,
	DITHER_PROFILE_COUNT = 27,
	DIFFUSION_PROFILE_COUNT = 55,
	COLOR_CUBE_EDGE = 17,
	PROFILE_PATH_BYTES = 1024,
	LOADED_PAPER_COUNT = 42,
};

struct cube_weights {
	uint16_t total;
	uint16_t corner[8];
};

struct scheduled_row {
	uint16_t swath;
	int16_t row;
};

struct raster_pass;

struct swath_layout {
	uint16_t profile;
	uint16_t resolution;
	uint16_t channels;
	uint8_t interlace;
	uint8_t reserved;
	void (*buffer_row)(struct printer_state *printer, struct raster_pass *pass);
	const struct scheduled_row *const *row_schedule;
	const uint16_t *feed_schedule;
	const uint16_t *swath_order;
	const uint16_t *padding_schedule;
	const int16_t *pass_lengths;
	uint16_t nozzle_rows;
	uint16_t feed_step;
	uint16_t row_step;
	uint16_t pass_count;
	int16_t first_row;
	uint16_t pass_rows;
	int32_t channel_offsets[4];
	uint16_t leading_rows;
	uint16_t trailing_rows;
	uint32_t flags;
	int32_t feed_rows;
};

struct profile_channel {
	uint16_t small_threshold;
	uint16_t medium_threshold;
	uint16_t large_threshold;
	uint16_t small_drop;
	uint16_t medium_drop;
	uint16_t large_drop;
	uint16_t threshold_error;
	uint16_t drop_error;
	uint16_t black_threshold;
	uint16_t black_drop;
};

struct halftone_profile {
	uint32_t key;
	uint32_t (*render_band)(struct printer_state *printer, int first_row, uint32_t width,
				struct raster_band *band, uint32_t row_count, int first_column,
				int column_count);
	uint32_t plane_stride;
	uint32_t pixel_scale;
	struct profile_channel channels[4];
};

struct calibration_selector {
	uint8_t media_class;
	uint8_t reserved;
	uint16_t profile;
	uint16_t index;
};

struct paper_id_entry {
	int16_t id;
	char name[32];
};

struct loaded_paper {
	char name[64];
	int16_t width;
	int16_t height;
};

struct custom_paper_record {
	char name[64];
	double horizontal_margin;
	double vertical_margin;
};

extern const struct swath_layout *const lpr_swath_layouts;
extern const struct halftone_profile *const lpr_dither_profiles;
extern const struct halftone_profile *const lpr_diffusion_profiles;

/* Writable job metadata is separate from immutable calibration tables. */
extern uint32_t lpr_input_band_bytes;
extern char lpr_color_table_directory[PROFILE_PATH_BYTES];
extern struct loaded_paper lpr_loaded_papers[LOADED_PAPER_COUNT];

/* Signed 5x5 enhancement kernels. */
extern const int32_t lpr_enhancement_kernel_soft[5][5];
extern const int32_t lpr_enhancement_kernel_medium[5][5];
extern const int32_t lpr_enhancement_kernel_sharp[5][5];
/* Five-pass masks and complete wire-command fragments. */
extern const uint8_t lpr_alternate_pass_masks[360];
extern const uint8_t lpr_command_direction_forward[2];
extern const uint8_t lpr_command_direction_reverse[2];
extern const uint8_t lpr_command_flush_swath[5];
/* RGB fractions 0..16: one bank per blue fraction, then green and red. */
extern const struct cube_weights lpr_interpolation_bank_0[17][17];
extern const struct cube_weights lpr_interpolation_bank_1[17][17];
extern const struct cube_weights lpr_interpolation_bank_2[17][17];
extern const struct cube_weights lpr_interpolation_bank_3[17][17];
extern const struct cube_weights lpr_interpolation_bank_4[17][17];
extern const struct cube_weights lpr_interpolation_bank_5[17][17];
extern const struct cube_weights lpr_interpolation_bank_6[17][17];
extern const struct cube_weights lpr_interpolation_bank_7[17][17];
extern const struct cube_weights lpr_interpolation_bank_8[17][17];
extern const struct cube_weights lpr_interpolation_bank_9[17][17];
extern const struct cube_weights lpr_interpolation_bank_10[17][17];
extern const struct cube_weights lpr_interpolation_bank_11[17][17];
extern const struct cube_weights lpr_interpolation_bank_12[17][17];
extern const struct cube_weights lpr_interpolation_bank_13[17][17];
extern const struct cube_weights lpr_interpolation_bank_14[17][17];
extern const struct cube_weights lpr_interpolation_bank_15[17][17];
extern const struct cube_weights lpr_interpolation_bank_16[17][17];
/* Swath masking noise and CMYK channel emission order. */
extern const int16_t lpr_noise_table[1024];
extern const uint16_t lpr_raster_channel_order[4];
extern const uint16_t lpr_jpeg_channel_order[4];
/* Six media rows of four quality choices and the wireless-JPEG lookup. */
extern const uint16_t lpr_quality_modes[24];
extern const int16_t lpr_media_modes[5];
/* RGB mapping, nine 256-sample curves and 17x17x17 CMYK fallback cubes. */
extern const uint8_t lpr_color_channel_map[256];
extern const uint16_t lpr_fallback_photo_curves[9][256];
extern const uint16_t lpr_fallback_plain_curves[9][256];
extern const uint16_t lpr_fallback_color_bank_0[4913][4];
extern const uint16_t lpr_fallback_color_bank_1[4913][4];
extern const uint16_t lpr_fallback_color_bank_2[4913][4];
extern const uint16_t lpr_fallback_color_bank_3[4913][4];
extern const uint16_t lpr_fallback_color_bank_4[4913][4];
extern const uint16_t lpr_fallback_color_bank_5[4913][4];
extern const uint16_t lpr_fallback_color_bank_6[4913][4];
extern const uint16_t lpr_fallback_color_bank_7[4913][4];
extern const uint16_t lpr_fallback_color_bank_8[4913][4];
/* Diffusion initial errors, minimum ink thresholds and 256x256 noise. */
extern const uint16_t lpr_cyan_initial_errors[256];
extern const uint16_t lpr_magenta_initial_errors[256];
extern const uint16_t lpr_yellow_initial_errors[256];
extern const uint16_t lpr_black_initial_errors[256];
extern const int16_t lpr_minimum_diffusion_thresholds[1024];
extern const int16_t lpr_diffusion_noise_forward[256][256];
extern const int16_t lpr_photo_noise_forward[256][256];
/* K/C/M/Y matrices: 128 rows, 128 columns and four thresholds per cell. */
extern const uint16_t lpr_dither_matrix_bank[4][128][128][4];
extern const uint16_t lpr_photo_dither_matrix_bank[4][128][128][4];
/* Printhead selectors and paper records preserve the reference lookup order. */
extern const struct calibration_selector lpr_head_parameter_selectors[28];
extern const struct calibration_selector lpr_density_selectors[20];
extern const struct paper_id_entry lpr_paper_id_table[42];
extern const struct custom_paper_record lpr_custom_paper_records[1];

#endif
