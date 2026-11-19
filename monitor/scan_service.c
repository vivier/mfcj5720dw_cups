/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Laurent Vivier */
#include "scan_service.h"
#include "scan_protocol.h"

#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-includes.h>
#include <glib/gi18n.h>
#include <string.h>

/* A bounded replay cache covers both duplicate packets and delayed retries. */
#define SCAN_REPLAY_SLOTS 32
#define SCAN_REPLAY_SECONDS 120
#define SCAN_JOB_TIMEOUT_SECONDS 1200

struct scan_connection {
	GSocket *socket;
	GInetAddress *printer;
	char local[16];
	char remote[16];
};

struct scan_service {
	unsigned int refs;
	char *host;
	struct scan_settings settings;
	struct scan_connection *connection;
	GCancellable *registration_cancel;
	GCancellable *job_cancel;
	GSource *listener;
	guint timer;
	guint job_timer;
	guint start_timer;
	gboolean registering;
	gboolean busy;
	gboolean stopped;
	GMutex job_lock;
	GCond job_finished;
	gboolean worker_running;
	gint64 registered_until;
	gint64 renew_at;
	struct scan_key_event recent[SCAN_REPLAY_SLOTS];
	gint64 recent_at[SCAN_REPLAY_SLOTS];
	unsigned int recent_next;
	struct scan_job job;
	scan_callback callback;
	gpointer data;
};

static gboolean register_timer(gpointer data);

static void connection_free(struct scan_connection *connection)
{
	if (!connection)
		return;
	g_clear_object(&connection->socket);
	g_clear_object(&connection->printer);
	g_free(connection);
}

static struct scan_service *service_ref(struct scan_service *service)
{
	service->refs++;
	return service;
}

static void service_unref(struct scan_service *service)
{
	if (--service->refs)
		return;
	connection_free(service->connection);
	g_clear_object(&service->registration_cancel);
	g_clear_object(&service->job_cancel);
	g_cond_clear(&service->job_finished);
	g_mutex_clear(&service->job_lock);
	g_free(service->host);
	g_free(service);
}

static void report(struct scan_service *service, enum scan_activity activity,
		   const char *message, const struct scan_result *result)
{
	if (service->callback)
		service->callback(activity, message, service->job.destination, result, service->data);
}

static struct scan_connection *connect_printer(struct scan_service *service, GError **error)
{
	struct scan_connection *connection = g_new0(struct scan_connection, 1);
	g_autoptr(GResolver) resolver = g_resolver_get_default();
	g_autoptr(GSocket) route = NULL;
	g_autoptr(GSocketAddress) peer = NULL, local = NULL, bind = NULL;
	g_autoptr(GInetAddress) any = g_inet_address_new_any(G_SOCKET_FAMILY_IPV4);
	GList *addresses = g_resolver_lookup_by_name_with_flags(resolver, service->host,
		G_RESOLVER_NAME_LOOKUP_FLAGS_IPV4_ONLY, service->registration_cancel, error);
	g_autofree char *ip = NULL, *source = NULL;

	if (!addresses)
		goto failed;
	connection->printer = g_object_ref(addresses->data);
	g_resolver_free_addresses(addresses);
	if (g_inet_address_get_is_any(connection->printer) ||
	    g_inet_address_get_is_multicast(connection->printer)) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
			_("Scan-to-PC needs a unicast IPv4 printer address."));
		goto failed;
	}
	ip = g_inet_address_to_string(connection->printer);
	g_strlcpy(connection->remote, ip, sizeof(connection->remote));
	route = g_socket_new(G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_DATAGRAM,
			     G_SOCKET_PROTOCOL_UDP, error);
	peer = g_inet_socket_address_new(connection->printer, 161);
	if (!route || !g_socket_connect(route, peer, service->registration_cancel, error))
		goto failed;
	local = g_socket_get_local_address(route, error);
	if (!local)
		goto failed;
	source = g_inet_address_to_string(g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(local)));
	g_strlcpy(connection->local, service->settings.address[0] ?
		  service->settings.address : source, sizeof(connection->local));
	g_clear_object(&route);
	connection->socket = g_socket_new(G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_DATAGRAM,
					 G_SOCKET_PROTOCOL_UDP, error);
	bind = g_inet_socket_address_new(any, SCAN_KEY_PORT);
	/* Exclusive bind: two user sessions must not steal each other's scan events. */
	if (!connection->socket || !g_socket_bind(connection->socket, bind, FALSE, error))
		goto failed;
	g_socket_set_blocking(connection->socket, FALSE);
	return connection;
failed:
	connection_free(connection);
	return NULL;
}

static gboolean register_destinations(struct scan_service *service,
				     struct scan_connection *connection, GError **error)
{
	static const oid registration_oid[] = { 1, 3, 6, 1, 4, 1, 2435, 2, 3, 9, 2, 11, 1, 1, 0 };
	netsnmp_session config;
	netsnmp_pdu *request, *response = NULL;
	g_autofree char *peer = g_strdup_printf("udp:%s:161", connection->remote);
	void *session;
	unsigned int i;
	gboolean ok = FALSE;

