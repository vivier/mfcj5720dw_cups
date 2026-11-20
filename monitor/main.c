/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Laurent Vivier */
#include "poll.h"
#include "scan_ui.h"
#include "tray.h"
#include "maintenance_ui.h"

#include <gtk/gtk.h>
#include <cups/ipp.h>
#include <glib/gi18n.h>
#include <locale.h>
#include <stdio.h>
#include <string.h>

#ifndef LOCALEDIR
#define LOCALEDIR "/usr/share/locale"
#endif
#define APP_ID "io.github.vivier.mfcj5720dw.monitor"
#define GETTEXT_DOMAIN "mfcj5720dw-monitor"
#ifndef MONITOR_VERSION
#define MONITOR_VERSION "development"
#endif

struct monitor_settings {
	struct monitor_target target;
	struct scan_settings scan;
	gboolean notify_enabled;
	gboolean keep_running;
	gboolean start_at_login;
};

enum settings_page {
	SETTINGS_PRINTER,
	SETTINGS_NOTIFICATIONS,
	SETTINGS_SCAN,
	SETTINGS_COUNT,
};

struct settings_window {
	struct monitor_app *app;
	GtkWindow *window;
	GtkStack *pages;
	GtkBox *content[SETTINGS_COUNT];
	GtkLabel *error;
};

struct monitor_app {
	GtkApplication *application;
	GtkWindow *window;
	struct monitor_tray *tray;
	GtkDropDown *queues;
	GtkEntry *host;
	GtkEntry *community;
	GtkLabel *status;
	GtkLabel *details;
	GtkLabel *updated;
	GtkLabel *identity;
	GtkProgressBar *ink[4];
	GtkLabel *ink_value[4];
	GtkCheckButton *notifications;
	GtkCheckButton *background;
	GtkCheckButton *autostart;
	struct scan_panel *scan_panel;
	struct maintenance_panel *maintenance_panel;
	struct monitor_settings settings;
	struct settings_window dialog;
	GtkMenuButton *menu;
	GtkWindow *about;
	struct monitor_snapshot *previous;
	GStrv queue_names;
	unsigned int revision;
	unsigned int failures;
	gboolean polling;
	gboolean start_hidden;
	gboolean initialized;
	gboolean problem_reported;
};

struct poll_request {
	struct monitor_target target;
	unsigned int revision;
};

static gboolean refresh(gpointer data);

static char *settings_path(void)
{
	return g_build_filename(g_get_user_config_dir(), GETTEXT_DOMAIN,
				"settings.ini", NULL);
}

static void load_settings(struct monitor_app *app)
{
	g_autoptr(GKeyFile) settings = g_key_file_new();
	g_autofree char *path = settings_path();
	g_autofree char *queue = NULL, *host = NULL, *community = NULL;

	g_strlcpy(app->settings.target.community, "public", sizeof(app->settings.target.community));
	app->settings.notify_enabled = TRUE;
	app->settings.keep_running = TRUE;
	scan_settings_defaults(&app->settings.scan);
	if (!g_key_file_load_from_file(settings, path, G_KEY_FILE_NONE, NULL))
		return;
	scan_settings_load(&app->settings.scan, settings);
	queue = g_key_file_get_string(settings, "Monitor", "queue", NULL);
	host = g_key_file_get_string(settings, "Monitor", "host", NULL);
	community = g_key_file_get_string(settings, "Monitor", "community", NULL);
	g_strlcpy(app->settings.target.queue, queue ? queue : "", sizeof(app->settings.target.queue));
	g_strlcpy(app->settings.target.host, host ? host : "", sizeof(app->settings.target.host));
	if (community && *community)
		g_strlcpy(app->settings.target.community, community, sizeof(app->settings.target.community));
	app->settings.notify_enabled = g_key_file_get_boolean(settings, "Monitor", "notifications", NULL);
	app->settings.keep_running = g_key_file_get_boolean(settings, "Monitor", "background", NULL);
	app->settings.start_at_login = g_key_file_get_boolean(settings, "Monitor", "autostart", NULL);
}

static gboolean save_settings(const struct monitor_settings *values, GError **error)
{
	g_autoptr(GKeyFile) settings = g_key_file_new();
	g_autofree char *path = settings_path();
	g_autofree char *directory = g_path_get_dirname(path);
	g_autofree char *text = NULL;

	g_key_file_set_string(settings, "Monitor", "queue", values->target.queue);
	g_key_file_set_string(settings, "Monitor", "host", values->target.host);
	g_key_file_set_string(settings, "Monitor", "community", values->target.community);
	g_key_file_set_boolean(settings, "Monitor", "notifications", values->notify_enabled);
	g_key_file_set_boolean(settings, "Monitor", "background", values->keep_running);
	g_key_file_set_boolean(settings, "Monitor", "autostart", values->start_at_login);
	scan_settings_save(&values->scan, settings);
	text = g_key_file_to_data(settings, NULL, NULL);
	if (g_mkdir_with_parents(directory, 0700) < 0) {
		g_set_error_literal(error, G_FILE_ERROR, g_file_error_from_errno(errno), g_strerror(errno));
		return FALSE;
	}
	return g_file_set_contents_full(path, text, -1,
				       G_FILE_SET_CONTENTS_CONSISTENT, 0600, error);
}

