/* Job-owned pointers use the native ABI, separate from retained data tables. */
#include "data.h"

/* CUPS supplies the input band; the JPEG identifier is borrowed job metadata. */
uint32_t lpr_driver_family = DRIVER_FAMILY_MFCJ5720DW;
uint16_t *lpr_input_band;
char *lpr_jpeg_job_identifier;

/* Installed by start_printer_job(), with distinct output/transfer signatures. */
int (*lpr_write_callback)(const void *bytes, size_t length);
int (*lpr_transfer_end_callback)(const void *bytes, size_t length, unsigned int tag);

/* Raw allocations contain multiple typed regions; release through their owners. */
void *lpr_swath_workspace;
void *lpr_color_workspace;
void *lpr_band_workspace;
void *lpr_feed_workspace;
void *lpr_correction_workspace;
void *lpr_enhancement_workspace;

/* Job records allocated and released together by memory.c. */
struct printer_state *lpr_printer_state;
uint16_t *lpr_device_options;
struct raster_band *lpr_band_descriptor;

/* Native storage for the raster input size, profile path and optional paper cache. */
uint32_t lpr_input_band_bytes;
char lpr_color_table_directory[PROFILE_PATH_BYTES];
struct loaded_paper lpr_loaded_papers[LOADED_PAPER_COUNT];
