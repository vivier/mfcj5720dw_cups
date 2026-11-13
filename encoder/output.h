/* Write printer bytes and append data to prepared output buffers. */
#ifndef BROTHER_ENCODER_OUTPUT_H
#define BROTHER_ENCODER_OUTPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct output_swath;

/* Stream writes return zero on success; append_output_bytes() needs reserved capacity. */
uint32_t write_printer_bytes(const void *buffer, size_t length);
bool write_transfer_terminator(void);
void *append_output_bytes(struct output_swath *swath, void *source, uint16_t length);

#endif
