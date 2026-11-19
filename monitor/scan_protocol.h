/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Laurent Vivier */
#ifndef MONITOR_SCAN_PROTOCOL_H
#define MONITOR_SCAN_PROTOCOL_H

#include "scan_job.h"

#define SCAN_KEY_PORT 54925
#define SCAN_KEY_LEASE 360
#define SCAN_KEY_RENEWAL 240
#define SCAN_KEY_PACKET_MAX 2048

struct scan_key_event {
	enum scan_destination destination;
	guint32 registration;
	guint32 sequence;
};

char *scan_key_registration(const char *name, const char *address,
			   enum scan_destination destination);
/* Match the complete registration identity; never use packet data as a path. */
gboolean scan_key_parse(const guint8 *packet, size_t size, const char *name,
			const char *address, struct scan_key_event *event);

#endif
