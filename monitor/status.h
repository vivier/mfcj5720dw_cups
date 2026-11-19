/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Laurent Vivier */
#ifndef MONITOR_STATUS_H
#define MONITOR_STATUS_H

#include <glib.h>
#include <stdint.h>

#define MONITOR_TEXT 512
#define MONITOR_MAX_QUEUES 32
#define MONITOR_MAX_JOBS 20
#define MONITOR_MAX_ALERTS 16

enum monitor_severity {
	MONITOR_UNKNOWN,
	MONITOR_OK,
	MONITOR_WARNING,
	MONITOR_ERROR,
};

struct monitor_queue {
	char name[256];
	char uri[1024];
};

struct monitor_job {
	int id;
	int state;
	char title[MONITOR_TEXT];
};

struct monitor_alert {
	unsigned long index;
	int severity;
	char text[MONITOR_TEXT];
};

struct monitor_target {
	char queue[256];
	char host[256];
	char community[256];
};

struct monitor_snapshot {
	struct monitor_target target;
	gint64 timestamp;
	gboolean online;
	gboolean ink_valid;
	gboolean errors_valid;
	gboolean alerts_valid;
	gboolean cups_valid;
	gboolean jobs_valid;
	unsigned int ink[4];
	uint32_t errors;
	int device_status;
	int printer_status;
	int brother_status;
	int queue_state;
	char display[MONITOR_TEXT];
	char network_error[MONITOR_TEXT];
	char queue_message[MONITOR_TEXT];
	char queue_reasons[MONITOR_TEXT];
	struct monitor_queue queues[MONITOR_MAX_QUEUES];
	unsigned int queue_count;
	struct monitor_job jobs[MONITOR_MAX_JOBS];
	unsigned int job_count;
	struct monitor_alert alerts[MONITOR_MAX_ALERTS];
	unsigned int alert_count;
	enum monitor_severity severity;
	char summary[MONITOR_TEXT];
	char details[4096];
};

/* RFC 2790 numbers bits from the most significant bit of each byte. */
uint32_t monitor_error_bits(const unsigned char *data, size_t size);
void monitor_classify(struct monitor_snapshot *snapshot);
gboolean monitor_issue_changed(const struct monitor_snapshot *previous,
			      const struct monitor_snapshot *current);
const char *monitor_job_state(int state);
/* Explicit terminal states only; disappearance from CUPS is not completion. */
gboolean monitor_job_finished(const struct monitor_job *previous,
			     const struct monitor_job *current);

#endif
