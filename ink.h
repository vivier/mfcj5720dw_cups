/* Brother maintenance data used for CUPS supply reporting. */
#ifndef BROTHER_INK_H
#define BROTHER_INK_H

#include <stddef.h>

#define BROTHER_INK_COLORS 4

/*
 * Decode raw maintenance bytes or contiguous hexadecimal text (size excludes
 * any string terminator). Return 0 with all four centipercent values in black,
 * cyan, magenta, yellow order, or -1 without changing levels on invalid input.
 */
int brother_decode_ink(const unsigned char *data, size_t size,
		       unsigned int levels[BROTHER_INK_COLORS]);

/*
 * Query the CUPS backend and publish marker attributes/states on stderr.
 * Return 0 after reporting or 1 if unavailable, without publishing new levels.
 * No printer data is written to stdout.
 */
int brother_report_ink_levels(void);

#endif
