/* Encode RGB bands with libjpeg and frame the output for the printer. */
#include "jpeg.h"
#include "data.h"
#include "settings.h"
#include "state.h"

#include <jerror.h>
#include <jpeglib.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JPEG_CHUNK_SIZE 4096

struct jpeg_failure {
	struct jpeg_error_mgr manager;
	jmp_buf recovery;
};

struct jpeg_encoder {
	struct jpeg_compress_struct compressor;
	struct jpeg_failure failure;
	struct jpeg_destination_mgr destination;
	JOCTET output[JPEG_CHUNK_SIZE];
	JSAMPLE *mirrored_row;
	unsigned int next_band;
};

/* A codec error must fail the CUPS job, never exit from inside libjpeg. */
static void jpeg_error_exit(j_common_ptr codec)
{
	struct jpeg_failure *failure = (struct jpeg_failure *)codec->err;
	char message[JMSG_LENGTH_MAX];

	codec->err->format_message(codec, message);
	fprintf(stderr, "ERROR: JPEG encoding failed: %s\n", message);
	longjmp(failure->recovery, 1);
}

/* G <length:u24le> carries JPEG bytes; protocol.c owns the page terminator. */
static void write_jpeg_chunk(j_compress_ptr codec, size_t length)
{
	struct jpeg_encoder *encoder = codec->client_data;
	uint8_t header[] = { 'G', length, length >> 8, length >> 16 };

	if (!length)
		return;
	if (lpr_write_callback(header, sizeof(header)) ||
	    lpr_write_callback(encoder->output, length))
		ERREXIT(codec, JERR_FILE_WRITE);
}

static void initialize_destination(j_compress_ptr codec)
{
	struct jpeg_encoder *encoder = codec->client_data;

	codec->dest->next_output_byte = encoder->output;
	codec->dest->free_in_buffer = sizeof(encoder->output);
}

static boolean flush_destination(j_compress_ptr codec)
{
	write_jpeg_chunk(codec, JPEG_CHUNK_SIZE);
	initialize_destination(codec);
	return TRUE;
}

static void finish_destination(j_compress_ptr codec)
{
	write_jpeg_chunk(codec, JPEG_CHUNK_SIZE - codec->dest->free_in_buffer);
}

/* Also called when a job is canceled or fails before its last band. */
void free_jpeg_encoder(struct printer_state *printer)
{
	struct jpeg_encoder *encoder = printer->jpeg;

	if (!encoder)
		return;
	if (encoder->compressor.mem)
		jpeg_destroy_compress(&encoder->compressor);
	free(encoder->mirrored_row);
	free(encoder);
	printer->jpeg = NULL;
}

static void start_jpeg_image(struct jpeg_encoder *encoder, unsigned int width,
			     unsigned int height, unsigned int resolution)
{
	struct jpeg_compress_struct *codec = &encoder->compressor;

	jpeg_create_compress(codec);
	codec->client_data = encoder;
	encoder->destination.init_destination = initialize_destination;
	encoder->destination.empty_output_buffer = flush_destination;
	encoder->destination.term_destination = finish_destination;
	codec->dest = &encoder->destination;
	codec->image_width = width;
	codec->image_height = height;
	codec->input_components = 3;
	codec->in_color_space = JCS_RGB;
	jpeg_set_defaults(codec);

	/* Brother's profile: baseline YCbCr, 4:2:2, Annex K tables scaled by 0.5.
	 * Monochrome is selected by a printer command; its JPEG still has color
	 * components. Let libjpeg perform conversion, downsampling and padding.
	 */
	jpeg_set_linear_quality(codec, 50, TRUE);
	codec->comp_info[0].h_samp_factor = 2;
	codec->comp_info[0].v_samp_factor = 1;
	codec->comp_info[1].h_samp_factor = 1;
	codec->comp_info[1].v_samp_factor = 1;
	codec->comp_info[2].h_samp_factor = 1;
	codec->comp_info[2].v_samp_factor = 1;
	codec->dct_method = JDCT_ISLOW;
	codec->optimize_coding = FALSE;
	codec->arith_code = FALSE;
	codec->restart_interval = 0;
	codec->write_JFIF_header = TRUE;
	codec->JFIF_major_version = 1;
	codec->JFIF_minor_version = 0;
	codec->density_unit = 1;
	codec->X_density = resolution;
	codec->Y_density = resolution;
	codec->write_Adobe_marker = FALSE;
	jpeg_start_compress(codec, TRUE);
}