static gboolean save_autostart(gboolean enabled, GError **error)
{
	g_autofree char *directory = g_build_filename(g_get_user_config_dir(), "autostart", NULL);
	g_autofree char *path = g_build_filename(directory, APP_ID ".desktop", NULL);
	g_autofree char *text = g_strdup_printf(
		"[Desktop Entry]\nType=Application\nName=Brother MFC-J5720DW Monitor\n"
		"Exec=mfcj5720dw-monitor --background\nIcon=printer\nHidden=%s\n",
		enabled ? "false" : "true");

	if (!enabled && !g_file_test(path, G_FILE_TEST_EXISTS))
		return TRUE;
	if (g_mkdir_with_parents(directory, 0700) < 0) {
		g_set_error_literal(error, G_FILE_ERROR, g_file_error_from_errno(errno), g_strerror(errno));
		return FALSE;
	}
	return g_file_set_contents_full(path, text, -1,
				       G_FILE_SET_CONTENTS_CONSISTENT, 0600, error);
}

static void notify(struct monitor_app *app, const char *id, const char *title,
		   const char *body, gboolean urgent)
{
	g_autoptr(GNotification) notification = g_notification_new(title);
	g_autoptr(GIcon) icon = g_themed_icon_new("printer");

	if (!app->settings.notify_enabled)
		return;
	g_notification_set_body(notification, body);
	g_notification_set_icon(notification, icon);
	g_notification_set_default_action(notification, "app.show");
	g_notification_set_priority(notification, urgent ?
		G_NOTIFICATION_PRIORITY_HIGH : G_NOTIFICATION_PRIORITY_NORMAL);
	g_application_send_notification(G_APPLICATION(app->application), id, notification);
}

static void send_changes(struct monitor_app *app, const struct monitor_snapshot *s)
{
	const struct monitor_snapshot *old = app->previous;
	unsigned int i, j;
	g_autofree char *body = NULL;

	if (!s->target.host[0])
		return;
	body = g_strdup_printf("%s\n%s", s->target.queue[0] ? s->target.queue :
			       s->target.host, s->details);
	if (!s->online)
		app->failures++;
	else
		app->failures = 0;
	/* Debounce a lost packet; never turn an unsuccessful query into recovery. */
	if (app->failures == 2) {
		notify(app, "printer", _("Printer is not responding"), body, TRUE);
		app->problem_reported = app->settings.notify_enabled;
	} else if (s->online && (!old || monitor_issue_changed(old, s))) {
		if (s->severity >= MONITOR_WARNING) {
			notify(app, "printer", s->summary, body, s->severity == MONITOR_ERROR);
			app->problem_reported = app->settings.notify_enabled;
		} else if (app->problem_reported && s->severity == MONITOR_OK) {
			notify(app, "printer", _("Printer is available again"), body, FALSE);
			app->problem_reported = FALSE;
		}
	}
	if (s->cups_valid && (!old || s->queue_state != old->queue_state) &&
	    s->queue_state == IPP_PSTATE_STOPPED)
		notify(app, "queue", _("Print queue is paused"), s->queue_message, TRUE);
	if (!old || !s->jobs_valid || !old->jobs_valid)
		return;
	for (i = 0; i < s->job_count; i++) {
		/* Automatic supply queries are not document-delivery notifications. */
		if (s->jobs[i].id == s->target.ink_job_id || s->jobs[i].id == old->target.ink_job_id)
			continue;
		for (j = 0; j < old->job_count; j++) {
			if (monitor_job_finished(&old->jobs[j], &s->jobs[i])) {
				char id[64];

				g_snprintf(id, sizeof(id), "job-%d", s->jobs[i].id);
				notify(app, id, monitor_job_state(s->jobs[i].state),
				       s->jobs[i].title, s->jobs[i].state == IPP_JSTATE_ABORTED);
			}
		}
	}
}

static void update_queues(struct monitor_app *app, const struct monitor_snapshot *s)
{
	GtkStringList *list = gtk_string_list_new(NULL);
	GPtrArray *names = g_ptr_array_new();
	const char *selected = app->settings.target.queue;
	unsigned int selection = 0, i;

	/* Do not replace a pending user selection during the next poll. */
	if (app->queue_names && gtk_widget_get_visible(GTK_WIDGET(app->dialog.window))) {
		unsigned int current = gtk_drop_down_get_selected(app->queues);

		if (current < g_strv_length(app->queue_names))
			selected = app->queue_names[current];
	}
	gtk_string_list_append(list, _("Direct connection (no queue)"));
	g_ptr_array_add(names, g_strdup(""));
	for (i = 0; i < s->queue_count; i++) {
		gtk_string_list_append(list, s->queues[i].name);
		g_ptr_array_add(names, g_strdup(s->queues[i].name));
		if (!strcmp(s->queues[i].name, selected))
			selection = i + 1;
	}
	if (selected[0] && !selection) {
		selection = names->len;
		gtk_string_list_append(list, selected);
		g_ptr_array_add(names, g_strdup(selected));
	}
	g_ptr_array_add(names, NULL);
	g_strfreev(app->queue_names);
	app->queue_names = (GStrv)g_ptr_array_free(names, FALSE);
	gtk_drop_down_set_model(app->queues, G_LIST_MODEL(list));
	gtk_drop_down_set_selected(app->queues, selection);
	g_object_unref(list);
}

