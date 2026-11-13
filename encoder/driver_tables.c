/*
 * Media and quality choices used by raster/JPEG selection.
 * Numeric data transcribed from the Brother MFC-J5720DW LPR driver.
 * See PROVENANCE.md for origin and retained notices.
 */

#include "data.h"

#include <stdint.h>

const uint16_t lpr_quality_modes[24] = {
	0, 1, 2, 3, 3, 3, 4, 5, 3, 3, 4, 5,
	4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, 5,
};

const int16_t lpr_media_modes[5] = {
	3, 3, 4, 5, 4,
};
