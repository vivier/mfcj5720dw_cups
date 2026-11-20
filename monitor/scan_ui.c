/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Laurent Vivier */
#include "scan_ui.h"

#include <glib/gi18n.h>
#include <string.h>

struct scan_panel {
	GtkApplication *application;
	const gboolean *notify_enabled;
	GtkCheckButton *enabled;
	GtkCheckButton *grayscale;
	GtkEntry *name;
	GtkButton *directory;
	GCancellable *directory_cancel;
	GtkEntry *languages;
	GtkEntry *address;
	GtkDropDown *resolution;
	GtkLabel *status;
	GtkButton *cancel;
	GtkButton *open;
	struct scan_service *service;
	struct scan_settings settings;
	char *printer;
	char *last_directory;
	char *last_error;
};

/* The panel may close before the desktop mail composer returns. */
struct email_completion {
	GApplication *application;
	gboolean notify_enabled;
};

/* Keep widgets alive until the asynchronous chooser finishes. */
struct folder_selection {
	GtkButton *button;
	GtkLabel *status;
	GCancellable *cancel;
};

static GtkLabel *scan_label(GtkBox *box, const char *text)
{
	GtkLabel *label = GTK_LABEL(gtk_label_new(text));

	gtk_label_set_xalign(label, 0);
	gtk_label_set_wrap(label, TRUE);
	gtk_box_append(box, GTK_WIDGET(label));
	return label;
}

static GtkEntry *scan_entry(GtkBox *box, const char *title, const char *value, int length)
{
	GtkEntry *entry = GTK_ENTRY(gtk_entry_new());

	scan_label(box, title);
	gtk_entry_set_max_length(entry, length);
	gtk_editable_set_text(GTK_EDITABLE(entry), value);
	gtk_box_append(box, GTK_WIDGET(entry));
	return entry;
}

static void scan_notice(struct scan_panel *panel, const char *title, const char *body,
			gboolean urgent)
{
	g_autoptr(GNotification) notice = g_notification_new(title);
	g_autoptr(GIcon) icon = g_themed_icon_new("scanner");

	if (!*panel->notify_enabled)
		return;
	g_notification_set_body(notice, body);
	g_notification_set_icon(notice, icon);
	g_notification_set_default_action(notice, "app.show");
	g_notification_set_priority(notice, urgent ? G_NOTIFICATION_PRIORITY_HIGH : G_NOTIFICATION_PRIORITY_NORMAL);
	g_application_send_notification(G_APPLICATION(panel->application), "scan", notice);
}

static void open_path(struct scan_panel *panel, const char *path)
{
	g_autoptr(GError) error = NULL;
	g_autofree char *uri = g_filename_to_uri(path, NULL, &error);

	if (uri && g_app_info_launch_default_for_uri(uri, NULL, &error))
		return;
	gtk_label_set_text(panel->status, error->message);
	scan_notice(panel, _("Cannot open the scanned document"), error->message, TRUE);
}

static void compose_done(GObject *object, GAsyncResult *async, gpointer data)
{
	struct email_completion *completion = data;
	g_autoptr(GError) error = NULL;

	if (!g_subprocess_wait_check_finish(G_SUBPROCESS(object), async, &error) &&
	    completion->notify_enabled) {
		g_autoptr(GNotification) notice = g_notification_new(_("Cannot open the email composer"));

		g_notification_set_body(notice, _("The PDF is saved. Configure a default email application and attach it manually."));
		g_notification_set_default_action(notice, "app.show");
		g_application_send_notification(completion->application, "scan-email", notice);
	}
	g_object_unref(completion->application);
	g_free(completion);
}

static void compose_email(struct scan_panel *panel, const char *document)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GSubprocess) child = g_subprocess_new(G_SUBPROCESS_FLAGS_NONE, &error,
		"xdg-email", "--subject", _("Scanned document"), "--attach", document, NULL);

	/* Open a draft only. No recipient is supplied and no email is sent. */
	if (child) {
		struct email_completion *completion = g_new0(struct email_completion, 1);

		completion->application = g_object_ref(G_APPLICATION(panel->application));
		completion->notify_enabled = *panel->notify_enabled;
		g_subprocess_wait_check_async(child, NULL, compose_done, completion);
	} else {
		gtk_label_set_text(panel->status, error->message);
		scan_notice(panel, _("Cannot open the email composer"), error->message, TRUE);
	}
}

