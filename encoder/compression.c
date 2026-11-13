/* Raster row compression and framing for the Brother print protocol. */
#include "compression.h"
#include "packbits.h"
#include "state.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* These are the driver's selectors, not the values in the M command. */
enum row_compression {
	ROW_UNCOMPRESSED = 0,
	ROW_ZERO_RUNS = 2,
	ROW_WORD_RUNS = 3,
	ROW_RANGE_CODED = 5,
};

/* M 08: five bits of zero count, three bits of literal count, then literals. */
static size_t encode_zero_runs(uint8_t *output, const uint8_t *input,
			       size_t length)
{
	uint8_t *destination = output;
	size_t position = 0;

	while (position < length) {
		size_t zeros = 0, literals = 0, start;

		while (position < length && !input[position] && zeros < 31) {
			position++;
			zeros++;
		}
		start = position;
		/* A full zero run is emitted before considering any literals. */
		if (zeros < 31) {
			while (position < length && input[position] && literals < 7) {
				position++;
				literals++;
			}
		}
		*destination++ = (literals << 5) | zeros;
		memcpy(destination, input + start, literals);
		destination += literals;
	}
	return destination - output;
}

/* Both M 06 and M 07 use count - 1 followed by verbatim literal bytes. */
static uint8_t *write_literals(uint8_t *output, const uint8_t *input,
			       size_t length)
{
	while (length) {
		size_t count = length > 128 ? 128 : length;

		*output++ = count - 1;
		memcpy(output, input, count);
		output += count;
		input += count;
		length -= count;
	}
	return output;
}

/* M 06: repeat a one-, two- or four-byte pattern up to 32 times. */
static uint8_t *write_pattern(uint8_t *output, const uint8_t *pattern,
			      size_t width, size_t count)
{
	*output++ = 0x80 | ((count - 1) << 2) | (width - 1);
	memcpy(output, pattern, width);
	return output + width;
}

static uint8_t *write_repeated_words(uint8_t *output, const uint8_t *word,
				     size_t length)
{
	size_t width = 4;

	if (!memcmp(word, word + 2, 2))
		width = word[0] == word[1] ? 1 : 2;

	while (length) {
		size_t block_width = width, count, bytes;

		/* Preserve the original choice of wider patterns for long runs. */
		if (length >= 128)
			block_width = 4;
		else if (width == 1 && length >= 64)
			block_width = 2;
		count = length / block_width;
		if (count > 32)
			count = 32;
		bytes = count * block_width;
		output = write_pattern(output, word, block_width, count);
		length -= bytes;
	}
	return output;
}

/* Encode the bytes between word runs, using byte repeats from length two. */
static uint8_t *write_byte_runs(uint8_t *output, const uint8_t *input,
			       size_t length)
{
	size_t position = 0, literal_start = 0;

	while (position < length) {
		size_t count = 1;

		while (position + count < length &&
		       input[position + count] == input[position])
			count++;
		if (count == 1) {
			position++;
			continue;
		}
		output = write_literals(output, input + literal_start,
					position - literal_start);
		position += count;
		while (count) {
			size_t chunk = count > 32 ? 32 : count;

			output = write_pattern(output, input + position - 1,
					       1, chunk);
			count -= chunk;
		}
		literal_start = position;
	}
	return write_literals(output, input + literal_start,
			      length - literal_start);
}

static size_t encode_word_runs(uint8_t *output, const uint8_t *input,
			       size_t length)
{
	const uint8_t *end = input + length;
	uint8_t *destination = output;
	size_t remaining = length;

	while (remaining > 8) {
		if (!memcmp(input, input + 4, 4)) {
			size_t bytes = 8;

			remaining -= 8;
			while (remaining > 4 && !memcmp(input, input + bytes, 4)) {
				bytes += 4;
				remaining -= 4;
			}
			destination = write_repeated_words(destination, input, bytes);
			input += bytes;
		} else {
			const uint8_t *literal_end = input;
			const uint8_t *lookahead = input + 8;

			/*
			 * Keep the original word lookahead and literal boundaries.
			 * The lookahead budget excludes one word; the final tail
			 * must therefore use end, not remaining, as its boundary.
			 */
			remaining -= 4;
			while (remaining > 8) {
				literal_end = lookahead - 4;
				if (!memcmp(lookahead - 4, lookahead, 4))
					break;
				remaining -= 4;
				lookahead += 4;
			}
			destination = write_byte_runs(destination, input,
						      literal_end - input);
			input = literal_end;
		}
	}
	if (remaining)
		destination = write_byte_runs(destination, input, end - input);
	return destination - output;
}