static void show_snapshot(struct monitor_app *app, const struct monitor_snapshot *s)
{
	g_autoptr(GDateTime) now = g_date_time_new_from_unix_local(s->timestamp / G_USEC_PER_SEC);
	g_autofree char *time = g_date_time_format(now, "%X");
	g_autofree char *updated = g_strdup_printf(_("Last update: %s"), time);
	g_autofree char *identity = s->target.queue[0] ?
		g_strdup_printf("%s — %s", s->target.queue, s->target.host[0] ? s->target.host : _("No address")) :
		g_strdup(s->target.host[0] ? s->target.host : _("No printer selected"));
	unsigned int i;

	update_queues(app, s);
	gtk_label_set_text(app->status, s->summary);
	gtk_label_set_text(app->details, s->details);
	gtk_label_set_text(app->updated, updated);
	gtk_label_set_text(app->identity, identity);
	for (i = 0; i < 4; i++) {
		char text[32];

		if (s->ink_valid)
			g_snprintf(text, sizeof(text), "%.0f %%", s->ink[i] / 100.0);
		else
			g_strlcpy(text, _("Unavailable"), sizeof(text));
		gtk_progress_bar_set_fraction(app->ink[i], s->ink_valid ? s->ink[i] / 10000.0 : 0);
		gtk_progress_bar_set_text(app->ink[i], text);
		gtk_label_set_text(app->ink_value[i], text);
		gtk_widget_set_sensitive(GTK_WIDGET(app->ink[i]), s->ink_valid);
	}
}

static void poll_worker(GTask *task, gpointer source, gpointer task_data,
			GCancellable *cancellable)
{
	struct poll_request *request = task_data;
	struct monitor_snapshot *snapshot = g_new0(struct monitor_snapshot, 1);

	(void)source; (void)cancellable;
	monitor_poll(&request->target, snapshot);
	g_task_return_pointer(task, snapshot, g_free);
}

static void poll_done(GObject *source, GAsyncResult *result, gpointer data)
{
	struct monitor_app *app = data;
	struct poll_request *request = g_task_get_task_data(G_TASK(result));
	struct monitor_snapshot *snapshot = g_task_propagate_pointer(G_TASK(result), NULL);

	(void)source;
	app->polling = FALSE;
	if (request->revision != app->revision) {
		g_free(snapshot);
		refresh(app);
		return;
	}
	if (!app->initialized && !app->settings.target.queue[0] && !app->settings.target.host[0])
		g_strlcpy(app->settings.target.queue, snapshot->target.queue, sizeof(app->settings.target.queue));
	app->initialized = TRUE;
	send_changes(app, snapshot);
	show_snapshot(app, snapshot);
	maintenance_panel_follow(app->maintenance_panel, snapshot->target.queue,
				 snapshot->maintenance_commands, snapshot->queue_accepting_jobs);
	maintenance_panel_update_ink(app->maintenance_panel, snapshot);
	scan_panel_follow(app->scan_panel, snapshot->target.host, &app->settings.scan);
	g_free(app->previous);
	app->previous = snapshot;
}

static gboolean refresh(gpointer data)
{
	struct monitor_app *app = data;
	g_autoptr(GTask) task = NULL;
	struct poll_request *request;

	if (app->polling)
		return G_SOURCE_CONTINUE;
	app->polling = TRUE;
	request = g_new0(struct poll_request, 1);
	request->target = app->settings.target;
	request->target.ink_job_id = maintenance_panel_ink_job(app->maintenance_panel);
	request->revision = app->revision;
	task = g_task_new(app->application, NULL, poll_done, app);
	g_task_set_task_data(task, request, g_free);
	g_task_run_in_thread(task, poll_worker);
	return G_SOURCE_CONTINUE;
}

static gboolean valid_host(const char *host)
{
	const unsigned char *p = (const unsigned char *)host;

	/* Accept a bare DNS name, IPv4 or IPv6 address, not a URL or transport. */
	for (; *p; p++) {
		if (!g_ascii_isalnum(*p) && !strchr(".-:%_", *p))
			return FALSE;
	}
	return strlen(host) < sizeof(((struct monitor_target *)0)->host);
}

static void reset_settings_window(struct settings_window *dialog)
{
	struct monitor_app *app = dialog->app;
	struct monitor_snapshot empty = { 0 };

	gtk_label_set_text(dialog->error, "");
	gtk_widget_set_visible(GTK_WIDGET(dialog->error), FALSE);
	g_clear_pointer(&app->queue_names, g_strfreev);
	update_queues(app, app->previous ? app->previous : &empty);
	gtk_editable_set_text(GTK_EDITABLE(app->host), app->settings.target.host);
	gtk_editable_set_text(GTK_EDITABLE(app->community), app->settings.target.community);
	scan_panel_reset(app->scan_panel, &app->settings.scan);
	gtk_check_button_set_active(app->notifications, app->settings.notify_enabled);
	gtk_check_button_set_active(app->background, app->settings.keep_running);
	gtk_check_button_set_active(app->autostart, app->settings.start_at_login);
}

static void cancel_settings(GtkButton *button, gpointer data)
{
	struct settings_window *dialog = data;

	(void)button;
	reset_settings_window(dialog);
	gtk_widget_set_visible(GTK_WIDGET(dialog->window), FALSE);
}

static gboolean close_settings(GtkWindow *window, gpointer data)
{
	(void)window;
	cancel_settings(NULL, data);
	return TRUE;
}

static gboolean settings_key_pressed(GtkEventControllerKey *controller, guint key,
				     guint keycode, GdkModifierType state, gpointer data)
{
	(void)controller; (void)keycode; (void)state;
	if (key != GDK_KEY_Escape)
		return FALSE;
	cancel_settings(NULL, data);
	return TRUE;
}

static void resize_settings_window(struct settings_window *dialog)
{
	int width = MAX(620, gtk_widget_get_width(GTK_WIDGET(dialog->window)));
	int height;

	gtk_widget_measure(GTK_WIDGET(dialog->window), GTK_ORIENTATION_VERTICAL,
			   width, NULL, &height, NULL, NULL);
	gtk_window_set_default_size(dialog->window, width, height);
}

