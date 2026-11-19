/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MONITOR_POLL_H
#define MONITOR_POLL_H

#include "status.h"

void monitor_network_init(void);
void monitor_poll(const struct monitor_target *target,
		  struct monitor_snapshot *snapshot);
gboolean monitor_uri_host(const char *uri, char *host, size_t size);

#endif
