/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Laurent Vivier */
#include "status.h"

#include <cups/ipp.h>
#include <glib/gi18n.h>
#include <string.h>

struct brother_state {
	int code;
	enum monitor_severity severity;
	const char *message;
};

/* MFC-J5720DW uses the INK15 status family. See README.md for evidence. */
static const struct brother_state brother_states[] = {
	{ 10001, MONITOR_OK, N_("Ready") },
	{ 10006, MONITOR_WARNING, N_("Ink is running low") },
	{ 12201, MONITOR_ERROR, N_("Tray 1 is missing") },
	{ 12301, MONITOR_ERROR, N_("Tray 2 is missing") },
	{ 30016, MONITOR_ERROR, N_("Printer memory is full") },
	{ 40000, MONITOR_OK, N_("Sleeping") },
	{ 40010, MONITOR_ERROR, N_("Ink cartridge is missing") },
	{ 40021, MONITOR_ERROR, N_("Cover is open") },
	{ 40022, MONITOR_ERROR, N_("Paper jam") },
	{ 40038, MONITOR_ERROR, N_("Replace ink cartridge") },
	{ 41000, MONITOR_ERROR, N_("Paper could not be fed") },
	{ 41200, MONITOR_ERROR, N_("Paper could not be fed from tray 1") },
	{ 41300, MONITOR_ERROR, N_("Paper could not be fed from tray 2") },
	{ 60000, MONITOR_ERROR, N_("Printer memory is full") },
	{ 60030, MONITOR_ERROR, N_("Printer requires service") },
	{ 60164, MONITOR_ERROR, N_("Printer cannot access its log") },
	{ 70004, MONITOR_ERROR, N_("Document feeder jam") },
	{ 70005, MONITOR_ERROR, N_("Loaded paper does not match the job") },
	{ 70014, MONITOR_ERROR, N_("Cover is open") },
	{ 70050, MONITOR_WARNING, N_("Ink absorber is almost full") },
	{ 70051, MONITOR_ERROR, N_("Printer temperature is too high") },
	{ 70052, MONITOR_ERROR, N_("Printer temperature is too low") },
	{ 70053, MONITOR_ERROR, N_("Ink cartridge is not recognized") },
	{ 70054, MONITOR_ERROR, N_("Disc guide is open") },
	{ 70055, MONITOR_WARNING, N_("Ink quantity cannot be measured") },
	{ 70056, MONITOR_ERROR, N_("Firmware version error") },
	{ 70058, MONITOR_ERROR, N_("Ink cartridge is in the wrong slot") },
	{ 70059, MONITOR_ERROR, N_("Starter ink cartridges are required") },
	{ 70500, MONITOR_ERROR, N_("Ink absorber is full") },
	{ 70501, MONITOR_ERROR, N_("Printing fault (3X)") },
	{ 70502, MONITOR_ERROR, N_("Printing fault (5X)") },
	{ 70503, MONITOR_ERROR, N_("Printing fault (7X)") },
	{ 70504, MONITOR_ERROR, N_("Scanning fault (AX)") },
	{ 70900, MONITOR_ERROR, N_("Printer is locked") },
};

static const char *const error_names[] = {
	N_("Paper is running low"), N_("Paper is empty"),
	N_("Ink is running low"), N_("Ink is empty"), N_("Cover is open"),
	N_("Paper jam"), N_("Printer is offline"), N_("Service is required"),
	N_("Input tray is missing"), N_("Output tray is missing"),
	N_("Ink cartridge is missing"), N_("Output tray is almost full"),
	N_("Output tray is full"), N_("Input tray is empty"),
	N_("Preventive maintenance is overdue"),
};

uint32_t monitor_error_bits(const unsigned char *data, size_t size)
{
	uint32_t bits = 0;
	size_t bit;

	for (bit = 0; bit < MIN(size, 4) * 8; bit++) {
		if (data[bit / 8] & (0x80 >> (bit % 8)))
			bits |= UINT32_C(1) << bit;
	}
	return bits;
}

static void add_detail(struct monitor_snapshot *s, const char *text)
{
	if (!text[0])
		return;
	if (!strcmp(s->details, text))
		return;
	if (s->details[0])
		g_strlcat(s->details, "\n", sizeof(s->details));
	g_strlcat(s->details, text, sizeof(s->details));
}

static void set_state(struct monitor_snapshot *s, enum monitor_severity severity,
		      const char *text)
{
	if (severity > s->severity) {
		s->severity = severity;
		g_strlcpy(s->summary, text, sizeof(s->summary));
	}
}