static void settings_page_changed(GObject *pages, GParamSpec *property, gpointer data)
{
	struct settings_window *dialog = data;

	(void)pages; (void)property;
	if (gtk_widget_get_visible(GTK_WIDGET(dialog->window)))
		resize_settings_window(dialog);
}

static void open_settings(GtkButton *button, gpointer data)
{
	struct settings_window *dialog = data;

	(void)button;
	if (!gtk_widget_get_visible(GTK_WIDGET(dialog->window)))
		reset_settings_window(dialog);
	gtk_window_present(dialog->window);
	/* Realize the title bar before including it in the natural height. */
	resize_settings_window(dialog);
}

static gboolean store_settings(struct monitor_app *app, const struct monitor_settings *settings,
			       GError **error)
{
	gboolean autostart_changed = settings->start_at_login != app->settings.start_at_login;

	if (autostart_changed && !save_autostart(settings->start_at_login, error))
		return FALSE;
	if (!save_settings(settings, error)) {
		g_autoptr(GError) rollback = NULL;

		if (autostart_changed && !save_autostart(app->settings.start_at_login, &rollback))
			g_prefix_error(error, _("Unable to restore login startup: %s. "), rollback->message);
		return FALSE;
	}
	app->settings = *settings;
	return TRUE;
}

static void confirm_settings(GtkButton *button, gpointer data)
{
	struct settings_window *dialog = data;
	struct monitor_app *app = dialog->app;
	struct monitor_settings settings = app->settings;
	g_autoptr(GError) error = NULL;
	unsigned int selected = gtk_drop_down_get_selected(app->queues);
	const char *host = gtk_editable_get_text(GTK_EDITABLE(app->host));
	const char *community = gtk_editable_get_text(GTK_EDITABLE(app->community));
	gboolean printer_changed, scan_changed;

	(void)button;
	if (!valid_host(host)) {
		g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
			_("Enter a host name or IP address, without a URL or port."));
		gtk_stack_set_visible_child_name(dialog->pages, "printers");
		goto failed;
	}
	if (app->queue_names && selected < g_strv_length(app->queue_names))
		g_strlcpy(settings.target.queue, app->queue_names[selected], sizeof(settings.target.queue));
	g_strlcpy(settings.target.host, host, sizeof(settings.target.host));
	g_strlcpy(settings.target.community, *community ? community : "public", sizeof(settings.target.community));
	if (!scan_panel_read(app->scan_panel, &settings.scan, &error)) {
		gtk_stack_set_visible_child_name(dialog->pages, "scan");
		goto failed;
	}
	settings.notify_enabled = gtk_check_button_get_active(app->notifications);
	settings.keep_running = gtk_check_button_get_active(app->background);
	settings.start_at_login = gtk_check_button_get_active(app->autostart);
	printer_changed = memcmp(&settings.target, &app->settings.target, sizeof(settings.target)) != 0;
	scan_changed = memcmp(&settings.scan, &app->settings.scan, sizeof(settings.scan)) != 0;
	if ((printer_changed || scan_changed) && scan_panel_busy(app->scan_panel)) {
		g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_BUSY,
			_("Wait for the current scan to finish, or cancel it before changing settings."));
		gtk_stack_set_visible_child_name(dialog->pages, printer_changed ? "printers" : "scan");
		goto failed;
	}
	if (!store_settings(app, &settings, &error))
		goto failed;
	cancel_settings(NULL, dialog);
	if (printer_changed) {
		app->revision++;
		app->failures = 0;
		app->problem_reported = FALSE;
		g_clear_pointer(&app->previous, g_free);
		scan_panel_follow(app->scan_panel, "", &app->settings.scan);
		maintenance_panel_follow(app->maintenance_panel, app->settings.target.queue, 0, FALSE);
		gtk_label_set_text(app->status, _("Checking…"));
		refresh(app);
	} else if (scan_changed) {
		const char *printer = app->previous ? app->previous->target.host : app->settings.target.host;

		scan_panel_follow(app->scan_panel, printer, &app->settings.scan);
	}
	return;
failed:
	gtk_label_set_text(dialog->error, error->message);
	gtk_widget_set_visible(GTK_WIDGET(dialog->error), TRUE);
}

static void settings_parent_visibility(GObject *parent, GParamSpec *property, gpointer data)
{
	struct monitor_app *app = data;

	(void)property;
	if (gtk_widget_get_visible(GTK_WIDGET(parent)))
		return;
	if (gtk_widget_get_visible(GTK_WIDGET(app->dialog.window)))
		cancel_settings(NULL, &app->dialog);
	if (app->about)
		gtk_widget_set_visible(GTK_WIDGET(app->about), FALSE);
}

static void destroy_settings_windows(struct monitor_app *app)
{
	g_signal_handlers_disconnect_by_func(app->window, settings_parent_visibility, app);
	if (app->about) {
		gtk_window_destroy(app->about);
		g_clear_object(&app->about);
	}
	gtk_window_destroy(app->dialog.window);
	g_clear_object(&app->dialog.window);
}

static gboolean close_window(GtkWindow *window, gpointer data)
{
	struct monitor_app *app = data;

	if (app->settings.keep_running)
		gtk_widget_set_visible(GTK_WIDGET(window), FALSE);
	else
		g_application_quit(G_APPLICATION(app->application));
	return TRUE;
}

