/* Retrieve and cache factory calibration through the CUPS backend. */
#ifndef BROTHER_CALIBRATION_H
#define BROTHER_CALIBRATION_H

#include <stdbool.h>
#include <stddef.h>

#include "encoder/calibration.h"

/* Query returns zero on success and the actual reply length in size. */
typedef int (*calibration_query_fn)(void *context, const char *oid,
				    char *reply, size_t *size);

bool calibration_valid(const struct printer_calibration *calibration);
int calibration_fetch(struct printer_calibration *calibration,
		      calibration_query_fn query, void *context);

bool calibration_prepare(struct printer_calibration *calibration,
			 const char *queue, const char *device_uri, bool refresh);

#endif
