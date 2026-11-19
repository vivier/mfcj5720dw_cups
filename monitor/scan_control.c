/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Laurent Vivier */
#include "scan_control.h"

#include <glib/gi18n.h>
#include <string.h>

#define SCANNER_PORT 54921
#define CONTROL_TIMEOUT_SECONDS 5
#define CONTROL_PAYLOAD_MAX 1024
#define CONTROL_REPLY 0x30
#define CONTROL_END 0x80

gboolean scan_control_parse(const guint8 *packet, size_t size,
			    enum scan_destination destination,
			    enum scan_sides *sides, GError **error)
{
	static const char *const functions[] = { "IMAGE", "EMAIL", "OCR", "FILE" };
	g_autoptr(GHashTable) fields = g_hash_table_new(g_str_hash, g_str_equal);
	g_autofree char *text = NULL;
	g_auto(GStrv) lines = NULL;
	const char *duplex, *edge;
	size_t i;

	/* Replies have a three-byte header and a little-endian payload length,
	 * including the final 80 byte. TCP packet boundaries are not framing.
	 */
	if (size < 5 || size > 3 + CONTROL_PAYLOAD_MAX || packet[0] != CONTROL_REPLY ||
	    (packet[1] | ((size_t)packet[2] << 8)) != size - 3 ||
	    packet[size - 1] != CONTROL_END || packet[size - 2] != '\n' ||
	    (unsigned int)destination >= G_N_ELEMENTS(functions))
		goto invalid;
	for (i = 3; i < size - 1; i++) {
		if (packet[i] != '\n' && (packet[i] < 0x20 || packet[i] > 0x7e))
			goto invalid;
	}
	text = g_strndup((const char *)packet + 3, size - 4);
	lines = g_strsplit(text, "\n", -1);
	for (i = 0; lines[i]; i++) {
		char *equals = strchr(lines[i], '=');

		if (!lines[i][0] && !lines[i + 1])
			break;
		if (!equals || equals == lines[i] || !equals[1])
			goto invalid;
		*equals = 0;
		if (g_hash_table_contains(fields, lines[i]))
			goto invalid;
		g_hash_table_insert(fields, lines[i], equals + 1);
	}
	if (g_strcmp0(g_hash_table_lookup(fields, "F"), functions[destination]))
		goto invalid;
	duplex = g_hash_table_lookup(fields, "D");
	edge = g_hash_table_lookup(fields, "E");
	if (!g_strcmp0(duplex, "SIN"))
		*sides = SCAN_ONE_SIDED;
	else if (!g_strcmp0(duplex, "DUP") && !g_strcmp0(edge, "LON"))
		*sides = SCAN_LONG_EDGE;
	else if (!g_strcmp0(duplex, "DUP") && !g_strcmp0(edge, "SHO"))
		*sides = SCAN_SHORT_EDGE;
	else
		goto invalid;
	return TRUE;
invalid:
	g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
		_("The scanner returned invalid or unsupported panel scan settings."));
	return FALSE;
}

static gboolean read_exact(GInputStream *input, void *buffer, size_t size,
			   GCancellable *cancel, GError **error)
{
	gsize received;

	if (!g_input_stream_read_all(input, buffer, size, &received, cancel, error))
		return FALSE;
	if (received == size)
		return TRUE;
	g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PARTIAL_INPUT,
		_("The scanner closed the connection before sending its panel settings."));
	return FALSE;
}

gboolean scan_control_query(const char *printer, enum scan_destination destination,
			    enum scan_sides *sides, GCancellable *cancel,
			    GString *log, GError **error)
{
	/* ESC K, protocol 10, terminator: the vendor's scan-key settings query. */
	static const guint8 request[] = { 0x1b, 'K', 0x0a, CONTROL_END };
	static const char greeting[] = "+OK 200\r\n";
	g_autoptr(GSocketClient) client = g_socket_client_new();
	g_autoptr(GSocketConnection) connection = NULL;
	GInputStream *input;
	GOutputStream *output;
	guint8 packet[3 + CONTROL_PAYLOAD_MAX];
	char hello[sizeof(greeting) - 1];
	size_t length;

	g_socket_client_set_enable_proxy(client, FALSE);
	g_socket_client_set_timeout(client, CONTROL_TIMEOUT_SECONDS);
	connection = g_socket_client_connect_to_host(client, printer, SCANNER_PORT, cancel, error);
	if (!connection)
		return FALSE;
	input = g_io_stream_get_input_stream(G_IO_STREAM(connection));
	output = g_io_stream_get_output_stream(G_IO_STREAM(connection));
	if (!read_exact(input, hello, sizeof(hello), cancel, error))
		return FALSE;
	if (memcmp(hello, greeting, sizeof(hello))) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
			_("Unexpected scanner greeting while reading the panel settings."));
		return FALSE;
	}
	if (!g_output_stream_write_all(output, request, sizeof(request), NULL, cancel, error) ||
	    !read_exact(input, packet, 1, cancel, error))
		return FALSE;
	/* A one-byte refusal (observed: d0 when idle) has no length field. */
	if (packet[0] != CONTROL_REPLY) {
		g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
			_("No panel scan settings are available (reply 0x%02x). Start the scan again from the printer."), packet[0]);
		return FALSE;
	}
	if (!read_exact(input, packet + 1, 2, cancel, error))
		return FALSE;
	length = packet[1] | ((size_t)packet[2] << 8);
	if (!length || length > CONTROL_PAYLOAD_MAX) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
			_("Invalid panel settings reply length."));
		return FALSE;
	}
	if (!read_exact(input, packet + 3, length, cancel, error))
		return FALSE;
	if (!scan_control_parse(packet, length + 3, destination, sides, error))
		return FALSE;
	g_string_append_printf(log, "Printer panel settings:\n%.*s\n", (int)length - 1, packet + 3);
	return TRUE;
}
