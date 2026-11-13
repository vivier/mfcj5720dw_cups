/* Write printer commands and append encoded bytes to output swaths. */
#include "output.h"
#include "protocol.h"
#include "state.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Output callbacks return zero on success and nonzero on write failure. */
uint32_t write_printer_bytes(const void *buffer, size_t length)
{
	if (!length)
		return 0;
	return fwrite(buffer, length, 1, stdout) != 1;
}

bool write_transfer_terminator(void)
{
	static const uint8_t command = COMMAND_FEED_COMPLETION;

	return fwrite(&command, sizeof(command), 1, stdout) != 1;
}

/* The caller reserves capacity; return the start of the appended bytes. */
void *append_output_bytes(struct output_swath *swath, void *source, uint16_t length)
{
	void *destination = (void *)swath->cursor;

	memcpy(destination, source, length);
	swath->cursor += length;
	return destination;
}
