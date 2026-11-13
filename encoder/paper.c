/* Resolve paper IDs, custom-paper margins and automatic rotation. */
#include "paper.h"
#include "config.h"
#include "data.h"
#include "protocol.h"
#include "settings.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum paper_constants {
	PAPER_TABLE_ENTRIES = 42,
	CUSTOM_MARGIN_RECORD = 0,
	NO_MARGIN_RECORD = -1,
	PAPER_THICKNESS_ENVELOPE = 4,
};

#define CUSTOM_LANDSCAPE_MIN_WIDTH 360.0f
#define CUSTOM_LANDSCAPE_MAX_WIDTH 612.0f
#define CUSTOM_LANDSCAPE_MIN_HEIGHT 612.0f
#define CUSTOM_LANDSCAPE_MAX_HEIGHT 842.0f

uint32_t is_landscape_custom_paper(uint8_t *paper_name, float width, float height)
{
	if (strcmp((const char *)paper_name, "Custom"))
		return 0;
	/* Dimensions are points; ordered comparisons also reject NaNs. */
	return width >= CUSTOM_LANDSCAPE_MIN_WIDTH && width <= CUSTOM_LANDSCAPE_MAX_WIDTH &&
	       height > CUSTOM_LANDSCAPE_MIN_HEIGHT && height <= CUSTOM_LANDSCAPE_MAX_HEIGHT;
}

uint32_t is_landscape_standard_paper(char *paper_name)
{
	/* Substring matching also recognizes the borderless variants. */
	return strstr(paper_name, "Letter") || strstr(paper_name, "A4") ||
	       strstr(paper_name, "B5") || strstr(paper_name, "Executive");
}

uint32_t needs_landscape_rotation(struct job_settings *settings)
{
	const char *orientation = current_option_text("com.brother.print.PrintSettings.Orientation");

	/* Any explicit option overrides automatic paper-based rotation. */
	if (orientation)
		return !strcmp(orientation, "Landscape");
	if (is_landscape_standard_paper(settings->paper_name))
		return 1;
	return is_landscape_custom_paper((uint8_t *)settings->paper_name,
					settings->paper_width, settings->paper_height);
}

bool needs_envelope_rotation(struct job_settings *settings)
{
	return is_japanese_envelope(settings->paper_name) != 0;
}

uint32_t is_japanese_envelope(char *paper_name)
{
	return strstr(paper_name, "EnvChou3") || strstr(paper_name, "EnvChou4") ||
	       strstr(paper_name, "EnvKaku2");
}

int identify_custom_paper(struct page_state *page)
{
	const char *name = lpr_custom_paper_records[0].name;

	if (!strstr(page->paper_name, name))
		return NO_MARGIN_RECORD;
	if (!strcmp(name, "Custom") && page->paper_thickness != PAPER_THICKNESS_ENVELOPE)
		return NO_MARGIN_RECORD;
	return CUSTOM_MARGIN_RECORD;
}

static const struct paper_id_entry *lookup_paper_name(const char *name)
{
	const struct paper_id_entry *papers = lpr_paper_id_table;
	unsigned int index;

	for (index = 0; index < PAPER_TABLE_ENTRIES; index++) {
		if (!strcmp(name, papers[index].name))
			return &papers[index];
	}
	return NULL;
}

static int paper_id(const struct paper_id_entry *paper, uint16_t *borderless)
{
	if (!paper)
		return PAPER_ID_CUSTOM;
	*borderless = strstr(paper->name, "_B") != NULL;
	return paper->id;
}

int find_paper_id(char *paper_name, short width, short height, uint16_t *borderless)
{
	const struct loaded_paper *loaded = lpr_loaded_papers;
	unsigned int index;

	*borderless = 0;
	/* An unknown explicit name selects custom paper, without automatic lookup. */
	if (paper_name)
		return paper_id(lookup_paper_name(paper_name), borderless);
	/*
	 * Select the first loaded, recognized paper large enough in both dimensions.
	 * Stop at an empty record or the array limit; no extra sentinel is required.
	 */
	for (index = 0; index < PAPER_TABLE_ENTRIES && loaded[index].name[0]; index++) {
		const struct paper_id_entry *paper = lookup_paper_name(loaded[index].name);

		if (paper && width <= loaded[index].width && height <= loaded[index].height)
			return paper_id(paper, borderless);
	}
	return PAPER_ID_CUSTOM;
}
