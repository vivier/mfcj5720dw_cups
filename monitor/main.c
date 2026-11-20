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
	GtkProgressBar *ink[4];
	GtkLabel *ink_value[4];
	GtkCheckButton *notifications;
	GtkCheckButton *background;
	GtkCheckButton *autostart;
	struct scan_panel *scan_panel;
	struct maintenance_panel *maintenance_panel;
	struct scan_settings scan_settings;
	struct monitor_target target;
	struct monitor_snapshot *previous;
	GStrv queue_names;
	unsigned int revision;
	unsigned int failures;
	gboolean polling;
	gboolean start_hidden;
	gboolean notify_enabled;
	gboolean keep_running;
	gboolean start_at_login;
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

	g_strlcpy(app->target.community, "public", sizeof(app->target.community));
	app->notify_enabled = TRUE;
	app->keep_running = TRUE;
	scan_settings_defaults(&app->scan_settings);
	if (!g_key_file_load_from_file(settings, path, G_KEY_FILE_NONE, NULL))
		return;
	scan_settings_load(&app->scan_settings, settings);
	queue = g_key_file_get_string(settings, "Monitor", "queue", NULL);
	host = g_key_file_get_string(settings, "Monitor", "host", NULL);
	community = g_key_file_get_string(settings, "Monitor", "community", NULL);
	g_strlcpy(app->target.queue, queue ? queue : "", sizeof(app->target.queue));
	g_strlcpy(app->target.host, host ? host : "", sizeof(app->target.host));
	if (community && *community)
		g_strlcpy(app->target.community, community, sizeof(app->target.community));
	app->notify_enabled = g_key_file_get_boolean(settings, "Monitor", "notifications", NULL);
	app->keep_running = g_key_file_get_boolean(settings, "Monitor", "background", NULL);
	app->start_at_login = g_key_file_get_boolean(settings, "Monitor", "autostart", NULL);
}

static gboolean save_settings(struct monitor_app *app, GError **error)
{
	g_autoptr(GKeyFile) settings = g_key_file_new();
	g_autofree char *path = settings_path();
	g_autofree char *directory = g_path_get_dirname(path);
	g_autofree char *text = NULL;

	g_key_file_set_string(settings, "Monitor", "queue", app->target.queue);
	g_key_file_set_string(settings, "Monitor", "host", app->target.host);
	g_key_file_set_string(settings, "Monitor", "community", app->target.community);
	g_key_file_set_boolean(settings, "Monitor", "notifications", app->notify_enabled);
	g_key_file_set_boolean(settings, "Monitor", "background", app->keep_running);
	g_key_file_set_boolean(settings, "Monitor", "autostart", app->start_at_login);
	scan_settings_save(&app->scan_settings, settings);
	text = g_key_file_to_data(settings, NULL, NULL);
	if (g_mkdir_with_parents(directory, 0700) < 0) {
		g_set_error_literal(error, G_FILE_ERROR, g_file_error_from_errno(errno), g_strerror(errno));
		return FALSE;
	}
	return g_file_set_contents_full(path, text, -1,
				       G_FILE_SET_CONTENTS_CONSISTENT, 0600, error);
}

