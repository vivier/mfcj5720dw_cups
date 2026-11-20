/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Laurent Vivier */
#include "maintenance.h"

#include <cups/cups.h>
#include <glib/gi18n.h>
#include <string.h>

#define CUPS_TIMEOUT_MS 5000

static const struct {
	const char *command;
	const char *label;
} operations[] = {
	[MAINTENANCE_CLEAN] = { "Clean", N_("Normal cleaning") },
	[MAINTENANCE_CLEAN_STRONG] = { "CleanStrong", N_("Strong cleaning") },
	[MAINTENANCE_CLEAN_STRONGEST] = { "CleanStrongest", N_("Strongest cleaning") },
	[MAINTENANCE_NOZZLE_CHECK] = { "PrintSelfTestPage", N_("Nozzle check") },
	[MAINTENANCE_INK_LEVELS] = { "ReportLevels", N_("Refresh CUPS ink levels") },
};

static const struct {
	const char *argument;
	const char *label;
} ink_groups[] = {
	[MAINTENANCE_ALL] = { "all", N_("All print heads") },
	[MAINTENANCE_BLACK] = { "black", N_("Black") },
	[MAINTENANCE_COLOR] = { "color", N_("Colors") },
};

unsigned int maintenance_commands(ipp_t *attributes)
{
	ipp_attribute_t *commands = ippFindAttribute(attributes, "printer-commands", IPP_TAG_KEYWORD);
	int type = ippGetInteger(ippFindAttribute(attributes, "printer-type", IPP_TAG_ENUM), 0);
	unsigned int supported = 0;
	size_t operation;
	int i;

	if (!(type & CUPS_PRINTER_COMMANDS))
		return 0;
	for (i = 0; i < ippGetCount(commands); i++) {
		const char *command = ippGetString(commands, i, NULL);

		if (!command)
			continue;
		for (operation = 0; operation < G_N_ELEMENTS(operations); operation++)
			if (!g_ascii_strcasecmp(command, operations[operation].command))
				supported |= 1U << operation;
	}
	return supported;
}

const char *maintenance_operation_name(enum maintenance_operation operation)
{
	if ((unsigned int)operation >= G_N_ELEMENTS(operations))
		return "";
	return _(operations[operation].label);
}

const char *maintenance_ink_name(enum maintenance_inks inks)
{
	if ((unsigned int)inks >= G_N_ELEMENTS(ink_groups))
		return "";
	return _(ink_groups[inks].label);
}

char *maintenance_title(enum maintenance_operation operation, enum maintenance_inks inks)
{
	if (operation <= MAINTENANCE_CLEAN_STRONGEST)
		return g_strdup_printf("%s — %s", maintenance_operation_name(operation),
				       maintenance_ink_name(inks));
	return g_strdup(maintenance_operation_name(operation));
}

static const char *no_password(const char *prompt, http_t *http,
			       const char *method, const char *resource, void *data)
{
	(void)prompt; (void)http; (void)method; (void)resource; (void)data;
	return NULL;
}

static gboolean valid_queue(const char *queue)
{
	const unsigned char *p = (const unsigned char *)queue;

	if (!queue || !*queue || strlen(queue) >= 256)
		return FALSE;
	for (; *p; p++)
		if (*p <= ' ' || *p == 127 || *p == '/')
			return FALSE;
	return TRUE;
}

int maintenance_submit(const char *queue, enum maintenance_operation operation,
		       enum maintenance_inks inks, GError **error)
{
	const char *wanted[] = { "printer-type", "printer-commands", "printer-is-accepting-jobs" };
	g_autofree char *document = NULL, *title = NULL;
	http_t *http;
	ipp_t *request, *response;
	http_status_t status;
	char uri[1024];
	const char *resource;
	int job_id = 0;

