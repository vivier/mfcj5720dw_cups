/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MONITOR_SCAN_UI_H
#define MONITOR_SCAN_UI_H

#include "scan_service.h"
#include <gtk/gtk.h>

struct scan_panel;

struct scan_panel *scan_panel_new(GtkBox *parent, GtkApplication *application,
				 const struct scan_settings *settings,
				 const gboolean *notify_enabled);
/* Restore form values without changing the running scan service. */
void scan_panel_reset(struct scan_panel *panel, const struct scan_settings *settings);
gboolean scan_panel_busy(struct scan_panel *panel);
gboolean scan_panel_read(struct scan_panel *panel, struct scan_settings *settings,
			GError **error);
void scan_panel_follow(struct scan_panel *panel, const char *printer,
		       const struct scan_settings *settings);
void scan_panel_free(struct scan_panel *panel);

#endif
