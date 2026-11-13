/* Job/page lifecycle and translation of settings into encoder state. */
#ifndef BROTHER_ENCODER_DRIVER_H
#define BROTHER_ENCODER_DRIVER_H

#include <stdint.h>

struct job_settings;
struct page_state;
struct printer_state;

/* Allocate state before starting a job; release it after finishing or failing. */
uint32_t start_printer_job(struct job_settings *settings, struct page_state *page);
uint32_t end_printer_job(void);
uint32_t start_printer_page(struct job_settings *settings, struct page_state *page,
			    uint32_t page_number);
uint32_t end_printer_page(struct page_state *page);
uint32_t send_printer_band(struct job_settings *settings, struct page_state *page);
void configure_printer_options(struct job_settings *settings, struct page_state *page);
uint32_t should_process_page(struct page_state *page);
void configure_raster_geometry(struct page_state *page);
uint32_t paper_uses_legacy_raster(struct printer_state *printer);
void choose_raster_mode(struct printer_state *printer);
uint32_t use_enhanced_media_mode(struct printer_state *printer);
void choose_compression_mode(struct printer_state *printer);

#endif
