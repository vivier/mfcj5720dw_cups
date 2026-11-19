/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Laurent Vivier */
#ifndef MONITOR_TRAY_H
#define MONITOR_TRAY_H

#include <gtk/gtk.h>

struct monitor_tray;

/* The application provides the "show" and "quit" actions. */
struct monitor_tray *monitor_tray_new(GtkApplication *application,
				      GtkWindow *window, GError **error);
void monitor_tray_free(struct monitor_tray *tray);

#endif
