/* Borderless expansion after the upstream CUPS renderer. */
#ifndef MFCJ5720DW_RASTER_H
#define MFCJ5720DW_RASTER_H

#include <cups/raster.h>
#include <signal.h>

/* Private CUPS Raster fields set by the PageSize/PageRegion PPD code. */
enum {
	BORDERLESS_SCALE_REAL = 0,
	BORDERLESS_DOWNWARD_MM_REAL = 1,
};

int enlarge_raster_rgb(cups_raster_t *raster, const cups_page_header2_t *header,
		       unsigned char *output, unsigned int width, unsigned int height,
		       const volatile sig_atomic_t *canceled);

#endif
