/* Convert CUPS Raster to Brother printer data in one process. */
#include "calibration.h"
#include "data.h"
#include "driver.h"
#include "encoder/driver.h"
#include "encoder/memory.h"
#include "encoder/settings.h"
#include "encoder/state.h"
#include "ink.h"

#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum cups_filter_arguments {
	ARG_OPTIONS = 5,
	ARG_INPUT_FILE = 6,
	ARG_COUNT_STDIN = 6,
	ARG_COUNT_FILE = 7,
	EXIT_USAGE = 2,
};

enum raster_layout {
	RED_CHANNEL,
	GREEN_CHANNEL,
	BLUE_CHANNEL,
	RGB_CHANNELS,
	WHITE_SAMPLE = 255,
	COLOR_PROFILE_PATH_CAPACITY = 1024,
};

static volatile sig_atomic_t job_canceled;

static void cancel_job_signal(int signal_number)
{
	(void)signal_number;
	job_canceled = 1;
}

static int write_printer_output(const void *buffer, size_t length)
{
	if (job_canceled)
		return 1;
	return fwrite(buffer, 1, length, stdout) != length;
}

static void usage(FILE *stream)
{
	fputs("Usage: rastertomfcj5720dw job-id user title copies options [file]\n"
	      "       rastertomfcj5720dw -h | --help\n\n"
	      "Read CUPS Raster from file or stdin; write Brother printer data to stdout.\n"
	      "PPD must name the direct raster driver's PPD. CUPS prepares page order,\n"
	      "duplex padding, layout and software copies before this filter.\n"
	      "No LPR command, RC file or printconf helper is used.\n"
	      "See rastertomfcj5720dw(1) for supported input and limitations.\n",
	      stream);
}

/* The encoder always receives interleaved RGB, including for grayscale input. */
static void copy_raster_row(unsigned char *rgb, const unsigned char *input, unsigned int width,
			    unsigned int channels)
{
	unsigned int column;

	if (channels == RGB_CHANNELS) {
		memcpy(rgb, input, width * RGB_CHANNELS);
		return;
	}
	for (column = 0; column < width; column++) {
		rgb[RED_CHANNEL] = input[column];
		rgb[GREEN_CHANNEL] = input[column];
		rgb[BLUE_CHANNEL] = input[column];
		rgb += RGB_CHANNELS;
	}
}

static int read_raster_page(cups_raster_t *raster, const cups_page_header2_t *header,
			    const struct job_settings *settings)
{
	size_t page_bytes = (size_t)settings->row_bytes * settings->height_pixels;
	unsigned char *input_row, *rgb_row;
	unsigned int row_index, copy_width;
	int result = 1;

	/* render_job owns the page buffer, including on a partial allocation failure. */
	lpr_input_band = malloc(page_bytes);
	input_row = malloc(header->cupsBytesPerLine);
	if (!lpr_input_band || !input_row) {
		fputs("ERROR: Cannot allocate raster page.\n", stderr);
		goto out;
	}
	lpr_input_band_bytes = page_bytes;
	/* Fill rounded-up edges with white; crop any extra input columns or rows. */
	memset(lpr_input_band, WHITE_SAMPLE, page_bytes);
	copy_width = header->cupsWidth;
	if (copy_width > (unsigned int)settings->width_pixels)
		copy_width = settings->width_pixels;
	for (row_index = 0; row_index < header->cupsHeight; row_index++) {
		if (job_canceled)
			goto out;
		if (cupsRasterReadPixels(raster, input_row, header->cupsBytesPerLine) !=
		    header->cupsBytesPerLine) {
			fputs("ERROR: Truncated raster page.\n", stderr);
			goto out;
		}
		/* Consume even cropped rows so the next read starts at a page header. */
		if (row_index >= (unsigned int)settings->height_pixels)
			continue;
		rgb_row = (unsigned char *)lpr_input_band + (size_t)row_index * settings->row_bytes;
		copy_raster_row(rgb_row, input_row, copy_width, header->cupsNumColors);
	}
	result = 0;
out:
	free(input_row);
	return result;
}

