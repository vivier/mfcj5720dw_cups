/* Printer-state ownership and allocation of raster workspaces. */
#ifndef BROTHER_ENCODER_MEMORY_H
#define BROTHER_ENCODER_MEMORY_H

#include <stdbool.h>
#include <stdint.h>

struct page_state;
struct printer_state;

/* Partial allocations remain owned until free_printer_state() is called. */
uint32_t allocate_raster_buffers(struct page_state *page);
uint32_t allocate_printer_state(struct page_state *page);
void free_printer_state(void);
bool allocate_feed_buffer(struct printer_state *printer);
void free_feed_buffer(void);
bool allocate_row_correction_buffers(struct printer_state *printer);
void free_row_correction_buffers(void);

#endif
