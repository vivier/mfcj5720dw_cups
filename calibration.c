/* Read the MFC-J5720DW factory calibration without changing printer settings. */
#include <stdio.h>
#include <string.h>

#include "calibration.h"

enum calibration_protocol {
	MAX_TABLE_ENTRIES = 64,
	MAX_CHUNK_BYTES = 210,
	OID_BYTES = 96,
	GAIN_SETS = 8,
	GAIN_LEVELS = 4,
	GAIN_HEADER_BYTES = GAIN_SETS * CALIBRATION_CHANNELS * GAIN_LEVELS,
	PARAMETER_RECORDS = 45,
	PARAMETER_RECORD_BYTES = 1 + CALIBRATION_CHANNELS * GAIN_LEVELS,
};

static const char calibration_oid[] = ".1.3.6.1.4.1.2435.2.3.9.4.2.1.5.1.2.67";

/* All table counts and identifiers in this printer fit in the range 1..64. */
static int query_integer(calibration_query_fn query, void *context,
			 const char *oid)
{
	char reply[16];
	size_t length = sizeof(reply);
	unsigned int value = 0;

	if (query(context, oid, reply, &length) || !length || length > sizeof(reply))
		return -1;
	for (size_t offset = 0; offset < length; offset++) {
		if (reply[offset] < '0' || reply[offset] > '9')
			return -1;
		value = value * 10 + reply[offset] - '0';
		if (value > MAX_TABLE_ENTRIES)
			return -1;
	}
	return value ? (int)value : -1;
}

static int hex_digit(unsigned char character)
{
	if (character >= '0' && character <= '9')
		return character - '0';
	if (character >= 'a' && character <= 'f')
		return character - 'a' + 10;
	if (character >= 'A' && character <= 'F')
		return character - 'A' + 10;
	return -1;
}

/* The OCTET STRING contains ASCII hex; its last decoded byte is a checksum. */
static int query_chunk(calibration_query_fn query, void *context,
		       const char *oid, uint8_t *output, size_t available)
{
	char reply[2 * (MAX_CHUNK_BYTES + 1)];
	uint8_t decoded[MAX_CHUNK_BYTES + 1];
	size_t length = sizeof(reply);
	uint8_t checksum = 0;

	if (query(context, oid, reply, &length) || length < 4 ||
	    length > sizeof(reply) || length % 2)
		return -1;
	for (size_t offset = 0; offset < length / 2; offset++) {
		int high = hex_digit(reply[offset * 2]);
		int low = hex_digit(reply[offset * 2 + 1]);

		if (high < 0 || low < 0)
			return -1;
		decoded[offset] = high * 16 + low;
		if (offset + 1 < length / 2)
			checksum += decoded[offset];
	}
	length = length / 2 - 1;
	if (checksum != decoded[length] || length > available)
		return -1;
	memcpy(output, decoded, length);
	return (int)length;
}

static int read_table(calibration_query_fn query, void *context,
		      unsigned int group, unsigned int bank, unsigned int column,
		      unsigned int count, uint8_t *output, size_t expected)
{
	bool seen[MAX_TABLE_ENTRIES + 1] = {false};
	size_t received = 0;
	char oid[OID_BYTES];

	for (unsigned int ordinal = 1; ordinal <= count; ordinal++) {
		int index, length;

		if (bank)
			snprintf(oid, sizeof(oid), "%s.%u.2.1.1.%u.%u",
				 calibration_oid, group, bank, ordinal);
		else
			snprintf(oid, sizeof(oid), "%s.%u.2.1.1.%u",
				 calibration_oid, group, ordinal);
		index = query_integer(query, context, oid);
		if (index < 0 || seen[index])
			return -1;
		seen[index] = true;

		if (bank)
			snprintf(oid, sizeof(oid), "%s.%u.2.1.%u.%u.%u",
				 calibration_oid, group, column, bank, index);
		else
			snprintf(oid, sizeof(oid), "%s.%u.2.1.%u.%u",
				 calibration_oid, group, column, index);
		length = query_chunk(query, context, oid, output + received,
				     expected - received);
		if (length < 0)
			return -1;
		received += length;
	}
	return received == expected ? 0 : -1;
}

bool calibration_valid(const struct printer_calibration *calibration)
{
	if (!calibration)
		return false;
	for (unsigned int record = 0; record < PARAMETER_RECORDS; record++) {
		const uint8_t *parameters = calibration->parameters +
			GAIN_HEADER_BYTES + record * PARAMETER_RECORD_BYTES;

		if (parameters[0] >= GAIN_SETS)
			return false;
		for (unsigned int offset = 1; offset < PARAMETER_RECORD_BYTES; offset++)
			if (parameters[offset] >= CALIBRATION_SETS)
				return false;
	}
	/* Every nozzle byte is valid; density values are clamped by the encoder. */
	return true;
}

int calibration_fetch(struct printer_calibration *calibration,
		      calibration_query_fn query, void *context)
{
	const unsigned int groups[] = {1, 2, 3, 4, 5, 11, 12, 13};
	struct printer_calibration snapshot;
	bool seen[MAX_TABLE_ENTRIES + 1] = {false};
	unsigned int counts[14] = {0};
	char oid[OID_BYTES];

	if (!calibration || !query)
		return -1;
	for (size_t table = 0; table < sizeof(groups) / sizeof(groups[0]); table++) {
		unsigned int group = groups[table];
		int count;

		snprintf(oid, sizeof(oid), "%s.%u.1.0", calibration_oid, group);
		count = query_integer(query, context, oid);
		if (count < 0)
			return -1;
		counts[group] = count;
	}
	if (counts[1] != CALIBRATION_SETS)
		return -1;

	for (unsigned int set = 0; set < CALIBRATION_SETS; set++) {
		int bank;

		snprintf(oid, sizeof(oid), "%s.1.2.1.1.%u", calibration_oid, set + 1);
		bank = query_integer(query, context, oid);
		if (bank < 0 || seen[bank])
			return -1;
		seen[bank] = true;
		/* Groups 2..5 are black, cyan, magenta and yellow, in that order. */
		for (unsigned int channel = 0; channel < CALIBRATION_CHANNELS; channel++)
			if (read_table(query, context, channel + 2, bank, 2,
				       counts[channel + 2], snapshot.nozzle[set][channel],
				       CALIBRATION_SAMPLES))
				return -1;
	}
	if (read_table(query, context, 11, 0, 2, counts[11], snapshot.parameters,
		       GAIN_HEADER_BYTES) ||
	    read_table(query, context, 12, 0, 3, counts[12],
		       snapshot.parameters + GAIN_HEADER_BYTES,
		       CALIBRATION_PARAMETERS - GAIN_HEADER_BYTES) ||
	    read_table(query, context, 13, 0, 2, counts[13],
		       &snapshot.density[0][0][0], sizeof(snapshot.density)) ||
	    !calibration_valid(&snapshot))
		return -1;

	/* A failed query must never replace even part of a usable calibration. */
	*calibration = snapshot;
	return 0;
}
