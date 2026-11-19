/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Laurent Vivier */
#include "poll.h"
#include "../ink.h"

#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-includes.h>
#include <cups/cups.h>
#include <glib/gi18n.h>
#include <string.h>

#define SNMP_TIMEOUT_US 1500000L
#define CUPS_TIMEOUT_MS 1500
#define ALERT_ROOT ".1.3.6.1.2.1.43.18.1.1"
#define LOCALIZATION_CHARSET ".1.3.6.1.2.1.43.7.1.1.4.1"

enum status_field {
	FIELD_PRINTER,
	FIELD_DEVICE,
	FIELD_ERRORS,
	FIELD_DISPLAY,
	FIELD_BROTHER,
	FIELD_INK,
	FIELD_CONSOLE_LOCALIZATION,
	FIELD_GENERAL_LOCALIZATION,
	FIELD_COUNT,
};

static const char *const status_oids[FIELD_COUNT] = {
	[FIELD_PRINTER] = ".1.3.6.1.2.1.25.3.5.1.1.1",
	[FIELD_DEVICE] = ".1.3.6.1.2.1.25.3.2.1.5.1",
	[FIELD_ERRORS] = ".1.3.6.1.2.1.25.3.5.1.2.1",
	[FIELD_DISPLAY] = ".1.3.6.1.2.1.43.16.5.1.2.1.1",
	[FIELD_BROTHER] = ".1.3.6.1.4.1.2435.2.3.9.4.2.1.5.5.6.0",
	[FIELD_INK] = ".1.3.6.1.4.1.2435.2.3.9.4.2.1.5.5.8.0",
	[FIELD_CONSOLE_LOCALIZATION] = ".1.3.6.1.2.1.43.5.1.1.10.1",
	[FIELD_GENERAL_LOCALIZATION] = ".1.3.6.1.2.1.43.5.1.1.2.1",
};

/* Device strings are untrusted data, never markup, paths or format strings. */
static void copy_text(char *out, size_t size, const unsigned char *data,
		      size_t length)
{
	g_autofree char *valid = g_utf8_make_valid((const char *)data,
					       MIN(length, size - 1));
	char *p;

	for (p = valid; *p; p++) {
		if ((unsigned char)*p < ' ' || *p == 127)
			*p = ' ';
	}
	/* Truncate at a character boundary, including replacement characters. */
	while (strlen(valid) >= size)
		*g_utf8_find_prev_char(valid, valid + strlen(valid)) = '\0';
	g_strlcpy(out, g_strstrip(valid), size);
}

static void copy_device_text(char *out, size_t size, const unsigned char *data,
			     size_t length, int charset)
{
	const char *encoding = NULL;
	g_autofree char *converted = NULL;
	size_t converted_size;

	/* IANA MIBenum identifiers, not code-page numbers. */
	switch (charset) {
	case 3: encoding = "US-ASCII"; break;
	case 4: encoding = "ISO-8859-1"; break;
	case 106: encoding = "UTF-8"; break;
	case 2004: encoding = "HP-ROMAN8"; break;
	}
	if (encoding)
		converted = g_convert_with_fallback((const char *)data,
			MIN(length, MONITOR_TEXT), "UTF-8", encoding, "�",
			NULL, &converted_size, NULL);
	if (converted)
		copy_text(out, size, (const unsigned char *)converted, converted_size);
	else
		copy_text(out, size, data, length);
}

void monitor_network_init(void)
{
	/* Numeric OIDs do not require MIB files or an administrator's snmp.conf. */
	netsnmp_ds_set_boolean(NETSNMP_DS_LIBRARY_ID,
			      NETSNMP_DS_LIB_DONT_READ_CONFIGS, 1);
	init_snmp("mfcj5720dw-monitor");
}

static void add_oid(netsnmp_pdu *pdu, const char *name)
{
	oid number[MAX_OID_LEN];
	size_t length = MAX_OID_LEN;

	if (read_objid(name, number, &length))
		snmp_add_null_var(pdu, number, length);
}

static gboolean oid_matches(const netsnmp_variable_list *var, const char *name)
{
	oid number[MAX_OID_LEN];
	size_t length = MAX_OID_LEN;

	return read_objid(name, number, &length) &&
	       var->name_length == length &&
	       snmp_oid_compare(var->name, var->name_length, number, length) == 0;
}