static void scan_changed(enum scan_activity activity, const char *message,
			 enum scan_destination destination, const struct scan_result *result,
			 gpointer data)
{
	struct scan_panel *panel = data;

	gtk_label_set_text(panel->status, message);
	gtk_widget_set_sensitive(GTK_WIDGET(panel->cancel), activity == SCAN_RECEIVING);
	if (result && result->directory) {
		g_free(panel->last_directory);
		panel->last_directory = g_strdup(result->directory);
		gtk_widget_set_sensitive(GTK_WIDGET(panel->open), TRUE);
	}
	if (activity == SCAN_FAILED) {
		if (g_strcmp0(panel->last_error, message))
			scan_notice(panel, _("Scanning is unavailable"), message, TRUE);
		g_free(panel->last_error);
		panel->last_error = g_strdup(message);
		return;
	}
	g_clear_pointer(&panel->last_error, g_free);
	if (activity == SCAN_RECEIVING)
		scan_notice(panel, _("Scanning started"), scan_destination_name(destination), FALSE);
	if (activity != SCAN_FINISHED)
		return;
	scan_notice(panel, _("Scanning finished"), message, FALSE);
	if (destination == SCAN_IMAGE || destination == SCAN_OCR)
		open_path(panel, result->document);
	else if (destination == SCAN_EMAIL)
		compose_email(panel, result->document);
}

static void cancel_scan(GtkButton *button, gpointer data)
{
	struct scan_panel *panel = data;

	(void)button;
	scan_service_cancel(panel->service);
}

static void open_folder(GtkButton *button, gpointer data)
{
	struct scan_panel *panel = data;

	(void)button;
	if (panel->last_directory)
		open_path(panel, panel->last_directory);
}

static void set_scan_directory(GtkButton *button, const char *path)
{
	g_autofree char *display = g_filename_display_name(path);
	GtkLabel *label;

	/* Keep the filesystem path separate from its UTF-8 display string. */
	g_object_set_data_full(G_OBJECT(button), "scan-directory", g_strdup(path), g_free);
	gtk_button_set_label(button, display);
	label = GTK_LABEL(gtk_button_get_child(button));
	gtk_label_set_xalign(label, 0);
	gtk_label_set_ellipsize(label, PANGO_ELLIPSIZE_MIDDLE);
	gtk_label_set_max_width_chars(label, 40);
	gtk_widget_set_tooltip_text(GTK_WIDGET(button), display);
}

static void scan_folder_selected(GObject *object, GAsyncResult *result, gpointer data)
{
	struct folder_selection *selection = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(GFile) folder = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(object), result, &error);
	g_autofree char *path = NULL;

	/* The panel may have been freed while the dialog was open. */
	if (g_cancellable_is_cancelled(selection->cancel))
		goto out;
	gtk_widget_set_sensitive(GTK_WIDGET(selection->button), TRUE);
	if (!folder) {
		if (error && !g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED) &&
		    !g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_CANCELLED) &&
		    !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
			gtk_label_set_text(selection->status, error->message);
		goto out;
	}
	path = g_file_get_path(folder);
	if (!path || !g_path_is_absolute(path)) {
		gtk_label_set_text(selection->status, _("Choose a folder on this PC."));
		goto out;
	}
	if (strlen(path) >= sizeof(((struct scan_settings *)0)->directory)) {
		gtk_label_set_text(selection->status, _("The scan folder path is too long."));
		goto out;
	}
	set_scan_directory(selection->button, path);
out:
	g_object_unref(selection->button);
	g_object_unref(selection->status);
	g_object_unref(selection->cancel);
	g_free(selection);
}

static void choose_scan_folder(GtkButton *button, gpointer data)
{
	struct scan_panel *panel = data;
	g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new();
	const char *path = g_object_get_data(G_OBJECT(button), "scan-directory");
	g_autoptr(GFile) folder = g_file_new_for_path(path);
	struct folder_selection *selection = g_new0(struct folder_selection, 1);
	GtkWindow *parent = GTK_WINDOW(gtk_widget_get_root(GTK_WIDGET(button)));

	selection->button = g_object_ref(button);
	selection->status = g_object_ref(panel->status);
	selection->cancel = g_object_ref(panel->directory_cancel);
	gtk_file_dialog_set_title(dialog, _("Save scans in this folder"));
	gtk_file_dialog_set_modal(dialog, TRUE);
	gtk_file_dialog_set_initial_folder(dialog, folder);
	gtk_widget_set_sensitive(GTK_WIDGET(button), FALSE);
	gtk_file_dialog_select_folder(dialog, parent, selection->cancel, scan_folder_selected, selection);
}

