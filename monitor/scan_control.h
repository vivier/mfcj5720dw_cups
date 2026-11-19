/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Laurent Vivier */
#ifndef MONITOR_SCAN_CONTROL_H
#define MONITOR_SCAN_CONTROL_H

#include "scan_job.h"

enum scan_sides {
	SCAN_ONE_SIDED,
	SCAN_LONG_EDGE,
	SCAN_SHORT_EDGE,
};

/* Read the settings of the pending panel request without starting acquisition. */
gboolean scan_control_query(const char *printer, enum scan_destination destination,
			    enum scan_sides *sides, GCancellable *cancel,
			    GString *log, GError **error);
gboolean scan_control_parse(const guint8 *packet, size_t size,
			    enum scan_destination destination,
			    enum scan_sides *sides, GError **error);

#endif