static int render_job(cups_raster_t *raster, struct job_settings *settings,
		      const struct printer_calibration *calibration)
{
	cups_page_header2_t header;
	struct page_state page = { 0 };
	bool printer_state_owned = false, job_started = false;
	int result = 1, page_number = 0;

	while (!job_canceled && cupsRasterReadHeader2(raster, &header)) {
		if (validate_raster_header(&header, settings) ||
		    read_raster_page(raster, &header, settings))
			goto out;
		if (!printer_state_owned) {
			/* Allocation may fail after acquiring some of the workspaces. */
			printer_state_owned = true;
			if (allocate_printer_state(&page)) {
				fputs("ERROR: Cannot allocate printer state.\n", stderr);
				goto out;
			}
			page.printer->calibration = calibration;
			page.borderless = settings->borderless;
			if (start_printer_job(settings, &page))
				goto out;
			job_started = true;
			lpr_write_callback = write_printer_output;
		}
		/* The entire CUPS page is passed to the encoder as a single input band. */
		page.band_index = 0;
		page_number++;
		fprintf(stderr, "PAGE: %d 1\n", page_number);
		if (start_printer_page(settings, &page, page_number) ||
		    send_printer_band(settings, &page) || end_printer_page(&page)) {
			fputs("ERROR: Brother page encoding failed.\n", stderr);
			goto out;
		}
		free(lpr_input_band);
		lpr_input_band = NULL;
		if (fflush(stdout) || ferror(stdout)) {
			fputs("ERROR: Cannot write printer data.\n", stderr);
			goto out;
		}
	}
	if (!page_number && !job_canceled) {
		fputs("ERROR: No raster pages were received.\n", stderr);
		goto out;
	}
	result = 0;
out:
	/* CUPS pads odd duplex jobs before reversing; never append another side. */
	if (job_started)
		end_printer_job();
	if (fflush(stdout) || ferror(stdout))
		result = 1;
	free(lpr_input_band);
	lpr_input_band = NULL;
	if (printer_state_owned)
		free_printer_state();
	/* A canceled CUPS job is complete, even if a read/write was interrupted. */
	return job_canceled ? 0 : result;
}

static int set_color_profile_directory(void)
{
	const char *directory = getenv("MFCJ5720DW_PROFILE_DIR");
	char *path = lpr_color_table_directory;

	if (!directory)
		directory = DATADIR "/lut";
	/* The retained encoder data reserves this fixed-size path buffer. */
	if (snprintf(path, COLOR_PROFILE_PATH_CAPACITY, "%s/", directory) >=
	    COLOR_PROFILE_PATH_CAPACITY) {
		fputs("ERROR: Color profile directory path is too long.\n", stderr);
		return 1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	struct job_settings settings;
	struct printer_calibration calibration;
	const struct printer_calibration *job_calibration = NULL;
	struct sigaction cancel_action = { 0 };
	cups_option_t *options = NULL;
	cups_raster_t *raster = NULL;
	ppd_file_t *ppd = NULL;
	ppd_choice_t *calibration_choice;
	const char *ppd_path, *device_uri;
	int input_fd = STDIN_FILENO, option_count = 0, result = 1;

	if (argc == 2 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
		usage(stdout);
		return 0;
	}
	if (argc != ARG_COUNT_STDIN && argc != ARG_COUNT_FILE) {
		usage(stderr);
		return EXIT_USAGE;
	}
	setbuf(stderr, NULL);
	cancel_action.sa_handler = cancel_job_signal;
	sigemptyset(&cancel_action.sa_mask);
	sigaction(SIGTERM, &cancel_action, NULL);
	/* Report a closed backend pipe through the normal write-error path. */
	signal(SIGPIPE, SIG_IGN);
	ppd_path = getenv("PPD");
	if (!ppd_path || !(ppd = ppdOpenFile(ppd_path))) {
		fputs("ERROR: Cannot open the direct raster driver's PPD.\n", stderr);
		goto out;
	}
	ppdMarkDefaults(ppd);
	option_count = cupsParseOptions(argv[ARG_OPTIONS], 0, &options);
	cupsMarkOptions(ppd, option_count, options);
	if (ppdConflicts(ppd)) {
		fputs("ERROR: Conflicting printer options.\n", stderr);
		goto out;
	}
	if (configure_job(ppd, &settings))
		goto out;
	if (set_color_profile_directory())
		goto out;
	if (argc == ARG_COUNT_FILE && (input_fd = open(argv[ARG_INPUT_FILE], O_RDONLY)) < 0) {
		perror("ERROR: Cannot open raster input");
		goto out;
	}
	raster = cupsRasterOpen(input_fd, CUPS_RASTER_READ);
	if (!raster) {
		fputs("ERROR: Cannot open CUPS Raster stream.\n", stderr);
		goto out;
	}
	device_uri = getenv("DEVICE_URI");
	if (device_uri && *device_uri) {
		brother_report_ink_levels();
		calibration_choice = ppdFindMarkedChoice(ppd, "BRCalibration");
		/* Queues created with an older PPD also use automatic retrieval. */
		if ((!calibration_choice || strcmp(calibration_choice->choice, "Off")) &&
		    calibration_prepare(&calibration, getenv("PRINTER"), device_uri,
					calibration_choice &&
					!strcmp(calibration_choice->choice, "Refresh")))
			job_calibration = &calibration;
	}
	result = render_job(raster, &settings, job_calibration);
out:
	if (raster)
		cupsRasterClose(raster);
	/* cupsRasterClose releases its decoder; the input descriptor is ours. */
	if (input_fd >= 0 && input_fd != STDIN_FILENO)
		close(input_fd);
	if (ppd)
		ppdClose(ppd);
	cupsFreeOptions(option_count, options);
	return result;
}
