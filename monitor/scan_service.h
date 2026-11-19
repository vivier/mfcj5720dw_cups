/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Laurent Vivier */
#ifndef MONITOR_SCAN_SERVICE_H
#define MONITOR_SCAN_SERVICE_H

#include "scan_job.h"

enum scan_activity {
	SCAN_WAITING,
	SCAN_RECEIVING,
	SCAN_FINISHED,
	SCAN_FAILED,
};

struct scan_service;
typedef void (*scan_callback)(enum scan_activity activity, const char *message,
			     enum scan_destination destination,
			     const struct scan_result *result, gpointer data);

struct scan_service *scan_service_new(const char *printer,
				     const struct scan_settings *settings,
				     scan_callback callback, gpointer data);
/* Disconnect callbacks; cancel and wait for acquisition/temporary-file cleanup. */
void scan_service_free(struct scan_service *service);
gboolean scan_service_busy(const struct scan_service *service);
void scan_service_cancel(struct scan_service *service);

#endif
