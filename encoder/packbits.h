/* Standalone interface to the PackBits encoder extracted from Gutenprint. */
#ifndef MFCJ5720DW_PACKBITS_H
#define MFCJ5720DW_PACKBITS_H

/* Preserve the upstream signature; the unused context may be NULL. */
typedef struct stp_vars stp_vars_t;

/*
 * Encode length bytes into a separate, caller-owned buffer. Reserve at least
 * 2 * length bytes; comp_ptr receives the end of the encoded output.
 * first and last may both be NULL, or point to the nonzero input byte bounds.
 */
int stp_pack_tiff(stp_vars_t *v, const unsigned char *line, int length,
		  unsigned char *comp_buf, unsigned char **comp_ptr,
		  int *first, int *last);

#endif