	if (g_cancellable_set_error_if_cancelled(service->registration_cancel, error))
		return FALSE;
	snmp_sess_init(&config);
	config.peername = peer;
	config.version = SNMP_VERSION_1;
	config.community = (unsigned char *)"internal";
	config.community_len = strlen("internal");
	config.timeout = 1500000;
	config.retries = 0;
	session = snmp_sess_open(&config);
	if (!session)
		goto failed;
	request = snmp_pdu_create(SNMP_MSG_SET);
	if (!request)
		goto close;
	for (i = 0; i < SCAN_DESTINATIONS; i++) {
		g_autofree char *value = scan_key_registration(service->settings.name, connection->local, i);

		snmp_pdu_add_variable(request, registration_oid, G_N_ELEMENTS(registration_oid),
				      ASN_OCTET_STR, (const u_char *)value, strlen(value));
	}
	ok = snmp_sess_synch_response(session, request, &response) == STAT_SUCCESS &&
		response && response->errstat == SNMP_ERR_NOERROR;
	if (response)
		snmp_free_pdu(response);
close:
	snmp_sess_close(session);
failed:
	if (!ok)
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
			_("Cannot register this PC with the printer. Check its address and SNMP access."));
	return ok;
}

static void register_worker(GTask *task, gpointer object, gpointer data, GCancellable *cancel)
{
	struct scan_service *service = data;
	struct scan_connection *connection = service->connection;
	g_autoptr(GError) error = NULL;

	(void)object; (void)cancel;
	if (!connection)
		connection = connect_printer(service, &error);
	if (!connection || !register_destinations(service, connection, &error)) {
		if (connection != service->connection)
			connection_free(connection);
		g_task_return_error(task, g_steal_pointer(&error));
		return;
	}
	/* The service owns the existing connection; only the first result transfers it. */
	g_task_return_pointer(task, connection == service->connection ? NULL : connection,
			      (GDestroyNotify)connection_free);
}

static gboolean job_timeout(gpointer data)
{
	struct scan_service *service = data;

	service->job_timer = 0;
	g_cancellable_cancel(service->job_cancel);
	return G_SOURCE_REMOVE;
}

static void job_worker(GTask *task, gpointer object, gpointer data, GCancellable *cancel)
{
	struct scan_service *service = data;
	struct scan_result *result;

	(void)object;
	result = scan_job_run(&service->job, cancel);
	g_mutex_lock(&service->job_lock);
	service->worker_running = FALSE;
	g_cond_signal(&service->job_finished);
	g_mutex_unlock(&service->job_lock);
	g_task_return_pointer(task, result, (GDestroyNotify)scan_result_free);
}

static void job_done(GObject *object, GAsyncResult *async, gpointer data)
{
	struct scan_service *service = data;
	struct scan_result *result = g_task_propagate_pointer(G_TASK(async), NULL);
	g_autofree char *message = NULL;

	(void)object;
	if (service->job_timer)
		g_source_remove(service->job_timer);
	service->job_timer = 0;
	service->busy = FALSE;
	g_clear_object(&service->job_cancel);
	if (result->error) {
		message = result->directory ? g_strdup_printf("%s\n%s", result->error->message,
							    result->directory) : g_strdup(result->error->message);
		report(service, SCAN_FAILED, message, result);
	} else {
		message = g_strdup_printf(_("Scan saved (%u pages): %s"), result->pages, result->document);
		report(service, SCAN_FINISHED, message, result);
	}
	scan_result_free(result);
	service_unref(service);
}

static gboolean start_job(gpointer data)
{
	struct scan_service *service = data;
	g_autoptr(GTask) task = g_task_new(NULL, service->job_cancel, job_done, service_ref(service));

	service->start_timer = 0;
	/* Preserve error/output paths even when a scan is cancelled. */
	g_task_set_check_cancellable(task, FALSE);
	g_task_set_task_data(task, service, NULL);
	service->job_timer = g_timeout_add_seconds(SCAN_JOB_TIMEOUT_SECONDS, job_timeout, service);
	g_mutex_lock(&service->job_lock);
	service->worker_running = TRUE;
	g_mutex_unlock(&service->job_lock);
	g_task_run_in_thread(task, job_worker);
	return G_SOURCE_REMOVE;
}