static GtkLabel *label(GtkBox *box, const char *text, const char *css)
{
	GtkWidget *widget = gtk_label_new(text);

	gtk_label_set_xalign(GTK_LABEL(widget), 0);
	gtk_label_set_wrap(GTK_LABEL(widget), TRUE);
	if (css)
		gtk_widget_add_css_class(widget, css);
	gtk_box_append(box, widget);
	return GTK_LABEL(widget);
}

static GtkCheckButton *check(GtkBox *box, const char *text, gboolean active)
{
	GtkCheckButton *button = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(text));

	gtk_check_button_set_active(button, active);
	gtk_box_append(box, GTK_WIDGET(button));
	return button;
}

static void create_settings_window(struct monitor_app *app)
{
	struct settings_window *dialog = &app->dialog;
	GtkBox *layout = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
	GtkBox *buttons = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12));
	GtkWidget *switcher = gtk_stack_switcher_new();
	GtkWidget *button;
	GtkEventController *keys = gtk_event_controller_key_new();
	const char *names[] = { "printers", "notifications", "scan" };
	const char *titles[] = { N_("Printers"), N_("Notifications"), N_("Scan to this PC") };
	unsigned int i;

	dialog->app = app;
	dialog->window = GTK_WINDOW(g_object_ref(gtk_window_new()));
	gtk_window_set_application(dialog->window, app->application);
	gtk_window_set_title(dialog->window, _("Settings"));
	gtk_window_set_transient_for(dialog->window, app->window);
	gtk_window_set_destroy_with_parent(dialog->window, TRUE);
	gtk_window_set_modal(dialog->window, TRUE);
	gtk_window_set_default_size(dialog->window, 620, -1);
	gtk_window_set_child(dialog->window, GTK_WIDGET(layout));
	dialog->pages = GTK_STACK(gtk_stack_new());
	gtk_stack_set_vhomogeneous(dialog->pages, FALSE);
	gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(switcher), dialog->pages);
	gtk_widget_set_halign(switcher, GTK_ALIGN_CENTER);
	gtk_widget_set_margin_top(switcher, 12);
	gtk_widget_set_margin_bottom(switcher, 8);
	gtk_box_append(layout, switcher);
	gtk_box_append(layout, GTK_WIDGET(dialog->pages));
	gtk_widget_set_vexpand(GTK_WIDGET(dialog->pages), TRUE);
	for (i = 0; i < SETTINGS_COUNT; i++) {
		GtkWidget *scroll = gtk_scrolled_window_new();

		gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
		gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
		gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 600);
		dialog->content[i] = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 8));
		gtk_widget_set_margin_start(GTK_WIDGET(dialog->content[i]), 24);
		gtk_widget_set_margin_end(GTK_WIDGET(dialog->content[i]), 24);
		gtk_widget_set_margin_top(GTK_WIDGET(dialog->content[i]), 12);
		gtk_widget_set_margin_bottom(GTK_WIDGET(dialog->content[i]), 20);
		gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(dialog->content[i]));
		gtk_stack_add_titled(dialog->pages, scroll, names[i], _(titles[i]));
	}
	dialog->error = label(layout, "", "error");
	gtk_widget_set_margin_start(GTK_WIDGET(dialog->error), 24);
	gtk_widget_set_margin_end(GTK_WIDGET(dialog->error), 24);
	gtk_label_set_selectable(dialog->error, TRUE);
	gtk_widget_set_visible(GTK_WIDGET(dialog->error), FALSE);
	gtk_widget_set_halign(GTK_WIDGET(buttons), GTK_ALIGN_END);
	gtk_widget_set_margin_start(GTK_WIDGET(buttons), 24);
	gtk_widget_set_margin_end(GTK_WIDGET(buttons), 24);
	gtk_widget_set_margin_top(GTK_WIDGET(buttons), 12);
	gtk_widget_set_margin_bottom(GTK_WIDGET(buttons), 12);
	button = gtk_button_new_with_label(_("Cancel"));
	g_signal_connect(button, "clicked", G_CALLBACK(cancel_settings), dialog);
	gtk_box_append(buttons, button);
	button = gtk_button_new_with_label(_("Confirm"));
	gtk_widget_add_css_class(button, "suggested-action");
	g_signal_connect(button, "clicked", G_CALLBACK(confirm_settings), dialog);
	gtk_box_append(buttons, button);
	gtk_window_set_default_widget(dialog->window, button);
	gtk_box_append(layout, GTK_WIDGET(buttons));
	g_signal_connect(dialog->window, "close-request", G_CALLBACK(close_settings), dialog);
	g_signal_connect(keys, "key-pressed", G_CALLBACK(settings_key_pressed), dialog);
	gtk_widget_add_controller(GTK_WIDGET(dialog->window), keys);
	g_signal_connect(dialog->pages, "notify::visible-child", G_CALLBACK(settings_page_changed), dialog);
}

