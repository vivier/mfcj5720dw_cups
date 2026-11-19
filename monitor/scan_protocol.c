/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Laurent Vivier */
#include "scan_protocol.h"

#include <string.h>

static const char *const functions[] = { "IMAGE", "EMAIL", "OCR", "FILE" };
static const unsigned int application_numbers[] = { 1, 2, 3, 5 };

char *scan_key_registration(const char *name, const char *address,
			   enum scan_destination destination)
{
	g_return_val_if_fail((unsigned int)destination < SCAN_DESTINATIONS, NULL);
	return g_strdup_printf("TYPE=BR;BUTTON=SCAN;USER=\"%s\";FUNC=%s;"
		"HOST=%s:%u;APPNUM=%u;DURATION=%u;BRID=;", name,
		functions[destination], address, SCAN_KEY_PORT,
		application_numbers[destination], SCAN_KEY_LEASE);
}

static gboolean decimal(const char *text, guint32 *number)
{
	guint64 value = 0;
	const char *p;

	if (!text || !*text)
		return FALSE;
	for (p = text; *p; p++) {
		if (!g_ascii_isdigit(*p))
			return FALSE;
		value = value * 10 + (*p - '0');
		if (value > G_MAXUINT32)
			return FALSE;
	}
	*number = value;
	return TRUE;
}

gboolean scan_key_parse(const guint8 *packet, size_t size, const char *name,
			const char *address, struct scan_key_event *event)
{
	g_autofree char *payload = NULL;
	g_autofree char *user = g_strdup_printf("\"%s\"", name);
	g_autofree char *host = g_strdup_printf("%s:%u", address, SCAN_KEY_PORT);
	g_autoptr(GHashTable) fields = g_hash_table_new(g_str_hash, g_str_equal);
	g_auto(GStrv) parts = NULL;
	const char *function;
	guint32 application;
	size_t declared;
	unsigned int i;

	/*
	 * The vendor fixture counts payload bytes. The MFC-J5720DW capture
	 * advertises the entire datagram length plus one; no NUL is transmitted.
	 */
	if (size < 5 || size > SCAN_KEY_PACKET_MAX)
		return FALSE;
	declared = ((size_t)packet[1] << 8) | packet[2];
	if (packet[0] != 2 ||
	    packet[3] != '0' || (declared != size - 4 && declared != size + 1) ||
	    packet[size - 1] != ';')
		return FALSE;
	for (i = 4; i < size; i++) {
		if (packet[i] < 0x20 || packet[i] > 0x7e)
			return FALSE;
	}
	payload = g_strndup((const char *)packet + 4, size - 4);
	parts = g_strsplit(payload, ";", -1);
	for (i = 0; parts[i]; i++) {
		char *equals = strchr(parts[i], '=');

		if (!parts[i][0]) {
			if (parts[i + 1])
				return FALSE;
			break;
		}
		if (!equals || equals == parts[i])
			return FALSE;
		*equals = 0;
		if (g_hash_table_contains(fields, parts[i]))
			return FALSE;
		g_hash_table_insert(fields, parts[i], equals + 1);
	}
	if (g_strcmp0(g_hash_table_lookup(fields, "TYPE"), "BR") ||
	    g_strcmp0(g_hash_table_lookup(fields, "BUTTON"), "SCAN") ||
	    g_strcmp0(g_hash_table_lookup(fields, "USER"), user) ||
	    g_strcmp0(g_hash_table_lookup(fields, "HOST"), host) ||
	    !decimal(g_hash_table_lookup(fields, "APPNUM"), &application) ||
	    !decimal(g_hash_table_lookup(fields, "REGID"), &event->registration) ||
	    !decimal(g_hash_table_lookup(fields, "SEQ"), &event->sequence))
		return FALSE;
	function = g_hash_table_lookup(fields, "FUNC");
	for (i = 0; i < SCAN_DESTINATIONS; i++) {
		if (!g_strcmp0(function, functions[i]) && application == application_numbers[i]) {
			event->destination = i;
			return TRUE;
		}
	}
	return FALSE;
}