static gboolean receive_event(GSocket *socket, GIOCondition condition, gpointer data)
{
	struct scan_service *service = data;
	guint8 packet[SCAN_KEY_PACKET_MAX + 1];
	g_autoptr(GSocketAddress) peer = NULL;
	struct scan_key_event event;
	gint64 now = g_get_monotonic_time();
	gssize size;
	unsigned int i;

	if (condition & (G_IO_ERR | G_IO_HUP))
		return G_SOURCE_CONTINUE;
	size = g_socket_receive_from(socket, &peer, (char *)packet, sizeof(packet), NULL, NULL);
	if (size < 0 || !G_IS_INET_SOCKET_ADDRESS(peer) ||
	    !g_inet_address_equal(service->connection->printer,
		g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(peer))) ||
	    now >= service->registered_until ||
	    !scan_key_parse(packet, size, service->settings.name, service->connection->local, &event))
		return G_SOURCE_CONTINUE;
	for (i = 0; i < SCAN_REPLAY_SLOTS; i++) {
		if (service->recent_at[i] &&
		    service->recent[i].registration == event.registration &&
		    service->recent[i].sequence == event.sequence &&
		    service->recent[i].destination == event.destination &&
		    now - service->recent_at[i] < SCAN_REPLAY_SECONDS * G_USEC_PER_SEC)
			return G_SOURCE_CONTINUE;
	}
	i = service->recent_next++ % SCAN_REPLAY_SLOTS;
	service->recent[i] = event;
	service->recent_at[i] = now;
	/* Do not queue more paper acquisition while the scanner is already in use. */
	if (service->busy)
		return G_SOURCE_CONTINUE;
	g_debug("Scan event: %.*s", (int)size - 4, (char *)packet + 4);
	service->job.settings = service->settings;
	service->job.destination = event.destination;
	g_strlcpy(service->job.printer, service->connection->remote, sizeof(service->job.printer));
	service->job_cancel = g_cancellable_new();
	service->busy = TRUE;
	report(service, SCAN_RECEIVING, _("Scan request received; acquiring the document…"), NULL);
	/* The panel needs a short interval to release the scanner after notifying us. */
	service->start_timer = g_timeout_add(2000, start_job, service);
	return G_SOURCE_CONTINUE;
}

static void register_done(GObject *object, GAsyncResult *async, gpointer data)
{
	struct scan_service *service = data;
	g_autoptr(GError) error = NULL;
	struct scan_connection *connection = g_task_propagate_pointer(G_TASK(async), &error);
	gint64 now = g_get_monotonic_time();
	g_autofree char *message = NULL;

	(void)object;
	service->registering = FALSE;
	if (service->stopped) {
		connection_free(connection);
		goto out;
	}
	if (error) {
		service->renew_at = now + 30 * G_USEC_PER_SEC;
		if (!service->busy)
			report(service, SCAN_FAILED, error->message, NULL);
		goto out;
	}
	if (connection) {
		service->connection = connection;
		service->listener = g_socket_create_source(connection->socket, G_IO_IN, NULL);
		g_source_set_callback(service->listener, G_SOURCE_FUNC(receive_event), service, NULL);
		g_source_attach(service->listener, NULL);
	}
	service->registered_until = now + SCAN_KEY_LEASE * G_USEC_PER_SEC;
	service->renew_at = now + SCAN_KEY_RENEWAL * G_USEC_PER_SEC;
	message = g_strdup_printf(_("Registered as %s — waiting for a scan from the printer."), service->settings.name);
	if (!service->busy)
		report(service, SCAN_WAITING, message, NULL);
out:
	service_unref(service);
}

static gboolean register_timer(gpointer data)
{
	struct scan_service *service = data;
	g_autoptr(GTask) task = NULL;

	if (service->registering || g_get_monotonic_time() < service->renew_at)
		return G_SOURCE_CONTINUE;
	service->registering = TRUE;
	task = g_task_new(NULL, service->registration_cancel, register_done, service_ref(service));
	g_task_set_task_data(task, service, NULL);
	g_task_run_in_thread(task, register_worker);
	return G_SOURCE_CONTINUE;
}

struct scan_service *scan_service_new(const char *printer,
				     const struct scan_settings *settings,
				     scan_callback callback, gpointer data)
{
	struct scan_service *service = g_new0(struct scan_service, 1);

	service->refs = 1;
	g_mutex_init(&service->job_lock);
	g_cond_init(&service->job_finished);
	service->host = g_strdup(printer);
	service->settings = *settings;
	service->callback = callback;
	service->data = data;
	service->registration_cancel = g_cancellable_new();
	service->timer = g_timeout_add_seconds(5, register_timer, service);
	register_timer(service);
	return service;
}

gboolean scan_service_busy(const struct scan_service *service)
{
	return service && service->busy;
}

void scan_service_cancel(struct scan_service *service)
{
	if (service && service->job_cancel)
		g_cancellable_cancel(service->job_cancel);
}

void scan_service_free(struct scan_service *service)
{
	if (!service)
		return;
	service->stopped = TRUE;
	service->callback = NULL;
	g_source_remove(service->timer);
	if (service->start_timer) {
		g_source_remove(service->start_timer);
		service->start_timer = 0;
	}
	if (service->job_timer) {
		g_source_remove(service->job_timer);
		service->job_timer = 0;
	}
	if (service->listener) {
		g_source_destroy(service->listener);
		g_source_unref(service->listener);
	}
	g_cancellable_cancel(service->registration_cancel);
	scan_service_cancel(service);
	/* Quitting must not exit the process while the worker still owns PNGs. */
	g_mutex_lock(&service->job_lock);
	while (service->worker_running)
		g_cond_wait(&service->job_finished, &service->job_lock);
	g_mutex_unlock(&service->job_lock);
	/* Brother's own tool withdraws by letting the six-minute lease expire. */
	service_unref(service);
}