static int encode_jpeg_rows(struct jpeg_encoder *encoder,
			    struct raster_band *band, unsigned int rows,
			    unsigned int orientation, unsigned int height,
			    unsigned int resolution, bool last_band)
{
	bool mirror = orientation == 1 || orientation == 2;
	unsigned int row;

	/* All libjpeg calls that can report an error share this live recovery
	 * point. Mutable codec state stays on the heap across longjmp().
	 */
	if (setjmp(encoder->failure.recovery))
		return 1;
	if (!encoder->next_band)
		start_jpeg_image(encoder, band->width, height, resolution);
	if (rows > encoder->compressor.image_height -
		   encoder->compressor.next_scanline) {
		fputs("ERROR: Too many rows in JPEG raster band.\n", stderr);
		return 1;
	}
	for (row = 0; row < rows; row++) {
		unsigned int source_row = orientation & 2 ? rows - row - 1 : row;
		JSAMPROW pixels = (uint8_t *)band->pixels +
				 (size_t)source_row * band->row_bytes;
		unsigned int column;

		if (mirror) {
			for (column = 0; column < (unsigned int)band->width; column++)
				memcpy(encoder->mirrored_row + 3 * column,
				       pixels + 3 * (band->width - column - 1), 3);
			pixels = encoder->mirrored_row;
		}
		jpeg_write_scanlines(&encoder->compressor, &pixels, 1);
	}
	if (last_band)
		jpeg_finish_compress(&encoder->compressor);
	return 0;
}

uint32_t render_jpeg_band(struct printer_state *printer, struct raster_band *band)
{
	struct jpeg_encoder *encoder = printer->jpeg;
	unsigned int band_index = *(uint16_t *)(printer->numeric_state + 0x8a4);
	unsigned int band_count = *(uint16_t *)(printer->numeric_state + 0x11f2);
	unsigned int last_rows = *(uint16_t *)(printer->numeric_state + 0x8a8);
	unsigned int orientation = printer->numeric_state[0x89c] & 3;
	unsigned int resolution = *(uint16_t *)(printer->numeric_state + 0x11b0);
	unsigned int rows;
	uint64_t height = 0;

	if (!band_count || band_index >= band_count || band->width <= 0 ||
	    band->width > JPEG_MAX_DIMENSION || band->height <= 0 ||
	    !last_rows || last_rows > (unsigned int)band->height ||
	    band->row_bytes < band->width * 3 || !band->pixels)
		goto invalid_band;
	rows = band_index + 1 == band_count ? last_rows : (unsigned int)band->height;
	if (!band_index) {
		if (encoder)
			goto invalid_band;
		height = (uint64_t)band->height * (band_count - 1) + last_rows;
		if (height > JPEG_MAX_DIMENSION)
			goto invalid_band;
		encoder = calloc(1, sizeof(*encoder));
		if (!encoder)
			goto out_of_memory;
		printer->jpeg = encoder;
		encoder->mirrored_row = malloc((size_t)band->width * 3);
		if (!encoder->mirrored_row)
			goto out_of_memory;
		encoder->compressor.err = jpeg_std_error(&encoder->failure.manager);
		encoder->failure.manager.error_exit = jpeg_error_exit;
	} else if (!encoder || encoder->next_band != band_index ||
		   encoder->compressor.image_width != (unsigned int)band->width) {
		goto invalid_band;
	}

	if (encode_jpeg_rows(encoder, band, rows, orientation, height, resolution,
			     band_index + 1 == band_count))
		goto failure;
	encoder->next_band++;
	if (band_index + 1 == band_count)
		free_jpeg_encoder(printer);
	return 0;

invalid_band:
	fputs("ERROR: Invalid JPEG raster band or band sequence.\n", stderr);
	goto failure;
out_of_memory:
	fputs("ERROR: Cannot allocate JPEG encoder.\n", stderr);
failure:
	free_jpeg_encoder(printer);
	return 1;
}