static void read_charsets(void *session, const netsnmp_pdu *status,
			  int *console_charset, int *general_charset)
{
	const netsnmp_variable_list *var;
	const char *fields[] = { status_oids[FIELD_CONSOLE_LOCALIZATION],
				status_oids[FIELD_GENERAL_LOCALIZATION] };
	char names[2][MAX_OID_LEN] = { "", "" };
	int *charsets[] = { console_charset, general_charset };
	netsnmp_pdu *request = snmp_pdu_create(SNMP_MSG_GET), *response = NULL;
	unsigned int i;

	if (!request)
		return;
	for (var = status->variables; var; var = var->next_variable) {
		if (var->type != ASN_INTEGER || *var->val.integer < 1 ||
		    *var->val.integer > 65535)
			continue;
		for (i = 0; i < G_N_ELEMENTS(fields); i++) {
			if (oid_matches(var, fields[i])) {
				g_snprintf(names[i], sizeof(names[i]), "%s.%ld",
					   LOCALIZATION_CHARSET, *var->val.integer);
				add_oid(request, names[i]);
			}
		}
	}
	if (!request->variables) {
		snmp_free_pdu(request);
		return;
	}
	if (snmp_sess_synch_response(session, request, &response) != STAT_SUCCESS ||
	    !response || response->errstat != SNMP_ERR_NOERROR)
		goto out;
	for (var = response->variables; var; var = var->next_variable) {
		if (var->type != ASN_INTEGER)
			continue;
		for (i = 0; i < G_N_ELEMENTS(names); i++) {
			if (names[i][0] && oid_matches(var, names[i]))
				*charsets[i] = *var->val.integer;
		}
	}
out:
	if (response)
		snmp_free_pdu(response);
}

static void read_alerts(void *session, struct monitor_snapshot *s, int charset)
{
	netsnmp_pdu *request = snmp_pdu_create(SNMP_MSG_GETBULK);
	netsnmp_pdu *response = NULL;
	netsnmp_variable_list *var;
	oid root[MAX_OID_LEN];
	size_t root_length = MAX_OID_LEN;
	unsigned int i;

	if (!request)
		return;
	add_oid(request, ALERT_ROOT ".2.1");
	add_oid(request, ALERT_ROOT ".8.1");
	request->non_repeaters = 0;
	request->max_repetitions = MONITOR_MAX_ALERTS;
	if (snmp_sess_synch_response(session, request, &response) != STAT_SUCCESS ||
	    !response || response->errstat != SNMP_ERR_NOERROR)
		goto out;
	s->alerts_valid = TRUE;
	read_objid(ALERT_ROOT, root, &root_length);
	for (var = response->variables; var; var = var->next_variable) {
		unsigned long column, index;

		if (var->name_length != root_length + 3 ||
		    memcmp(var->name, root, root_length * sizeof(oid)) ||
		    var->name[root_length + 1] != 1)
			continue;
		column = var->name[root_length];
		index = var->name[root_length + 2];
		if ((column != 2 || var->type != ASN_INTEGER) &&
		    (column != 8 || var->type != ASN_OCTET_STR))
			continue;
		for (i = 0; i < s->alert_count; i++) {
			if (s->alerts[i].index == index)
				break;
		}
		if (i == MONITOR_MAX_ALERTS)
			continue;
		if (i == s->alert_count) {
			s->alert_count++;
			s->alerts[i].index = index;
			s->alerts[i].severity = -1;
		}
		if (column == 2)
			s->alerts[i].severity = *var->val.integer;
		else
			copy_device_text(s->alerts[i].text, sizeof(s->alerts[i].text),
					 var->val.string, var->val_len, charset);
	}
out:
	if (response)
		snmp_free_pdu(response);
}

