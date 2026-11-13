/* CUPS-facing interface to the reconstructed Brother encoder. */
#ifndef MFCJ5720DW_DRIVER_H
#define MFCJ5720DW_DRIVER_H

#include <cups/cups.h>
#include <cups/ppd.h>
#include <cups/raster.h>

struct job_settings;

struct paper_geometry {
	const char *name;
	int width_points;
	int height_points;
	int width_pixels;
	int height_pixels;
	int left;
	int top;
	int right;
	int bottom;
};

const struct paper_geometry *find_paper(const char *name);
int configure_job(ppd_file_t *ppd, struct job_settings *settings);
int validate_raster_header(const cups_page_header2_t *header,
			   const struct job_settings *settings);

#endif
