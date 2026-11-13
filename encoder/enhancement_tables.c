/*
 * Image-enhancement coefficient matrices.
 * Numeric data transcribed from the Brother MFC-J5720DW LPR driver.
 * See PROVENANCE.md for origin and retained notices.
 */

#include "data.h"

#include <stdint.h>

const int32_t lpr_enhancement_kernel_soft[5][5] = {
	{1, 1, 1, 1, 1},
	{1, 1, 1, 1, 1},
	{1, 1, 1, 1, 1},
	{1, 1, 1, 1, 1},
	{1, 1, 1, 1, 1},
};

const int32_t lpr_enhancement_kernel_medium[5][5] = {
	{-1, -1, -1, -1, -1},
	{-1, 0, 0, 0, -1},
	{-1, 0, 48, 0, -1},
	{-1, 0, 0, 0, -1},
	{-1, -1, -1, -1, -1},
};

const int32_t lpr_enhancement_kernel_sharp[5][5] = {
	{1, 1, 1, 1, 1},
	{1, 2, 2, 2, 1},
	{1, 2, 4, 2, 1},
	{1, 2, 2, 2, 1},
	{1, 1, 1, 1, 1},
};