static void read_printer(struct monitor_snapshot *s)
{
	netsnmp_session config;
	netsnmp_pdu *request, *response = NULL;
	netsnmp_variable_list *var;
	void *session;
	unsigned int i;
	int console_charset = 0, general_charset = 0;
	g_autofree char *peer = NULL;

	if (!s->target.host[0]) {
		g_strlcpy(s->network_error,
			  _("Select a network printer or enter its IP address."),
			  sizeof(s->network_error));
		return;
	}
	/* Force UDP port 161: an entry cannot select another Net-SNMP transport. */
	peer = strchr(s->target.host, ':') ?
		g_strdup_printf("udp6:[%s]:161", s->target.host) :
		g_strdup_printf("udp:%s:161", s->target.host);
	snmp_sess_init(&config);
	config.peername = peer;
	config.version = SNMP_VERSION_2c;
	config.community = (unsigned char *)s->target.community;
	config.community_len = strlen(s->target.community);
	config.timeout = SNMP_TIMEOUT_US;
	config.retries = 0;
	session = snmp_sess_open(&config);
	if (!session)
		goto unavailable;
	request = snmp_pdu_create(SNMP_MSG_GET);
	if (!request)
		goto close;
	for (i = 0; i < FIELD_COUNT; i++)
		add_oid(request, status_oids[i]);
	if (snmp_sess_synch_response(session, request, &response) != STAT_SUCCESS ||
	    !response || response->errstat != SNMP_ERR_NOERROR)
		goto close;
	s->online = TRUE;
	read_charsets(session, response, &console_charset, &general_charset);
	for (var = response->variables; var; var = var->next_variable) {
		for (i = 0; i < FIELD_COUNT; i++) {
			if (oid_matches(var, status_oids[i]))
				break;
		}
		if (var->type == ASN_INTEGER) {
			if (i == FIELD_PRINTER)
				s->printer_status = *var->val.integer;
			else if (i == FIELD_DEVICE)
				s->device_status = *var->val.integer;
			else if (i == FIELD_BROTHER)
				s->brother_status = *var->val.integer;
		} else if (var->type == ASN_OCTET_STR) {
			if (i == FIELD_ERRORS && var->val_len > 0) {
				s->errors_valid = TRUE;
				s->errors = monitor_error_bits(var->val.string, var->val_len);
			} else if (i == FIELD_DISPLAY) {
				copy_device_text(s->display, sizeof(s->display),
						 var->val.string, var->val_len, console_charset);
			} else if (i == FIELD_INK) {
				s->ink_valid = !brother_decode_ink(var->val.string,
								 var->val_len, s->ink);
			}
		}
	}
	read_alerts(session, s, general_charset);
close:
	if (response)
		snmp_free_pdu(response);
	snmp_sess_close(session);
unavailable:
	if (!s->online)
		g_strlcpy(s->network_error,
			  _("No SNMP reply. Check the address, network and SNMP settings."),
			  sizeof(s->network_error));
}

gboolean monitor_uri_host(const char *uri, char *host, size_t size)
{
	char scheme[32], username[256], resource[1024];
	int port;

	host[0] = '\0';
	if (httpSeparateURI(HTTP_URI_CODING_ALL, uri, scheme, sizeof(scheme),
			    username, sizeof(username), host, size, &port,
			    resource, sizeof(resource)) < HTTP_URI_STATUS_OK)
		goto unsupported;
	if (strcmp(scheme, "socket") && strcmp(scheme, "lpd") &&
	    strcmp(scheme, "ipp") && strcmp(scheme, "ipps"))
		goto unsupported;
	return host[0] != '\0';
unsupported:
	host[0] = '\0';
	return FALSE;
}

static const char *no_password(const char *prompt, http_t *http,
			       const char *method, const char *resource, void *data)
{
	(void)prompt; (void)http; (void)method; (void)resource; (void)data;
	return NULL;
}

static const char *attribute_string(ipp_t *response, const char *name)
{
	const char *text = ippGetString(ippFindAttribute(response, name,
						     IPP_TAG_ZERO), 0, NULL);
	return text ? text : "";
}

static void read_queues(http_t *http, struct monitor_snapshot *s)
{
	ipp_t *request = ippNewRequest(CUPS_GET_PRINTERS);
	ipp_t *response;
	ipp_attribute_t *attr;
	struct monitor_queue queue = { 0 };
	char model[256] = "";
	const char *attributes[] = { "printer-name", "device-uri", "printer-make-and-model" };

	ippAddStrings(request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD,
		      "requested-attributes", G_N_ELEMENTS(attributes), NULL, attributes);
	response = cupsDoRequest(http, request, "/");
	for (attr = ippFirstAttribute(response); ; attr = ippNextAttribute(response)) {
		const char *name = ippGetName(attr);
		const char *value;

		if (!name) {
			if (queue.name[0] && (strstr(model, "MFC-J5720") ||
			    !strcmp(queue.name, s->target.queue)) &&
			    s->queue_count < MONITOR_MAX_QUEUES)
				s->queues[s->queue_count++] = queue;
			memset(&queue, 0, sizeof(queue));
			model[0] = '\0';
			if (!attr)
				break;
			continue;
		}
		value = ippGetString(attr, 0, NULL);
		if (!value)
			continue;
		if (!strcmp(name, "printer-name"))
			g_strlcpy(queue.name, value, sizeof(queue.name));
		else if (!strcmp(name, "device-uri"))
			g_strlcpy(queue.uri, value, sizeof(queue.uri));
		else if (!strcmp(name, "printer-make-and-model"))
			g_strlcpy(model, value, sizeof(model));
	}
	ippDelete(response);
}

