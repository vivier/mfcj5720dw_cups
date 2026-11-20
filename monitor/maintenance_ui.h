/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Laurent Vivier */
#ifndef MONITOR_MAINTENANCE_UI_H
#define MONITOR_MAINTENANCE_UI_H

#include <gtk/gtk.h>

struct maintenance_panel;
struct monitor_snapshot;

struct maintenance_panel *maintenance_panel_new(GtkWindow *parent, GtkButton *open,
						GtkLabel *levels_status);
void maintenance_panel_follow(struct maintenance_panel *panel, const char *queue,
			      unsigned int commands, gboolean accepting_jobs);
int maintenance_panel_ink_job(struct maintenance_panel *panel);
void maintenance_panel_update_ink(struct maintenance_panel *panel,
				  const struct monitor_snapshot *snapshot);
void maintenance_panel_free(struct maintenance_panel *panel);

#endif