	if (!valid_queue(queue) || (unsigned int)operation >= MAINTENANCE_OPERATION_COUNT ||
	    (unsigned int)inks >= MAINTENANCE_INK_COUNT ||
	    (operation > MAINTENANCE_CLEAN_STRONGEST && inks != MAINTENANCE_ALL)) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
				    _("Invalid maintenance request or print queue."));
		return 0;
	}
	httpAssembleURIf(HTTP_URI_CODING_ALL, uri, sizeof(uri), "ipp", NULL,
			"localhost", 631, "/printers/%s", queue);
	resource = strstr(uri, "/printers/");
	/* Pin the local scheduler; CUPS owns delivery and serializes print jobs. */
	cupsSetPasswordCB2(no_password, NULL);
	http = httpConnect2("/run/cups/cups.sock", 631, NULL, AF_UNSPEC,
			    HTTP_ENCRYPTION_IF_REQUESTED, 1, CUPS_TIMEOUT_MS, NULL);
	if (!http) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
			    _("Cannot connect to local CUPS: %s"), cupsLastErrorString());
		return 0;
	}
	httpSetTimeout(http, CUPS_TIMEOUT_MS / 1000.0, NULL, NULL);
	request = ippNewRequest(IPP_OP_GET_PRINTER_ATTRIBUTES);
	ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, uri);
	ippAddStrings(request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "requested-attributes",
		      G_N_ELEMENTS(wanted), NULL, wanted);
	response = cupsDoRequest(http, request, resource);
	if (!response || ippGetStatusCode(response) >= IPP_STATUS_ERROR_BAD_REQUEST) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
			    _("Cannot read maintenance capabilities for %s: %s"), queue, cupsLastErrorString());
		ippDelete(response);
		goto close;
	}
	/* Recheck at submission: the queue's driver may have changed since polling. */
	if (!(maintenance_commands(response) & (1U << operation))) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
				    _("This print queue does not support the selected maintenance operation."));
		ippDelete(response);
		goto close;
	}
	if (!ippGetBoolean(ippFindAttribute(response, "printer-is-accepting-jobs", IPP_TAG_BOOLEAN), 0)) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
				    _("This print queue is not accepting jobs."));
		ippDelete(response);
		goto close;
	}
	ippDelete(response);
	document = operation <= MAINTENANCE_CLEAN_STRONGEST ?
		g_strdup_printf("#CUPS-COMMAND\n%s %s\n", operations[operation].command,
				ink_groups[inks].argument) :
		g_strdup_printf("#CUPS-COMMAND\n%s\n", operations[operation].command);
	title = maintenance_title(operation, inks);
	request = ippNewRequest(IPP_OP_PRINT_JOB);
	ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, uri);
	ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, cupsUser());
	ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "job-name", NULL, title);
	ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_MIMETYPE, "document-format", NULL, CUPS_FORMAT_COMMAND);
	ippAddInteger(request, IPP_TAG_JOB, IPP_TAG_INTEGER, "copies", 1);
	/* Submit one command in one job, without changing queue defaults. Never
	 * retry a possibly accepted maintenance request after a lost response.
	 */
	status = cupsSendRequest(http, request, resource, ippLength(request) + strlen(document));
	ippDelete(request);
	if (status == HTTP_STATUS_CONTINUE)
		status = cupsWriteRequestData(http, document, strlen(document));
	response = (status == HTTP_STATUS_CONTINUE || status == HTTP_STATUS_OK) ?
		cupsGetResponse(http, resource) : NULL;
	if (response && ippGetStatusCode(response) < IPP_STATUS_ERROR_BAD_REQUEST)
		job_id = ippGetInteger(ippFindAttribute(response, "job-id", IPP_TAG_INTEGER), 0);
	if (response && ippGetStatusCode(response) >= IPP_STATUS_ERROR_BAD_REQUEST)
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
			    _("CUPS rejected the maintenance request: %s"), cupsLastErrorString());
	else if (job_id <= 0)
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
				    _("CUPS did not confirm the maintenance request. Check the queue before trying again."));
	ippDelete(response);
close:
	httpClose(http);
	return job_id > 0 ? job_id : 0;
}