void monitor_classify(struct monitor_snapshot *s)
{
	const struct brother_state *state = NULL;
	unsigned int i;
	char text[MONITOR_TEXT];

	s->severity = MONITOR_UNKNOWN;
	s->details[0] = '\0';
	g_strlcpy(s->summary, _("Status unavailable"), sizeof(s->summary));
	if (!s->online) {
		add_detail(s, s->network_error);
		return;
	}
	for (i = 0; i < G_N_ELEMENTS(brother_states); i++) {
		if (brother_states[i].code == s->brother_status) {
			state = &brother_states[i];
			break;
		}
	}
	if (state) {
		set_state(s, state->severity, _(state->message));
		if (state->severity == MONITOR_OK && s->printer_status == 4)
			g_strlcpy(s->summary, _("Printing"), sizeof(s->summary));
	} else if (s->brother_status >= 0) {
		/* Never label an unrecognized vendor error as a healthy printer. */
		g_snprintf(s->summary, sizeof(s->summary),
			   _("Unrecognized printer status (%d)"), s->brother_status);
	} else if (s->printer_status >= 3 && s->printer_status <= 5) {
		set_state(s, MONITOR_OK, s->printer_status == 3 ? _("Ready") :
			  s->printer_status == 4 ? _("Printing") : _("Warming up"));
	}
	add_detail(s, s->display);
	for (i = 0; i < G_N_ELEMENTS(error_names); i++) {
		if (!(s->errors & (UINT32_C(1) << i)))
			continue;
		set_state(s, (i == 0 || i == 2 || i == 11 || i == 14) ?
			  MONITOR_WARNING : MONITOR_ERROR, _(error_names[i]));
		add_detail(s, _(error_names[i]));
	}
	if (s->errors & ~UINT32_C(0x7fff)) {
		set_state(s, MONITOR_WARNING, _("Unrecognized printer error"));
		g_snprintf(text, sizeof(text), _("Error flags: 0x%08x"), s->errors);
		add_detail(s, text);
	}
	if (s->device_status == 5)
		set_state(s, MONITOR_ERROR, _("Printer is unavailable"));
	else if (s->device_status == 3)
		set_state(s, MONITOR_WARNING, _("Printer needs attention"));
	for (i = 0; i < s->alert_count; i++) {
		add_detail(s, s->alerts[i].text);
		/* This model also publishes its normal display with severity -1. */
		if (s->alerts[i].severity == 3 || s->alerts[i].severity == 4)
			set_state(s, s->alerts[i].severity == 3 ? MONITOR_ERROR :
				  MONITOR_WARNING, s->alerts[i].text[0] ?
				  s->alerts[i].text : _("Printer needs attention"));
	}
	if (s->ink_valid) {
		for (i = 0; i < 4; i++) {
			if (!s->ink[i])
				set_state(s, MONITOR_ERROR, _("Ink is empty"));
			else if (s->ink[i] <= 500)
				set_state(s, MONITOR_WARNING, _("Ink is running low"));
		}
	}
	if (!s->errors_valid || !s->alerts_valid)
		add_detail(s, _("Some diagnostic information is unavailable."));
}

gboolean monitor_issue_changed(const struct monitor_snapshot *previous,
			      const struct monitor_snapshot *current)
{
	unsigned int i;

	if (previous->ink_valid && current->ink_valid) {
		for (i = 0; i < 4; i++) {
			if ((previous->ink[i] <= 500) != (current->ink[i] <= 500) ||
			    (!previous->ink[i]) != (!current->ink[i]))
				return TRUE;
		}
	}
	return previous->online != current->online ||
	       previous->severity != current->severity ||
	       previous->errors != current->errors ||
	       strcmp(previous->summary, current->summary) != 0 ||
	       (current->severity >= MONITOR_WARNING &&
		strcmp(previous->details, current->details) != 0);
}

const char *monitor_job_state(int state)
{
	switch (state) {
	case IPP_JSTATE_PENDING: return _("Waiting");
	case IPP_JSTATE_HELD: return _("Held");
	case IPP_JSTATE_PROCESSING: return _("Processing");
	case IPP_JSTATE_STOPPED: return _("Stopped");
	case IPP_JSTATE_CANCELED: return _("Canceled");
	case IPP_JSTATE_ABORTED: return _("Failed");
	case IPP_JSTATE_COMPLETED: return _("Sent to printer");
	default: return _("Unknown");
	}
}

gboolean monitor_job_finished(const struct monitor_job *previous,
			     const struct monitor_job *current)
{
	return previous->id > 0 && previous->id == current->id &&
	       previous->state >= IPP_JSTATE_PENDING &&
	       previous->state <= IPP_JSTATE_STOPPED &&
	       current->state >= IPP_JSTATE_CANCELED &&
	       current->state <= IPP_JSTATE_COMPLETED;
}
