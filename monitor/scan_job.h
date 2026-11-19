/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Laurent Vivier */
#ifndef MONITOR_SCAN_JOB_H
#define MONITOR_SCAN_JOB_H

#include <gio/gio.h>

enum scan_destination {
	SCAN_IMAGE,
	SCAN_EMAIL,
	SCAN_OCR,
	SCAN_FILE,
	SCAN_DESTINATIONS,
};

struct scan_settings {
	gboolean enabled;
	char name[16];
	char directory[4096];
	char address[16]; /* Optional reachable IPv4 address, for routed networks. */
	char languages[64];
	unsigned int resolution;
	gboolean grayscale;
};

struct scan_job {
	struct scan_settings settings;
	enum scan_destination destination;
	char printer[16];
};

struct scan_result {
	char *directory;
	char *document;
	unsigned int pages;
	GError *error;
};

/* Shared by settings validation and the resolution selector. */
extern const unsigned int scan_resolutions[];
extern const size_t scan_resolution_count;

void scan_settings_defaults(struct scan_settings *settings);
gboolean scan_settings_valid(const struct scan_settings *settings, GError **error);
void scan_settings_load(struct scan_settings *settings, GKeyFile *file);
void scan_settings_save(const struct scan_settings *settings, GKeyFile *file);
const char *scan_destination_name(enum scan_destination destination);
/* Run in a worker thread. All output stays in a new private job directory. */
struct scan_result *scan_job_run(const struct scan_job *job, GCancellable *cancel);
void scan_result_free(struct scan_result *result);
gboolean scan_write_pdf(const char *directory, unsigned int pages,
			unsigned int resolution, GCancellable *cancel, GError **error);
char *scan_device_for_printer(const char *config, const char *devices,
			      const char *printer, GError **error);

#endif