static void read_jobs(http_t *http, const char *uri, struct monitor_snapshot *s)
{
	ipp_t *request = ippNewRequest(IPP_OP_GET_JOBS);
	ipp_t *response;
	ipp_attribute_t *attr;
	struct monitor_job job = { 0 };
	const char *attributes[] = { "job-id", "job-name", "job-state" };

	ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, uri);
	ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_NAME, "requesting-user-name", NULL, cupsUser());
	ippAddBoolean(request, IPP_TAG_OPERATION, "my-jobs", 1);
	ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "which-jobs", NULL, "all");
	ippAddInteger(request, IPP_TAG_OPERATION, IPP_TAG_INTEGER, "limit", MONITOR_MAX_JOBS);
	ippAddStrings(request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "requested-attributes",
		      G_N_ELEMENTS(attributes), NULL, attributes);
	response = cupsDoRequest(http, request, "/");
	s->jobs_valid = response && ippGetStatusCode(response) <= IPP_STATUS_OK_EVENTS_COMPLETE;
	if (!s->jobs_valid)
		goto out;
	for (attr = ippFirstAttribute(response); ; attr = ippNextAttribute(response)) {
		const char *name = ippGetName(attr);
		const char *value;

		if (!name) {
			if (job.id > 0 && s->job_count < MONITOR_MAX_JOBS)
				s->jobs[s->job_count++] = job;
			memset(&job, 0, sizeof(job));
			if (!attr)
				break;
		} else if (!strcmp(name, "job-id")) {
			job.id = ippGetInteger(attr, 0);
		} else if (!strcmp(name, "job-state")) {
			job.state = ippGetInteger(attr, 0);
		} else if (!strcmp(name, "job-name") && (value = ippGetString(attr, 0, NULL))) {
			copy_text(job.title, sizeof(job.title), (const unsigned char *)value, strlen(value));
		}
	}
out:
	ippDelete(response);
}

static void read_cups(struct monitor_snapshot *s)
{
	http_t *http;
	ipp_t *request, *response;
	ipp_attribute_t *attr;
	char uri[1024];
	unsigned int i;
	const char *attributes[] = { "printer-state", "printer-state-message",
		"printer-state-reasons", "device-uri" };

	/* This application follows local queues and never prompts from a worker. */
	cupsSetPasswordCB2(no_password, NULL);
	http = httpConnect2("/run/cups/cups.sock", 631, NULL, AF_UNSPEC,
			    HTTP_ENCRYPTION_IF_REQUESTED, 1, CUPS_TIMEOUT_MS, NULL);
	if (!http)
		return;
	httpSetTimeout(http, CUPS_TIMEOUT_MS / 1000.0, NULL, NULL);
	read_queues(http, s);
	if (!s->target.queue[0] && !s->target.host[0] && s->queue_count)
		g_strlcpy(s->target.queue, s->queues[0].name, sizeof(s->target.queue));
	if (!s->target.queue[0])
		goto close;
	httpAssembleURIf(HTTP_URI_CODING_ALL, uri, sizeof(uri), "ipp", NULL,
			"localhost", 631, "/printers/%s", s->target.queue);
	request = ippNewRequest(IPP_OP_GET_PRINTER_ATTRIBUTES);
	ippAddString(request, IPP_TAG_OPERATION, IPP_TAG_URI, "printer-uri", NULL, uri);
	ippAddStrings(request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD, "requested-attributes",
		      G_N_ELEMENTS(attributes), NULL, attributes);
	response = cupsDoRequest(http, request, "/");
	s->cups_valid = response && ippGetStatusCode(response) <= IPP_STATUS_OK_EVENTS_COMPLETE;
	if (s->cups_valid) {
		s->queue_state = ippGetInteger(ippFindAttribute(response, "printer-state", IPP_TAG_ENUM), 0);
		g_strlcpy(s->queue_message, attribute_string(response, "printer-state-message"), sizeof(s->queue_message));
		attr = ippFindAttribute(response, "printer-state-reasons", IPP_TAG_KEYWORD);
		for (i = 0; i < (unsigned int)ippGetCount(attr); i++) {
			const char *reason = ippGetString(attr, i, NULL);

			if (!strcmp(reason, "none"))
				continue;
			if (s->queue_reasons[0])
				g_strlcat(s->queue_reasons, ", ", sizeof(s->queue_reasons));
			g_strlcat(s->queue_reasons, reason, sizeof(s->queue_reasons));
		}
		if (!s->target.host[0])
			monitor_uri_host(attribute_string(response, "device-uri"),
					 s->target.host, sizeof(s->target.host));
	}
	ippDelete(response);
	read_jobs(http, uri, s);
close:
	httpClose(http);
}

void monitor_poll(const struct monitor_target *target, struct monitor_snapshot *s)
{
	memset(s, 0, sizeof(*s));
	s->target = *target;
	s->brother_status = s->printer_status = s->device_status = -1;
	read_cups(s);
	read_printer(s);
	s->timestamp = g_get_real_time();
	monitor_classify(s);
}
