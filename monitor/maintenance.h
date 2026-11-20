/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Laurent Vivier */
#ifndef MONITOR_MAINTENANCE_H
#define MONITOR_MAINTENANCE_H

#include <cups/ipp.h>
#include <gio/gio.h>

enum maintenance_operation {
	MAINTENANCE_CLEAN,
	MAINTENANCE_CLEAN_STRONG,
	MAINTENANCE_CLEAN_STRONGEST,
	MAINTENANCE_NOZZLE_CHECK,
	MAINTENANCE_INK_LEVELS,
	MAINTENANCE_OPERATION_COUNT,
};

enum maintenance_inks {
	MAINTENANCE_ALL,
	MAINTENANCE_BLACK,
	MAINTENANCE_COLOR,
	MAINTENANCE_INK_COUNT,
};

/* One capability bit per operation, obtained from CUPS printer attributes. */
unsigned int maintenance_commands(ipp_t *attributes);
const char *maintenance_operation_name(enum maintenance_operation operation);
const char *maintenance_ink_name(enum maintenance_inks inks);
char *maintenance_title(enum maintenance_operation operation, enum maintenance_inks inks);
/* Blocking local CUPS operation: call from a worker. Returns a job ID or zero. */
int maintenance_submit(const char *queue, enum maintenance_operation operation,
		       enum maintenance_inks inks, GError **error);

#endif
