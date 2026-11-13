/* Decode the MFC-J5720DW maintenance OID without trusting response lengths. */
#include <endian.h>
#include <stdint.h>
#include <string.h>

#include "ink.h"

/* Each record is an identifier, two format bytes and a big-endian value. */
enum maintenance_format {
	RECORD_FIELD_OFFSET = 0,
	RECORD_FORMAT_OFFSET = 1,
	RECORD_LENGTH_OFFSET = 2,
	RECORD_VALUE_OFFSET = 3,
	RECORD_VALUE_BYTES = 4,
	MAINTENANCE_RECORD_BYTES = RECORD_VALUE_OFFSET + RECORD_VALUE_BYTES,
	MAINTENANCE_FORMAT_MARKER = 0x01,
	MAINTENANCE_TERMINATOR = 0xff,
	FIRST_INK_FIELD = 0x6f,
	LAST_INK_FIELD = 0x72,
	FULL_INK_CENTIPERCENT = 10000,
	ALL_INK_COLORS = (1U << BROTHER_INK_COLORS) - 1,
	/* Decoder storage limit, not a claimed maximum for the printer firmware. */
	MAX_MAINTENANCE_BYTES = 1024,
};

static int hex_digit(unsigned char value)
{
	if (value >= '0' && value <= '9')
		return value - '0';
	if (value >= 'a' && value <= 'f')
		return value - 'a' + 10;
	if (value >= 'A' && value <= 'F')
		return value - 'A' + 10;
	return -1;
}

/* CUPS hex replies contain adjacent pairs, with no whitespace or trailing NUL. */
static int decode_hex_frame(const unsigned char *text, size_t text_bytes,
			    unsigned char frame[MAX_MAINTENANCE_BYTES])
{
	size_t offset;

	if (text_bytes % 2 || text_bytes / 2 > MAX_MAINTENANCE_BYTES)
		return -1;
	for (offset = 0; offset < text_bytes; offset += 2) {
		int high = hex_digit(text[offset]);
		int low = hex_digit(text[offset + 1]);

		if (high < 0 || low < 0)
			return -1;
		frame[offset / 2] = (unsigned char)((high << 4) | low);
	}
	return 0;
}

static uint32_t maintenance_value(const unsigned char *record)
{
	uint32_t value;

	/* Seven-byte records do not guarantee alignment for the four-byte value. */
	memcpy(&value, record + RECORD_VALUE_OFFSET, sizeof(value));
	return be32toh(value);
}

int brother_decode_ink(const unsigned char *data, size_t size,
		       unsigned int levels[BROTHER_INK_COLORS])
{
	unsigned char decoded[MAX_MAINTENANCE_BYTES];
	unsigned int parsed_levels[BROTHER_INK_COLORS] = {0};
	unsigned int seen_colors = 0;
	size_t offset;

	if (!size)
		return -1;
	/* CUPS returns either an OCTET STRING or its contiguous hex encoding. */
	if (data[size - 1] != MAINTENANCE_TERMINATOR) {
		if (decode_hex_frame(data, size, decoded))
			return -1;
		size /= 2;
		data = decoded;
	}
	if (!size || size > MAX_MAINTENANCE_BYTES || data[size - 1] != MAINTENANCE_TERMINATOR ||
	    (size - 1) % MAINTENANCE_RECORD_BYTES)
		return -1;

	/* A terminator inside a record's value is ordinary data, not a boundary. */
	for (offset = 0; offset < size - 1; offset += MAINTENANCE_RECORD_BYTES) {
		const unsigned char *record = data + offset;
		unsigned int field = record[RECORD_FIELD_OFFSET];
		unsigned int color;
		uint32_t centipercent;

		/* Validate unrelated records too, before ignoring their unknown values. */
		if (record[RECORD_FORMAT_OFFSET] != MAINTENANCE_FORMAT_MARKER ||
		    record[RECORD_LENGTH_OFFSET] != RECORD_VALUE_BYTES)
			return -1;
		if (field < FIRST_INK_FIELD || field > LAST_INK_FIELD)
			continue;
		color = field - FIRST_INK_FIELD;
		centipercent = maintenance_value(record);
		if (centipercent > FULL_INK_CENTIPERCENT || (seen_colors & (1U << color)))
			return -1;
		seen_colors |= 1U << color;
		parsed_levels[color] = centipercent;
	}
	if (seen_colors != ALL_INK_COLORS)
		return -1;
	/* Commit all four measurements together; never expose a partial frame. */
	memcpy(levels, parsed_levels, sizeof(parsed_levels));
	return 0;
}