static void create_main_menu(struct monitor_app *app)
{
	GtkWidget *header = gtk_header_bar_new();
	g_autoptr(GMenu) menu = g_menu_new();
	g_autoptr(GMenu) navigation = g_menu_new();
	g_autoptr(GMenu) closing = g_menu_new();

	g_menu_append(navigation, _("Maintenance"), "app.maintenance");
	g_menu_append(navigation, _("Settings"), "app.settings");
	g_menu_append(navigation, _("About"), "app.about");
	g_menu_append(closing, _("Close This Window"), "app.close");
	g_menu_append(closing, _("Quit"), "app.quit");
	g_menu_append_section(menu, NULL, G_MENU_MODEL(navigation));
	g_menu_append_section(menu, NULL, G_MENU_MODEL(closing));
	app->menu = GTK_MENU_BUTTON(gtk_menu_button_new());
	gtk_menu_button_set_icon_name(app->menu, "open-menu-symbolic");
	gtk_menu_button_set_menu_model(app->menu, G_MENU_MODEL(menu));
	gtk_widget_set_tooltip_text(GTK_WIDGET(app->menu), _("Main menu"));
	gtk_accessible_update_property(GTK_ACCESSIBLE(app->menu),
		GTK_ACCESSIBLE_PROPERTY_LABEL, _("Main menu"), -1);
	gtk_header_bar_pack_end(GTK_HEADER_BAR(header), GTK_WIDGET(app->menu));
	gtk_window_set_titlebar(app->window, header);
}

static void create_window(struct monitor_app *app)
{
	GtkWidget *scroll = gtk_scrolled_window_new();
	GtkBox *layout = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
	GtkBox *box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 12));
	GtkBox *settings;
	GtkWidget *row;
	GtkLabel *levels_status;
	GtkCssProvider *css = gtk_css_provider_new();
	const char *colors[] = { "black-ink", "cyan-ink", "magenta-ink", "yellow-ink" };
	const char *names[] = { N_("Black"), N_("Cyan"), N_("Magenta"), N_("Yellow") };
	unsigned int i;

	app->window = GTK_WINDOW(gtk_application_window_new(app->application));
	gtk_window_set_title(app->window, _("Brother MFC-J5720DW — Status monitor"));
	gtk_window_set_default_size(app->window, 620, -1);
	create_main_menu(app);
	gtk_window_set_child(app->window, GTK_WIDGET(layout));
	gtk_box_append(layout, scroll);
	gtk_widget_set_vexpand(scroll, TRUE);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(box));
	gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
	gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), 640);
	gtk_widget_set_margin_start(GTK_WIDGET(box), 24);
	gtk_widget_set_margin_end(GTK_WIDGET(box), 24);
	gtk_widget_set_margin_top(GTK_WIDGET(box), 20);
	gtk_widget_set_margin_bottom(GTK_WIDGET(box), 20);
	app->identity = label(box, _("No printer selected"), "dim-label");
	gtk_label_set_selectable(app->identity, TRUE);
	label(box, _("Printer status"), "title-2");
	app->status = label(box, _("Checking…"), "title-3");
	app->details = label(box, "", NULL);
	gtk_label_set_selectable(app->details, TRUE);
	app->updated = label(box, "", "dim-label");
	label(box, _("Ink levels"), "title-3");
	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
	gtk_box_set_homogeneous(GTK_BOX(row), TRUE);
	gtk_box_append(box, row);
	for (i = 0; i < 4; i++) {
		GtkBox *column = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 8));
		GtkLabel *name;

		gtk_box_append(GTK_BOX(row), GTK_WIDGET(column));
		name = label(column, _(names[i]), NULL);
		gtk_label_set_xalign(name, 0.5);
		app->ink[i] = GTK_PROGRESS_BAR(gtk_progress_bar_new());
		gtk_orientable_set_orientation(GTK_ORIENTABLE(app->ink[i]), GTK_ORIENTATION_VERTICAL);
		gtk_progress_bar_set_inverted(app->ink[i], TRUE);
		gtk_widget_set_halign(GTK_WIDGET(app->ink[i]), GTK_ALIGN_CENTER);
		gtk_widget_add_css_class(GTK_WIDGET(app->ink[i]), "ink-level");
		gtk_widget_add_css_class(GTK_WIDGET(app->ink[i]), colors[i]);
		gtk_box_append(column, GTK_WIDGET(app->ink[i]));
		app->ink_value[i] = label(column, "", NULL);
		gtk_label_set_xalign(app->ink_value[i], 0.5);
		gtk_accessible_update_relation(GTK_ACCESSIBLE(app->ink[i]),
			GTK_ACCESSIBLE_RELATION_LABELLED_BY, name, NULL,
			GTK_ACCESSIBLE_RELATION_DESCRIBED_BY, app->ink_value[i], NULL, -1);
	}
	levels_status = label(box, "", "dim-label");
	gtk_label_set_selectable(levels_status, TRUE);
	gtk_widget_set_visible(GTK_WIDGET(levels_status), FALSE);
	gtk_css_provider_load_from_string(css,
		".ink-level trough { min-width: 36px; min-height: 150px; }"
		".ink-level progress { min-width: 36px; min-height: 0; }"
		".black-ink progress { background: #333333; }"
		".cyan-ink progress { background: #00a4cc; }"
		".magenta-ink progress { background: #c52a87; }"
		".yellow-ink progress { background: #e5bb00; }");
	gtk_style_context_add_provider_for_display(gtk_widget_get_display(GTK_WIDGET(box)),
		GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_object_unref(css);
	create_settings_window(app);
	app->scan_panel = scan_panel_new(app->dialog.content[SETTINGS_SCAN], app->application,
					 &app->settings.scan, &app->settings.notify_enabled);
	settings = app->dialog.content[SETTINGS_PRINTER];
	label(settings, _("Print queue"), NULL);
	app->queues = GTK_DROP_DOWN(gtk_drop_down_new(NULL, NULL));
	gtk_box_append(settings, GTK_WIDGET(app->queues));
	label(settings, _("Printer address (optional for a network queue)"), NULL);
	app->host = GTK_ENTRY(gtk_entry_new());
	gtk_entry_set_max_length(app->host, sizeof(app->settings.target.host) - 1);
	gtk_entry_set_placeholder_text(app->host, _("Automatic, or an IP address / host name"));
	gtk_editable_set_text(GTK_EDITABLE(app->host), app->settings.target.host);
	gtk_box_append(settings, GTK_WIDGET(app->host));
	label(settings, _("SNMP read community"), NULL);
	app->community = GTK_ENTRY(gtk_entry_new());
	gtk_entry_set_max_length(app->community, sizeof(app->settings.target.community) - 1);
	gtk_editable_set_text(GTK_EDITABLE(app->community), app->settings.target.community);
	gtk_box_append(settings, GTK_WIDGET(app->community));
	settings = app->dialog.content[SETTINGS_NOTIFICATIONS];
	app->notifications = check(settings, _("Notify about errors and job delivery"), app->settings.notify_enabled);
	app->background = check(settings, _("Keep monitoring when the window is closed"), app->settings.keep_running);
	app->autostart = check(settings, _("Start monitoring when I log in"), app->settings.start_at_login);
	app->maintenance_panel = maintenance_panel_new(app->window, levels_status);
	g_signal_connect(app->window, "close-request", G_CALLBACK(close_window), app);
	g_signal_connect(app->window, "notify::visible", G_CALLBACK(settings_parent_visibility), app);
}

