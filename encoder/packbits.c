/*
 *
 *   Softweave calculator for Gutenprint.
 *
 *   Copyright 2000 Charles Briscoe-Smith <cpbs@debian.org>
 *
 *   This program is free software; you can redistribute it and/or modify it
 *   under the terms of the GNU General Public License as published by the Free
 *   Software Foundation; either version 2 of the License, or (at your option)
 *   any later version.
 *
 *   This program is distributed in the hope that it will be useful, but
 *   WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 *   or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 *   for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*
 * Extracted unchanged from Gutenprint src/main/bit-ops.c at commit
 * 11a6edb87d36ed0e51a8ece5ad36815202dd2f12 (GPL-2.0-or-later).
 * Only the includes and NOINLINE definition are local to this extraction.
 * Keep the upstream formatting to make comparison straightforward.
 */
#include "packbits.h"

#include <string.h>

#ifdef __GNUC__
#define NOINLINE __attribute__((noinline))
#else
#define NOINLINE
#endif

static void NOINLINE
find_first_and_last(const unsigned char *line, int length,
		    int *first, int *last)
{
  int found_first = 0;
  int f = 0;
  int l = 0;
  for (f = 0; f < length; f++)
    {
      if (line[f])
	{
	  found_first = 1;
	  break;
	}
    }
  *first = f;
  if (!found_first)
    {
      *last = 0;
      return;
    }
  for (l = length - 1; l >= f; l--)
    if (line[l])
      break;
  ;
  *last = l;
}

int
stp_pack_tiff(stp_vars_t *v,
	      const unsigned char *line,
	      int length,
	      unsigned char *comp_buf,
	      unsigned char **comp_ptr,
	      int *first,
	      int *last)
{
  unsigned char *comp_pti = comp_buf;
  (void)v;
  if (first && last)
    find_first_and_last(line, length, first, last);

  /*
   * Compress using TIFF "packbits" run-length encoding...
   */

  while (length > 0)
    {
      const unsigned char *start = line;	/* Start of compressed data */
      unsigned char repeat;		/* Repeating char */
      int count;			/* Count of compressed bytes */
      /*
       * Get a run of at least 3 non-repeated chars...
       */

      line   += 2;
      length -= 2;

      while (length > 0 && (line[-2] != line[-1] || line[-1] != line[0]))
	{
	  line ++;
	  length --;
	}

      line   -= 2;
      length += 2;

      /*
       * Output the non-repeated sequences (max 128 at a time).
       */

      count = line - start;
      while (count > 0)
	{
	  int tcount = count > 128 ? 128 : count;

	  comp_pti[0] = tcount - 1;
	  memcpy(comp_pti + 1, start, tcount);

	  comp_pti += tcount + 1;
	  start    += tcount;
	  count    -= tcount;
	}

      if (length <= 0)
	break;

      /*
       * Find the repeated sequences...
       */

      start  = line;
      repeat = line[0];

      line ++;
      length --;

      while (length > 0 && *line == repeat)
	{
	  line++;
	  length--;
	}

      /*
       * Output the repeated sequences (max 128 at a time).
       */

      count = line - start;
      while (count > 0)
	{
	  int tcount = count > 128 ? 128 : count;

	  comp_pti[0] = 1 - tcount;
	  comp_pti[1] = repeat;

	  comp_pti += 2;
	  count    -= tcount;
	}
    }
  (*comp_ptr) = comp_pti;

  if (first && last && *first > *last)
    return 0;
  else
    return 1;
}