/* Reverse packed pixels without reversing the bits within each pixel. */
static uint8_t mirror_byte(uint8_t sample, uint16_t mode)
{
	uint16_t depth = mode & 0xc000;

	if (depth != 0x4000 && depth != 0x8000)
		sample = ((sample & 0x55) << 1) | ((sample >> 1) & 0x55);
	if (depth != 0x8000)
		sample = ((sample & 0x33) << 2) | ((sample >> 2) & 0x33);
	return (sample << 4) | (sample >> 4);
}

/* Input and output belong to separate row buffers. */
static void mirror_row(uint8_t *output, const uint8_t *input, size_t length,
			uint16_t mode)
{
	size_t index;

	for (index = 0; index < length; index++)
		output[index] = mirror_byte(input[length - index - 1], mode);
}

uint32_t append_compressed_row(struct output_swath *swath, intptr_t input_address,
			       uint32_t length, struct raster_pass *pass)
{
	const uint8_t *input = (const uint8_t *)input_address;
	size_t row_length = (uint16_t)length;
	uint8_t *packet = pass->encoded_row;
	size_t encoded_length;

	if (pass->mirrored && (pass->compression == ROW_ZERO_RUNS ||
			       pass->compression == ROW_WORD_RUNS)) {
		mirror_row(pass->mirrored_row, input, row_length, pass->mode);
		input = pass->mirrored_row;
	}

	if (pass->compression == ROW_RANGE_CODED) {
		size_t padding = pass->padded_row_bytes - pass->output_bytes;

		/* M 0c collects padded rows; range_coder.c frames the swath. */
		if (pass->mirrored)
			mirror_row((uint8_t *)swath->cursor, input, row_length,
				   pass->mode);
		else
			memcpy((void *)swath->cursor, input, row_length);
		swath->cursor += row_length;
		memset((void *)swath->cursor, 0, padding);
		swath->cursor += padding;
		return 1;
	}

	switch (pass->compression) {
	case ROW_UNCOMPRESSED:
		/* Write directly to the swath, including its three-byte G header. */
		packet = (uint8_t *)swath->cursor;
		if (pass->mirrored)
			mirror_row(packet + 3, input, row_length, pass->mode);
		else
			memcpy(packet + 3, input, row_length);
		encoded_length = row_length;
		break;
	case ROW_ZERO_RUNS:
		encoded_length = encode_zero_runs(packet + 3, input, row_length);
		break;
	case ROW_WORD_RUNS:
		encoded_length = encode_word_runs(packet + 3, input, row_length);
		break;
	default: {
		uint8_t *end;

		/* M 07 mirrors individual bits, regardless of pass->mode. */
		if (pass->mirrored) {
			mirror_row(pass->mirrored_row, input, row_length, 0);
			input = pass->mirrored_row;
		}
		stp_pack_tiff(NULL, input, row_length, packet + 3, &end, NULL, NULL);
		encoded_length = end - (packet + 3);
		break;
	}
	}

	/* G carries a little-endian 16-bit payload length in these modes. */
	packet[0] = 'G';
	packet[1] = encoded_length & 0xff;
	packet[2] = encoded_length >> 8;
	if (pass->compression != ROW_UNCOMPRESSED)
		memcpy((void *)swath->cursor, packet, encoded_length + 3);
	swath->cursor += encoded_length + 3;
	return 1;
}

uint16_t count_leading_zero_bytes(char *input, uint16_t length)
{
	uint16_t count = 0;

	while (count < length && !input[count])
		count++;
	return count;
}

uint16_t trim_trailing_zero_bytes(intptr_t input_address, uint16_t length)
{
	const uint8_t *input = (const uint8_t *)input_address;

	while (length && !input[length - 1])
		length--;
	return length;
}

void count_packed_symbols(intptr_t input_address, uint16_t length,
			  uint32_t bits_per_symbol, int *counts)
{
	const uint8_t *input = (const uint8_t *)input_address;
	unsigned int symbols_per_byte = 8 / bits_per_symbol;
	unsigned int mask = (1U << bits_per_symbol) - 1;
	size_t index;

	/* The raster pass supplies a symbol depth of 1, 2, 4 or 8 bits. */
	for (index = 0; index < length; index++) {
		unsigned int symbol;

		if (!input[index]) {
			counts[0] += symbols_per_byte;
			continue;
		}
		for (symbol = 0; symbol < symbols_per_byte; symbol++) {
			unsigned int shift = 8 - (symbol + 1) * bits_per_symbol;

			counts[(input[index] >> shift) & mask]++;
		}
	}
}