static gboolean save_autostart(struct monitor_app *app, GError **error)
{
	g_autofree char *directory = g_build_filename(g_get_user_config_dir(), "autostart", NULL);
	g_autofree char *path = g_build_filename(directory, APP_ID ".desktop", NULL);
	g_autofree char *text = g_strdup_printf(
		"[Desktop Entry]\nType=Application\nName=Brother MFC-J5720DW Monitor\n"
		"Exec=mfcj5720dw-monitor --background\nIcon=printer\nHidden=%s\n",
		app->start_at_login ? "false" : "true");

	if (!app->start_at_login && !g_file_test(path, G_FILE_TEST_EXISTS))
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

	if (!app->notify_enabled)
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
		app->problem_reported = app->notify_enabled;
	} else if (s->online && (!old || monitor_issue_changed(old, s))) {
		if (s->severity >= MONITOR_WARNING) {
			notify(app, "printer", s->summary, body, s->severity == MONITOR_ERROR);
			app->problem_reported = app->notify_enabled;
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
	const char *selected = app->target.queue;
	unsigned int selection = 0, i;

	/* Do not replace a pending user selection during the next poll. */
	if (app->queue_names) {
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
	g_autofree char *updated = g_strdup_printf(_("Last update: %s — %s"), time,
						 s->target.host[0] ? s->target.host : _("No address"));
	unsigned int i;

	update_queues(app, s);
	gtk_label_set_text(app->status, s->summary);
	gtk_label_set_text(app->details, s->details);
	gtk_label_set_text(app->updated, updated);
	for (i = 0; i < 4; i++) {
		char text[32];

		if (s->ink_valid)
			g_snprintf(text, sizeof(text), "%.1f %%", s->ink[i] / 100.0);
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
	if (!app->initialized && !app->target.queue[0] && !app->target.host[0])
		g_strlcpy(app->target.queue, snapshot->target.queue, sizeof(app->target.queue));
	app->initialized = TRUE;
	send_changes(app, snapshot);
	show_snapshot(app, snapshot);
	maintenance_panel_follow(app->maintenance_panel, snapshot->target.queue,
				 snapshot->maintenance_commands, snapshot->queue_accepting_jobs);
	maintenance_panel_update_ink(app->maintenance_panel, snapshot);
	scan_panel_follow(app->scan_panel, snapshot->target.host, &app->scan_settings);
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
	request->target = app->target;
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

static void apply_settings(GtkButton *button, gpointer data)
{
	struct monitor_app *app = data;
	unsigned int selected = gtk_drop_down_get_selected(app->queues);
	const char *host = gtk_editable_get_text(GTK_EDITABLE(app->host));
	const char *community = gtk_editable_get_text(GTK_EDITABLE(app->community));
	g_autoptr(GError) error = NULL;
	struct scan_settings scan_settings = app->scan_settings;

	(void)button;
	if (!valid_host(host)) {
		gtk_label_set_text(app->status, _("Enter a host name or IP address, without a URL or port."));
		return;
	}
	if (!scan_panel_read(app->scan_panel, &scan_settings, &error)) {
		gtk_label_set_text(app->status, error->message);
		return;
	}
	app->scan_settings = scan_settings;
	if (app->queue_names && selected < g_strv_length(app->queue_names))
		g_strlcpy(app->target.queue, app->queue_names[selected], sizeof(app->target.queue));
	g_strlcpy(app->target.host, host, sizeof(app->target.host));
	g_strlcpy(app->target.community, *community ? community : "public", sizeof(app->target.community));
	app->notify_enabled = gtk_check_button_get_active(app->notifications);
	app->keep_running = gtk_check_button_get_active(app->background);
	app->start_at_login = gtk_check_button_get_active(app->autostart);
	if (!save_autostart(app, &error) || !save_settings(app, &error))
		gtk_label_set_text(app->status, error->message);
	app->revision++;
	app->failures = 0;
	app->problem_reported = FALSE;
	g_clear_pointer(&app->previous, g_free);
	scan_panel_follow(app->scan_panel, app->target.host, &app->scan_settings);
	maintenance_panel_follow(app->maintenance_panel, app->target.queue, 0, FALSE);
	refresh(app);
}

static void quit_clicked(GtkButton *button, gpointer data)
{
	struct monitor_app *app = data;

	(void)button;
	g_application_quit(G_APPLICATION(app->application));
}

static gboolean close_window(GtkWindow *window, gpointer data)
{
	struct monitor_app *app = data;

	if (app->keep_running)
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

static void create_window(struct monitor_app *app)
{
	GtkWidget *scroll = gtk_scrolled_window_new();
	GtkBox *layout = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
	GtkBox *box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 12));
	GtkBox *settings = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 8));
	GtkWidget *expander = gtk_expander_new(_("Printer and notifications"));
	GtkWidget *button, *row;
	GtkButton *maintenance;
	GtkLabel *levels_status;
	GtkCssProvider *css = gtk_css_provider_new();
	const char *colors[] = { "black-ink", "cyan-ink", "magenta-ink", "yellow-ink" };
	const char *names[] = { N_("Black"), N_("Cyan"), N_("Magenta"), N_("Yellow") };
	unsigned int i;

	app->window = GTK_WINDOW(gtk_application_window_new(app->application));
	gtk_window_set_title(app->window, _("Brother MFC-J5720DW — Status monitor"));
	gtk_window_set_default_size(app->window, 620, -1);
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
	app->scan_panel = scan_panel_new(box, app->application, &app->scan_settings, &app->notify_enabled);
	gtk_box_append(box, expander);
	gtk_expander_set_child(GTK_EXPANDER(expander), GTK_WIDGET(settings));
	gtk_expander_set_expanded(GTK_EXPANDER(expander), !app->target.queue[0] && !app->target.host[0]);
	label(settings, _("Print queue"), NULL);
	app->queues = GTK_DROP_DOWN(gtk_drop_down_new(NULL, NULL));
	gtk_box_append(settings, GTK_WIDGET(app->queues));
	label(settings, _("Printer address (optional for a network queue)"), NULL);
	app->host = GTK_ENTRY(gtk_entry_new());
	gtk_entry_set_max_length(app->host, sizeof(app->target.host) - 1);
	gtk_entry_set_placeholder_text(app->host, _("Automatic, or an IP address / host name"));
	gtk_editable_set_text(GTK_EDITABLE(app->host), app->target.host);
	gtk_box_append(settings, GTK_WIDGET(app->host));
	label(settings, _("SNMP read community"), NULL);
	app->community = GTK_ENTRY(gtk_entry_new());
	gtk_entry_set_max_length(app->community, sizeof(app->target.community) - 1);
	gtk_editable_set_text(GTK_EDITABLE(app->community), app->target.community);
	gtk_box_append(settings, GTK_WIDGET(app->community));
	app->notifications = check(settings, _("Notify about errors and job delivery"), app->notify_enabled);
	app->background = check(settings, _("Keep monitoring when the window is closed"), app->keep_running);
	app->autostart = check(settings, _("Start monitoring when I log in"), app->start_at_login);
	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	button = gtk_button_new_with_label(_("Apply"));
	g_signal_connect(button, "clicked", G_CALLBACK(apply_settings), app);
	gtk_box_append(GTK_BOX(row), button);
	maintenance = GTK_BUTTON(gtk_button_new_with_label(_("Maintenance")));
	gtk_box_append(GTK_BOX(row), GTK_WIDGET(maintenance));
	app->maintenance_panel = maintenance_panel_new(app->window, maintenance, levels_status);
	button = gtk_button_new_with_label(_("Quit monitor"));
	g_signal_connect(button, "clicked", G_CALLBACK(quit_clicked), app);
	gtk_box_append(GTK_BOX(row), button);
	gtk_widget_set_margin_start(row, 24);
	gtk_widget_set_margin_end(row, 24);
	gtk_widget_set_margin_top(row, 12);
	gtk_widget_set_margin_bottom(row, 12);
	gtk_box_append(layout, row);
	g_signal_connect(app->window, "close-request", G_CALLBACK(close_window), app);
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

static void select_action(GSimpleAction *action, GVariant *parameter, gpointer data)
{
	struct monitor_app *app = data;
	const char *queue, *host, *community;

	(void)action;
	g_variant_get(parameter, "(&s&s&s)", &queue, &host, &community);
	if (!valid_host(host) || strlen(queue) >= sizeof(app->target.queue) ||
	    strlen(community) >= sizeof(app->target.community))
		return;
	g_strlcpy(app->target.queue, queue, sizeof(app->target.queue));
	g_strlcpy(app->target.host, host, sizeof(app->target.host));
	g_strlcpy(app->target.community, community, sizeof(app->target.community));
	gtk_editable_set_text(GTK_EDITABLE(app->host), host);
	gtk_editable_set_text(GTK_EDITABLE(app->community), community);
	g_clear_pointer(&app->queue_names, g_strfreev);
	g_clear_pointer(&app->previous, g_free);
	app->revision++;
	app->failures = 0;
	app->problem_reported = FALSE;
	maintenance_panel_follow(app->maintenance_panel, app->target.queue, 0, FALSE);
	refresh(app);
}

static void startup(GApplication *application, gpointer data)
{
	struct monitor_app *app = data;
	g_autoptr(GError) error = NULL;
	const GActionEntry actions[] = {
		{ .name = "show", .activate = show_action },
		{ .name = "quit", .activate = quit_action },
		{ .name = "select", .activate = select_action, .parameter_type = "(sss)" },
	};

	create_window(app);
	g_action_map_add_action_entries(G_ACTION_MAP(application), actions, G_N_ELEMENTS(actions), app);
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
	    (queue && strlen(queue) >= sizeof(app.target.queue)) ||
	    (community && strlen(community) >= sizeof(app.target.community))) {
		g_printerr("%s\n", _("Invalid arguments; use --help."));
		return 2;
	}
	if (queue)
		g_strlcpy(app.target.queue, queue, sizeof(app.target.queue));
	if (host)
		g_strlcpy(app.target.host, host, sizeof(app.target.host));
	if (community)
		g_strlcpy(app.target.community, community, sizeof(app.target.community));
	monitor_network_init();
	if (once) {
		struct monitor_snapshot *s = g_new0(struct monitor_snapshot, 1);
		unsigned int i;

		monitor_poll(&app.target, s);
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
				g_variant_new("(sss)", app.target.queue, app.target.host, app.target.community));
	}
	result = g_application_run(G_APPLICATION(app.application), argc, argv);
	monitor_tray_free(app.tray);
	maintenance_panel_free(app.maintenance_panel);
	app.maintenance_panel = NULL;
	scan_panel_free(app.scan_panel);
	g_object_unref(app.application);
	g_strfreev(app.queue_names);
	g_free(app.previous);
	return result;
}