struct scan_panel *scan_panel_new(GtkBox *parent, GtkApplication *application,
				 const struct scan_settings *settings,
				 const gboolean *notify_enabled)
{
	struct scan_panel *panel = g_new0(struct scan_panel, 1);
	GtkBox *box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 8));
	GtkBox *fields = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 8));
	GtkBox *row = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12));
	g_auto(GStrv) resolutions = g_new0(char *, scan_resolution_count + 1);
	guint resolution_index = GTK_INVALID_LIST_POSITION;
	GtkLabel *hint;
	size_t i;

	for (i = 0; i < scan_resolution_count; i++) {
		resolutions[i] = g_strdup_printf("%u dpi", scan_resolutions[i]);
		if (scan_resolutions[i] == settings->resolution)
			resolution_index = i;
	}

	panel->application = application;
	panel->notify_enabled = notify_enabled;
	gtk_box_append(parent, GTK_WIDGET(box));
	panel->enabled = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("Receive scans started on the printer")));
	gtk_check_button_set_active(panel->enabled, settings->enabled);
	gtk_box_append(box, GTK_WIDGET(panel->enabled));
	panel->status = scan_label(box, _("Disabled"));
	gtk_label_set_selectable(panel->status, TRUE);
	gtk_box_append(box, GTK_WIDGET(fields));
	g_object_bind_property(panel->enabled, "active", fields, "sensitive", G_BINDING_SYNC_CREATE);
	hint = scan_label(fields, _("Requires the Brother brscan4 64-bit scanner driver, configured for this printer's IP address."));
	gtk_widget_add_css_class(GTK_WIDGET(hint), "dim-label");
	panel->name = scan_entry(fields, _("PC name shown on the printer"), settings->name, 15);
	scan_label(fields, _("Save scans in this folder"));
	panel->directory = GTK_BUTTON(gtk_button_new());
	panel->directory_cancel = g_cancellable_new();
	set_scan_directory(panel->directory, settings->directory);
	g_signal_connect(panel->directory, "clicked", G_CALLBACK(choose_scan_folder), panel);
	gtk_box_append(fields, GTK_WIDGET(panel->directory));
	hint = scan_label(fields, _("The printer uses the document feeder when loaded, otherwise the scanner glass."));
	gtk_widget_add_css_class(GTK_WIDGET(hint), "dim-label");
	hint = scan_label(fields, _("Choose one-sided, long-edge or short-edge scanning on the printer. Two-sided scans always use the document feeder."));
	gtk_widget_add_css_class(GTK_WIDGET(hint), "dim-label");
	scan_label(fields, _("Scan resolution"));
	panel->resolution = GTK_DROP_DOWN(gtk_drop_down_new_from_strings((const char *const *)resolutions));
	gtk_drop_down_set_selected(panel->resolution, resolution_index);
	gtk_box_append(fields, GTK_WIDGET(panel->resolution));
	panel->grayscale = GTK_CHECK_BUTTON(gtk_check_button_new_with_label(_("Scan in grayscale")));
	gtk_check_button_set_active(panel->grayscale, settings->grayscale);
	gtk_box_append(fields, GTK_WIDGET(panel->grayscale));
	panel->languages = scan_entry(fields, _("OCR languages (for example fra+eng)"), settings->languages, sizeof(settings->languages) - 1);
	panel->address = scan_entry(fields, _("PC address (optional; automatic on a local network)"), settings->address, 15);
	hint = scan_label(fields, _("File: PDF. Image: JPEG. OCR: searchable PDF. Email: open a draft with a PDF attachment."));
	gtk_widget_add_css_class(GTK_WIDGET(hint), "dim-label");
	hint = scan_label(fields, _("Keep the monitor running to receive scans. Allow UDP port 54925 from the printer in the PC firewall."));
	gtk_widget_add_css_class(GTK_WIDGET(hint), "dim-label");
	panel->cancel = GTK_BUTTON(gtk_button_new_with_label(_("Cancel scan")));
	gtk_widget_set_sensitive(GTK_WIDGET(panel->cancel), FALSE);
	g_signal_connect(panel->cancel, "clicked", G_CALLBACK(cancel_scan), panel);
	gtk_box_append(row, GTK_WIDGET(panel->cancel));
	panel->open = GTK_BUTTON(gtk_button_new_with_label(_("Open last scan folder")));
	gtk_widget_set_sensitive(GTK_WIDGET(panel->open), FALSE);
	g_signal_connect(panel->open, "clicked", G_CALLBACK(open_folder), panel);
	gtk_box_append(row, GTK_WIDGET(panel->open));
	gtk_box_append(box, GTK_WIDGET(row));
	return panel;
}

