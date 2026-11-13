/* Paper geometry transcribed from Brother paperinfij2, PaperDimension and
 * ImagingArea. Kept as typed data; no /opt files are read by this filter.
 * check_sources.py verifies every record against the reference files.
 */
#include "driver.h"

#include <string.h>

static const struct paper_geometry papers[] = {
	{ "A3", 842, 1191, 6868, 9775, 9, 9, 833, 1182 },
	{ "BrA3_B_S", 842, 1191, 7013, 9925, 0, 0, 842, 1191 },
	{ "B4_S", 729, 1032, 5924, 8450, 9, 9, 720, 1023 },
	{ "Tabloid_S", 792, 1224, 6448, 10050, 9, 9, 783, 1215 },
	{ "BrLedger_B_B", 792, 1224, 6600, 10200, 0, 0, 792, 1224 },
	{ "A4", 595, 842, 4808, 6868, 9, 9, 586, 833 },
	{ "BrA4_B", 595, 842, 4960, 7016, 0, 0, 595, 842 },
	{ "Letter", 612, 792, 4948, 6450, 9, 9, 603, 783 },
	{ "BrLetter_B", 612, 792, 5100, 6600, 0, 0, 612, 792 },
	{ "Legal", 612, 1008, 4948, 8250, 9, 9, 603, 999 },
	{ "Folio", 612, 936, 4948, 7650, 9, 9, 603, 927 },
	{ "Executive", 522, 756, 4200, 6150, 9, 9, 513, 747 },
	{ "B5", 516, 729, 4152, 5925, 9, 9, 507, 720 },
	{ "A5", 420, 595, 3352, 4808, 9, 9, 411, 586 },
	{ "A6", 297, 420, 2324, 3350, 9, 9, 288, 411 },
	{ "BrA6_B", 297, 420, 2478, 3500, 0, 0, 297, 420 },
	{ "BrPostC4x6_S", 288, 432, 2252, 3450, 9, 9, 279, 423 },
	{ "BrPostC4x6_B", 288, 432, 2400, 3600, 0, 0, 288, 432 },
	{ "BrIndexC5x8_S", 360, 576, 2852, 4650, 9, 9, 351, 567 },
	{ "BrIndexC5x8_B", 360, 576, 3000, 4800, 0, 0, 360, 576 },
	{ "BrPhotoL_S", 252, 360, 1952, 2850, 9, 9, 243, 351 },
	{ "BrPhotoL_B", 252, 360, 2100, 3000, 0, 0, 252, 360 },
	{ "BrPhoto2L_S", 359, 505, 2840, 4060, 9, 9, 350, 496 },
	{ "BrPhoto2L_B", 359, 505, 2992, 4208, 0, 0, 359, 505 },
	{ "Postcard", 284, 419, 2216, 3340, 9, 9, 275, 410 },
	{ "BrHagaki_B", 284, 419, 2368, 3492, 0, 0, 284, 419 },
	{ "DoublePostcardRotated", 420, 567, 3352, 4576, 9, 9, 411, 558 },
	{ "EnvC5", 459, 649, 3676, 4608, 9, 62, 450, 615 },
	{ "EnvDL", 312, 624, 2452, 4400, 9, 62, 303, 590 },
	{ "Env10", 297, 684, 2324, 4900, 9, 62, 288, 650 },
	{ "EnvMonarch", 279, 540, 2176, 3700, 9, 62, 270, 506 },
	{ "EnvC6", 323, 459, 2542, 3025, 9, 62, 314, 425 },
	{ "EnvYou4", 298, 666, 2332, 4750, 9, 62, 289, 632 },
};

const struct paper_geometry *find_paper(const char *name)
{
	size_t index;

	for (index = 0; index < sizeof(papers) / sizeof(papers[0]); index++) {
		if (!strcmp(papers[index].name, name))
			return &papers[index];
	}
	return NULL;
}