static void show_action(GSimpleAction *action, GVariant *parameter, gpointer data)
{
	struct monitor_app *app = data;

	(void)action; (void)parameter;
	gtk_window_present(app->window);
}

static void quit_action(GSimpleAction *action, GVariant *parameter, gpointer data)
{
	struct monitor_app *app = data;

	(void)action; (void)parameter;
	g_application_quit(G_APPLICATION(app->application));
}

static void maintenance_action(GSimpleAction *action, GVariant *parameter, gpointer data)
{
	struct monitor_app *app = data;

	(void)action; (void)parameter;
	gtk_window_present(app->window);
	maintenance_panel_present(app->maintenance_panel);
}

static void settings_action(GSimpleAction *action, GVariant *parameter, gpointer data)
{
	struct monitor_app *app = data;

	(void)action; (void)parameter;
	gtk_window_present(app->window);
	open_settings(NULL, &app->dialog);
}

static void close_action(GSimpleAction *action, GVariant *parameter, gpointer data)
{
	struct monitor_app *app = data;

	(void)action; (void)parameter;
	gtk_window_close(app->window);
}

static void about_action(GSimpleAction *action, GVariant *parameter, gpointer data)
{
	struct monitor_app *app = data;
	const char *authors[] = { "Laurent Vivier", NULL };
	const char *tools[] = { "OpenAI Codex", NULL };

	(void)action; (void)parameter;
	if (!app->about) {
		GtkAboutDialog *about = GTK_ABOUT_DIALOG(gtk_about_dialog_new());

		app->about = GTK_WINDOW(g_object_ref(about));
		gtk_window_set_application(app->about, app->application);
		gtk_window_set_transient_for(app->about, app->window);
		gtk_window_set_destroy_with_parent(app->about, TRUE);
		gtk_window_set_hide_on_close(app->about, TRUE);
		gtk_window_set_modal(app->about, TRUE);
		gtk_about_dialog_set_program_name(about, _("Brother MFC-J5720DW — Status monitor"));
		gtk_about_dialog_set_version(about, MONITOR_VERSION);
		gtk_about_dialog_set_logo_icon_name(about, "printer");
		gtk_about_dialog_set_comments(about,
			_("Printer status, ink levels, maintenance and scan-to-PC for the Brother MFC-J5720DW."));
		gtk_about_dialog_set_copyright(about, "Copyright © 2026 Laurent Vivier");
		gtk_about_dialog_set_authors(about, authors);
		gtk_about_dialog_add_credit_section(about, _("Development tools"), tools);
		gtk_about_dialog_set_license_type(about, GTK_LICENSE_GPL_3_0);
		gtk_about_dialog_set_website(about, "https://github.com/vivier/mfcj5720dw_cups");
		gtk_about_dialog_set_website_label(about, _("Project website"));
	}
	gtk_window_present(app->about);
}

static void select_action(GSimpleAction *action, GVariant *parameter, gpointer data)
{
	struct monitor_app *app = data;
	const char *queue, *host, *community;

	(void)action;
	g_variant_get(parameter, "(&s&s&s)", &queue, &host, &community);
	if (!valid_host(host) || strlen(queue) >= sizeof(app->settings.target.queue) ||
	    strlen(community) >= sizeof(app->settings.target.community))
		return;
	g_strlcpy(app->settings.target.queue, queue, sizeof(app->settings.target.queue));
	g_strlcpy(app->settings.target.host, host, sizeof(app->settings.target.host));
	g_strlcpy(app->settings.target.community, community, sizeof(app->settings.target.community));
	gtk_editable_set_text(GTK_EDITABLE(app->host), host);
	gtk_editable_set_text(GTK_EDITABLE(app->community), community);
	g_clear_pointer(&app->queue_names, g_strfreev);
	g_clear_pointer(&app->previous, g_free);
	app->revision++;
	app->failures = 0;
	app->problem_reported = FALSE;
	maintenance_panel_follow(app->maintenance_panel, app->settings.target.queue, 0, FALSE);
	refresh(app);
}

