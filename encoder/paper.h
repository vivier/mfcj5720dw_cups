/* Paper identifiers, custom margins and automatic page rotation. */
#ifndef BROTHER_ENCODER_PAPER_H
#define BROTHER_ENCODER_PAPER_H

#include <stdbool.h>
#include <stdint.h>

struct job_settings;
struct page_state;

uint32_t is_landscape_custom_paper(uint8_t *paper_name, float width, float height);
uint32_t is_landscape_standard_paper(char *paper_name);
uint32_t needs_landscape_rotation(struct job_settings *settings);
bool needs_envelope_rotation(struct job_settings *settings);
uint32_t is_japanese_envelope(char *paper_name);
int identify_custom_paper(struct page_state *page);
int find_paper_id(char *paper_name, short width, short height, uint16_t *borderless);

#endif