void scan_panel_reset(struct scan_panel *panel, const struct scan_settings *settings)
{
	guint resolution = GTK_INVALID_LIST_POSITION;
	size_t i;

	/* Discard a late folder-chooser reply along with the other draft values. */
	g_cancellable_cancel(panel->directory_cancel);
	g_clear_object(&panel->directory_cancel);
	panel->directory_cancel = g_cancellable_new();
	gtk_widget_set_sensitive(GTK_WIDGET(panel->directory), TRUE);
	gtk_check_button_set_active(panel->enabled, settings->enabled);
	gtk_check_button_set_active(panel->grayscale, settings->grayscale);
	gtk_editable_set_text(GTK_EDITABLE(panel->name), settings->name);
	gtk_editable_set_text(GTK_EDITABLE(panel->languages), settings->languages);
	gtk_editable_set_text(GTK_EDITABLE(panel->address), settings->address);
	set_scan_directory(panel->directory, settings->directory);
	for (i = 0; i < scan_resolution_count; i++) {
		if (scan_resolutions[i] == settings->resolution)
			resolution = i;
	}
	gtk_drop_down_set_selected(panel->resolution, resolution);
}

gboolean scan_panel_busy(struct scan_panel *panel)
{
	return scan_service_busy(panel->service);
}

gboolean scan_panel_read(struct scan_panel *panel, struct scan_settings *settings, GError **error)
{
	guint resolution_index = gtk_drop_down_get_selected(panel->resolution);
	const char *directory = g_object_get_data(G_OBJECT(panel->directory), "scan-directory");

	settings->enabled = gtk_check_button_get_active(panel->enabled);
	if (strlen(directory) >= sizeof(settings->directory)) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
			_("The scan folder path is too long."));
		return FALSE;
	}
	settings->grayscale = gtk_check_button_get_active(panel->grayscale);
	settings->resolution = resolution_index < scan_resolution_count ?
		scan_resolutions[resolution_index] : 0;
	g_strlcpy(settings->name, gtk_editable_get_text(GTK_EDITABLE(panel->name)), sizeof(settings->name));
	if (strspn(settings->name, " ") == strlen(settings->name)) {
		struct scan_settings defaults;

		scan_settings_defaults(&defaults);
		g_strlcpy(settings->name, defaults.name, sizeof(settings->name));
	}
	g_strlcpy(settings->directory, directory, sizeof(settings->directory));
	g_strlcpy(settings->address, gtk_editable_get_text(GTK_EDITABLE(panel->address)), sizeof(settings->address));
	g_strlcpy(settings->languages, gtk_editable_get_text(GTK_EDITABLE(panel->languages)), sizeof(settings->languages));
	if (settings->enabled && !scan_settings_valid(settings, error))
		return FALSE;
	gtk_editable_set_text(GTK_EDITABLE(panel->name), settings->name);
	return TRUE;
}

void scan_panel_follow(struct scan_panel *panel, const char *printer, const struct scan_settings *settings)
{
	g_autoptr(GError) error = NULL;

	if (!g_strcmp0(panel->printer, printer) && !memcmp(&panel->settings, settings, sizeof(*settings)))
		return;
	scan_service_free(panel->service);
	panel->service = NULL;
	g_free(panel->printer);
	panel->printer = g_strdup(printer);
	panel->settings = *settings;
	if (!settings->enabled) {
		gtk_label_set_text(panel->status, _("Disabled"));
		return;
	}
	if (!scan_settings_valid(settings, &error)) {
		gtk_label_set_text(panel->status, error->message);
		return;
	}
	if (!printer[0]) {
		gtk_label_set_text(panel->status, _("Select a network printer or enter its IP address."));
		return;
	}
	gtk_label_set_text(panel->status, _("Registering this PC with the printer…"));
	panel->service = scan_service_new(printer, settings, scan_changed, panel);
}

void scan_panel_free(struct scan_panel *panel)
{
	if (!panel)
		return;
	g_cancellable_cancel(panel->directory_cancel);
	g_clear_object(&panel->directory_cancel);
	scan_service_free(panel->service);
	g_free(panel->printer);
	g_free(panel->last_directory);
	g_free(panel->last_error);
	g_free(panel);
}