static void setup_actions(struct monitor_app *app)
{
	const GActionEntry actions[] = {
		{ .name = "show", .activate = show_action },
		{ .name = "quit", .activate = quit_action },
		{ .name = "maintenance", .activate = maintenance_action },
		{ .name = "settings", .activate = settings_action },
		{ .name = "about", .activate = about_action },
		{ .name = "close", .activate = close_action },
		{ .name = "select", .activate = select_action, .parameter_type = "(sss)" },
	};

	g_action_map_add_action_entries(G_ACTION_MAP(app->application), actions, G_N_ELEMENTS(actions), app);
}

static void startup(GApplication *application, gpointer data)
{
	struct monitor_app *app = data;
	g_autoptr(GError) error = NULL;

	(void)application;
	setup_actions(app);
	create_window(app);
	app->tray = monitor_tray_new(app->application, app->window, &error);
	if (!app->tray)
		g_warning("Cannot create tray icon: %s", error->message);
	g_application_hold(application);
	g_timeout_add_seconds(5, refresh, app);
	refresh(app);
}

static void activate(GApplication *application, gpointer data)
{
	struct monitor_app *app = data;

	(void)application;
	if (!app->start_hidden)
		gtk_window_present(app->window);
	app->start_hidden = FALSE;
}

int main(int argc, char **argv)
{
	struct monitor_app app = { 0 };
	g_autofree char *queue = NULL, *host = NULL, *community = NULL;
	gboolean once = FALSE;
	g_autoptr(GOptionContext) context = NULL;
	g_autoptr(GError) error = NULL;
	GOptionEntry options[] = {
		{ "queue", 'p', 0, G_OPTION_ARG_STRING, &queue, N_("Local CUPS queue"), "QUEUE" },
		{ "host", 'H', 0, G_OPTION_ARG_STRING, &host, N_("Printer IP address or host name"), "HOST" },
		{ "community", 0, 0, G_OPTION_ARG_STRING, &community, N_("SNMP read community (default: public)"), "COMMUNITY" },
		{ "background", 0, 0, G_OPTION_ARG_NONE, &app.start_hidden, N_("Start without showing the window"), NULL },
		{ "once", 0, 0, G_OPTION_ARG_NONE, &once, N_("Print one status report without a graphical display"), NULL },
		{ NULL }
	};
	int result;

	setlocale(LC_ALL, "");
	bindtextdomain(GETTEXT_DOMAIN, LOCALEDIR);
	bind_textdomain_codeset(GETTEXT_DOMAIN, "UTF-8");
	textdomain(GETTEXT_DOMAIN);
	load_settings(&app);
	context = g_option_context_new(_("— Brother MFC-J5720DW status monitor"));
	g_option_context_add_main_entries(context, options, GETTEXT_DOMAIN);
	if (!g_option_context_parse(context, &argc, &argv, &error)) {
		g_printerr("%s\n", error->message);
		return 2;
	}
	if (argc != 1 || (host && !valid_host(host)) ||
	    (queue && strlen(queue) >= sizeof(app.settings.target.queue)) ||
	    (community && strlen(community) >= sizeof(app.settings.target.community))) {
		g_printerr("%s\n", _("Invalid arguments; use --help."));
		return 2;
	}
	if (queue)
		g_strlcpy(app.settings.target.queue, queue, sizeof(app.settings.target.queue));
	if (host)
		g_strlcpy(app.settings.target.host, host, sizeof(app.settings.target.host));
	if (community)
		g_strlcpy(app.settings.target.community, community, sizeof(app.settings.target.community));
	monitor_network_init();
	if (once) {
		struct monitor_snapshot *s = g_new0(struct monitor_snapshot, 1);
		unsigned int i;

		monitor_poll(&app.settings.target, s);
		g_print("%s\n%s\n%s\n", s->target.host, s->summary, s->details);
		g_print("Brother: %d; HR: %d; errors: 0x%08x\n", s->brother_status, s->printer_status, s->errors);
		if (s->ink_valid)
			g_print("K=%.2f%% C=%.2f%% M=%.2f%% Y=%.2f%%\n", s->ink[0] / 100.0,
				s->ink[1] / 100.0, s->ink[2] / 100.0, s->ink[3] / 100.0);
		g_print("CUPS: %s (%d) %s\n", s->target.queue, s->queue_state, s->queue_message);
		for (i = 0; i < s->job_count; i++)
			g_print("#%d %s — %s\n", s->jobs[i].id, s->jobs[i].title, monitor_job_state(s->jobs[i].state));
		result = s->online ? 0 : 1;
		g_free(s);
		return result;
	}
	app.application = gtk_application_new(APP_ID, G_APPLICATION_DEFAULT_FLAGS);
	g_signal_connect(app.application, "startup", G_CALLBACK(startup), &app);
	g_signal_connect(app.application, "activate", G_CALLBACK(activate), &app);
	if (!g_application_register(G_APPLICATION(app.application), NULL, &error)) {
		g_printerr("%s\n", error->message);
		g_object_unref(app.application);
		return 1;
	}
	if (g_application_get_is_remote(G_APPLICATION(app.application))) {
		/* Forward an explicit target to the existing background instance. */
		if (queue || host || community)
			g_action_group_activate_action(G_ACTION_GROUP(app.application), "select",
				g_variant_new("(sss)", app.settings.target.queue, app.settings.target.host, app.settings.target.community));
	}
	result = g_application_run(G_APPLICATION(app.application), argc, argv);
	monitor_tray_free(app.tray);
	maintenance_panel_free(app.maintenance_panel);
	app.maintenance_panel = NULL;
	scan_panel_free(app.scan_panel);
	if (app.window)
		destroy_settings_windows(&app);
	g_object_unref(app.application);
	g_strfreev(app.queue_names);
	g_free(app.previous);
	return result;
}
