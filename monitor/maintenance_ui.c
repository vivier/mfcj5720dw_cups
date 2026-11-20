/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Laurent Vivier */
#include "maintenance_ui.h"
#include "maintenance.h"
#include "status.h"

#include <glib/gi18n.h>
#include <string.h>

#define INK_UPDATE_INTERVAL (5 * 60 * G_USEC_PER_SEC)

struct maintenance_panel {
	GtkWindow *parent;
	GtkWindow *window;
	GtkBox *box;
	GtkLabel *queue_label;
	GtkLabel *status;
	GtkLabel *levels_status;
	GtkDropDown *strength;
	GtkDropDown *inks;
	GtkButton *clean;
	GtkButton *nozzles;
	GCancellable *dialog_cancel;
	char *queue;
	char *pending_queue;
	enum maintenance_operation pending_operation;
	enum maintenance_inks pending_inks;
	unsigned int commands;
	int ink_job_id;
	gint64 next_ink_update;
	gboolean accepting_jobs;
	gboolean busy;
	gboolean closing;
};

static GtkLabel *panel_label(GtkBox *box, const char *text)
{
	GtkLabel *label = GTK_LABEL(gtk_label_new(text));

	gtk_label_set_xalign(label, 0);
	gtk_label_set_wrap(label, TRUE);
	gtk_box_append(box, GTK_WIDGET(label));
	return label;
}

static gboolean available(struct maintenance_panel *panel, unsigned int operation)
{
	return panel->queue && *panel->queue && panel->accepting_jobs &&
		operation < MAINTENANCE_OPERATION_COUNT && (panel->commands & (1U << operation));
}

static void update_controls(struct maintenance_panel *panel)
{
	gboolean ready = !panel->busy && !panel->closing;
	gboolean cleaning = panel->accepting_jobs && (panel->commands &
		((1U << MAINTENANCE_CLEAN) | (1U << MAINTENANCE_CLEAN_STRONG) |
		 (1U << MAINTENANCE_CLEAN_STRONGEST)));

	gtk_widget_set_sensitive(GTK_WIDGET(panel->clean), ready &&
		available(panel, gtk_drop_down_get_selected(panel->strength)));
	gtk_widget_set_sensitive(GTK_WIDGET(panel->nozzles), ready &&
		available(panel, MAINTENANCE_NOZZLE_CHECK));
	gtk_widget_set_sensitive(GTK_WIDGET(panel->strength), ready && cleaning);
	gtk_widget_set_sensitive(GTK_WIDGET(panel->inks), ready && cleaning);
}

static void selection_changed(GObject *object, GParamSpec *property, gpointer data)
{
	(void)object; (void)property;
	update_controls(data);
}

static void submit_worker(GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
	struct maintenance_panel *panel = data;
	g_autoptr(GError) error = NULL;
	int job;

	(void)source; (void)cancel;
	/* Pending fields are immutable until this worker's callback completes. */
	job = maintenance_submit(panel->pending_queue, panel->pending_operation,
				 panel->pending_inks, &error);
	if (job)
		g_task_return_int(task, job);
	else
		g_task_return_error(task, g_steal_pointer(&error));
}

static void show_result(struct maintenance_panel *panel, const char *message)
{
	GtkLabel *label = panel->pending_operation == MAINTENANCE_INK_LEVELS ?
		panel->levels_status : panel->status;

	gtk_label_set_text(label, message);
	gtk_widget_set_visible(GTK_WIDGET(label), TRUE);
}

static void submit_done(GObject *source, GAsyncResult *result, gpointer data)
{
	struct maintenance_panel *panel = data;
	g_autoptr(GError) error = NULL;
	int job = g_task_propagate_int(G_TASK(result), &error);

	(void)source;
	panel->busy = FALSE;
	if (panel->closing)
		return;
	/* A result for a previous queue must not overwrite the current ink status. */
	if (panel->pending_operation == MAINTENANCE_INK_LEVELS &&
	    g_strcmp0(panel->queue, panel->pending_queue)) {
		update_controls(panel);
		return;
	}
	if (error) {
		show_result(panel, error->message);
	} else if (panel->pending_operation == MAINTENANCE_INK_LEVELS) {
		panel->ink_job_id = job;
		show_result(panel, _("Updating ink levels…"));
	} else {
		g_autofree char *title = maintenance_title(panel->pending_operation, panel->pending_inks);
		g_autofree char *message = g_strdup_printf(
			_("%s: job #%d submitted to %s."),
			title, job, panel->pending_queue);

		show_result(panel, message);
	}
	update_controls(panel);
}

static void submit(struct maintenance_panel *panel)
{
	g_autoptr(GTask) task = g_task_new(NULL, NULL, submit_done, panel);

	show_result(panel, panel->pending_operation == MAINTENANCE_INK_LEVELS ?
		_("Requesting updated CUPS ink levels…") : _("Submitting the maintenance request to CUPS…"));
	g_task_set_task_data(task, panel, NULL);
	g_task_run_in_thread(task, submit_worker);
}

static void prepare_request(struct maintenance_panel *panel, enum maintenance_operation operation,
			    enum maintenance_inks inks)
{
	g_free(panel->pending_queue);
	panel->pending_queue = g_strdup(panel->queue);
	panel->pending_operation = operation;
	panel->pending_inks = inks;
	panel->busy = TRUE;
	update_controls(panel);
}

static void cleaning_confirmed(GObject *object, GAsyncResult *result, gpointer data)
{
	struct maintenance_panel *panel = data;
	int choice = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(object), result, NULL);

	if (!panel->closing && choice == 1 &&
	    !g_strcmp0(panel->queue, panel->pending_queue) && available(panel, panel->pending_operation)) {
		submit(panel);
		return;
	}
	panel->busy = FALSE;
	if (!panel->closing) {
		if (choice == 1)
			show_result(panel, _("The print queue changed. Select the maintenance operation again."));
		update_controls(panel);
	}
}

static void operation_clicked(GtkButton *button, gpointer data)
{
	struct maintenance_panel *panel = data;
	enum maintenance_operation operation = button == panel->clean ?
		gtk_drop_down_get_selected(panel->strength) : MAINTENANCE_NOZZLE_CHECK;

	if (panel->busy || panel->closing || !available(panel, operation))
		return;
	prepare_request(panel, operation, button == panel->clean ?
			gtk_drop_down_get_selected(panel->inks) : MAINTENANCE_ALL);
	if (button == panel->clean) {
		g_autoptr(GtkAlertDialog) dialog = gtk_alert_dialog_new("%s", _("Start print head cleaning?"));
		g_autofree char *title = maintenance_title(operation, panel->pending_inks);
		g_autofree char *detail = g_strdup_printf(
			_("%s\nPrint queue: %s\n\nCleaning consumes ink. Strong and strongest cleaning use more ink than normal cleaning."),
			title, panel->pending_queue);
		const char *buttons[] = { _("Cancel"), _("Start cleaning"), NULL };

		gtk_alert_dialog_set_detail(dialog, detail);
		gtk_alert_dialog_set_buttons(dialog, buttons);
		gtk_alert_dialog_set_cancel_button(dialog, 0);
		gtk_alert_dialog_set_default_button(dialog, 0);
		g_clear_object(&panel->dialog_cancel);
		panel->dialog_cancel = g_cancellable_new();
		gtk_alert_dialog_choose(dialog, panel->window, panel->dialog_cancel, cleaning_confirmed, panel);
	} else {
		submit(panel);
	}
}

static GtkButton *action_button(struct maintenance_panel *panel, const char *title)
{
	GtkButton *button = GTK_BUTTON(gtk_button_new_with_label(title));

	gtk_widget_set_halign(GTK_WIDGET(button), GTK_ALIGN_START);
	g_signal_connect(button, "clicked", G_CALLBACK(operation_clicked), panel);
	return button;
}

void maintenance_panel_present(struct maintenance_panel *panel)
{
	gtk_window_present(panel->window);
}

static void parent_visibility_changed(GObject *parent, GParamSpec *property, gpointer data)
{
	struct maintenance_panel *panel = data;

	(void)property;
	if (!gtk_widget_get_visible(GTK_WIDGET(parent))) {
		g_cancellable_cancel(panel->dialog_cancel);
		gtk_widget_set_visible(GTK_WIDGET(panel->window), FALSE);
	}
}

struct maintenance_panel *maintenance_panel_new(GtkWindow *parent, GtkLabel *levels_status)
{
	struct maintenance_panel *panel = g_new0(struct maintenance_panel, 1);
	GtkWidget *scroll = gtk_scrolled_window_new();
	GtkWidget *grid = gtk_grid_new();
	GtkBox *nozzle_row = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12));
	GtkWidget *ink_label, *strength_label;
	const char *strengths[] = { maintenance_operation_name(MAINTENANCE_CLEAN),
		maintenance_operation_name(MAINTENANCE_CLEAN_STRONG),
		maintenance_operation_name(MAINTENANCE_CLEAN_STRONGEST), NULL };
	const char *inks[] = { maintenance_ink_name(MAINTENANCE_ALL),
		maintenance_ink_name(MAINTENANCE_BLACK), maintenance_ink_name(MAINTENANCE_COLOR), NULL };

	panel->parent = g_object_ref(parent);
	panel->window = GTK_WINDOW(g_object_ref(gtk_window_new()));
	gtk_window_set_application(panel->window, gtk_window_get_application(parent));
	gtk_window_set_title(panel->window, _("Maintenance"));
	gtk_window_set_transient_for(panel->window, parent);
	gtk_window_set_destroy_with_parent(panel->window, TRUE);
	gtk_window_set_hide_on_close(panel->window, TRUE);
	gtk_window_set_default_size(panel->window, 640, -1);
	gtk_window_set_child(panel->window, scroll);
	panel->levels_status = g_object_ref(levels_status);
	panel->dialog_cancel = g_cancellable_new();
	panel->box = GTK_BOX(g_object_ref_sink(gtk_box_new(GTK_ORIENTATION_VERTICAL, 8)));
	gtk_widget_set_margin_start(GTK_WIDGET(panel->box), 24);
	gtk_widget_set_margin_end(GTK_WIDGET(panel->box), 24);
	gtk_widget_set_margin_top(GTK_WIDGET(panel->box), 20);
	gtk_widget_set_margin_bottom(GTK_WIDGET(panel->box), 20);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), GTK_WIDGET(panel->box));
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
	panel->queue_label = panel_label(panel->box, "");
	gtk_box_append(panel->box, grid);
	gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
	gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
	ink_label = gtk_label_new(_("Print heads to clean"));
	strength_label = gtk_label_new(_("Cleaning strength"));
	gtk_label_set_xalign(GTK_LABEL(ink_label), 0);
	gtk_label_set_xalign(GTK_LABEL(strength_label), 0);
	gtk_grid_attach(GTK_GRID(grid), ink_label, 0, 0, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), strength_label, 1, 0, 1, 1);
	panel->inks = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(inks));
	panel->strength = GTK_DROP_DOWN(gtk_drop_down_new_from_strings(strengths));
	gtk_widget_set_hexpand(GTK_WIDGET(panel->inks), TRUE);
	gtk_widget_set_hexpand(GTK_WIDGET(panel->strength), TRUE);
	gtk_accessible_update_relation(GTK_ACCESSIBLE(panel->inks),
		GTK_ACCESSIBLE_RELATION_LABELLED_BY, ink_label, NULL, -1);
	gtk_accessible_update_relation(GTK_ACCESSIBLE(panel->strength),
		GTK_ACCESSIBLE_RELATION_LABELLED_BY, strength_label, NULL, -1);
	gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(panel->inks), 0, 1, 1, 1);
	gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(panel->strength), 1, 1, 1, 1);
	panel->clean = action_button(panel, _("Clean print heads…"));
	gtk_grid_attach(GTK_GRID(grid), GTK_WIDGET(panel->clean), 2, 1, 1, 1);
	gtk_box_append(panel->box, GTK_WIDGET(nozzle_row));
	panel->nozzles = action_button(panel, _("Print nozzle check"));
	gtk_box_append(nozzle_row, GTK_WIDGET(panel->nozzles));
	panel_label(nozzle_row, _("The nozzle check prints one sheet."));
	panel->status = panel_label(panel->box, "");
	gtk_label_set_selectable(panel->status, TRUE);
	gtk_widget_set_visible(GTK_WIDGET(panel->status), FALSE);
	g_signal_connect(panel->strength, "notify::selected", G_CALLBACK(selection_changed), panel);
	g_signal_connect(panel->parent, "notify::visible", G_CALLBACK(parent_visibility_changed), panel);
	maintenance_panel_follow(panel, "", 0, FALSE);
	return panel;
}

void maintenance_panel_follow(struct maintenance_panel *panel, const char *queue,
			      unsigned int commands, gboolean accepting_jobs)
{
	g_autofree char *text = NULL;

	if (!panel || panel->closing)
		return;
	if (g_strcmp0(panel->queue, queue)) {
		panel->ink_job_id = 0;
		panel->next_ink_update = 0;
		gtk_label_set_text(panel->status, "");
		gtk_widget_set_visible(GTK_WIDGET(panel->status), FALSE);
		gtk_label_set_text(panel->levels_status, "");
		gtk_widget_set_visible(GTK_WIDGET(panel->levels_status), FALSE);
	}
	g_free(panel->queue);
	panel->queue = g_strdup(queue);
	panel->commands = commands;
	panel->accepting_jobs = accepting_jobs;
	if (!queue || !*queue)
		text = g_strdup(_("Select a CUPS queue in Settings → Printers, then confirm to use maintenance."));
	else if (!commands)
		text = g_strdup_printf(_("Maintenance is unavailable for %s. Check CUPS and select the Brother MFC-J5720DW CUPS driver."), queue);
	else if (!accepting_jobs)
		text = g_strdup_printf(_("Print queue: %s — not accepting jobs"), queue);
	else
		text = g_strdup_printf(_("Print queue: %s"), queue);
	gtk_label_set_text(panel->queue_label, text);
	update_controls(panel);
}

int maintenance_panel_ink_job(struct maintenance_panel *panel)
{
	return panel && !panel->closing ? panel->ink_job_id : 0;
}

static void clear_ink_status(struct maintenance_panel *panel)
{
	gtk_label_set_text(panel->levels_status, "");
	gtk_widget_set_visible(GTK_WIDGET(panel->levels_status), FALSE);
}

static void synchronize_ink(struct maintenance_panel *panel, const struct monitor_snapshot *snapshot)
{
	gboolean matches = snapshot->cups_ink_valid;
	unsigned int i;
	gint64 now;

	if (panel->busy || !snapshot->online || !snapshot->ink_valid || !snapshot->cups_valid ||
	    !snapshot->queue_host[0] || g_ascii_strcasecmp(snapshot->queue_host, snapshot->target.host))
		return;
	for (i = 0; i < G_N_ELEMENTS(snapshot->ink); i++) {
		/* Match the command filter's rounding, including nearly empty tanks. */
		unsigned int percent = snapshot->ink[i] ? MAX(1U, (snapshot->ink[i] + 50) / 100) : 0;

		matches &= percent == snapshot->cups_ink[i];
	}
	if (matches) {
		clear_ink_status(panel);
		return;
	}
	now = g_get_monotonic_time();
	if (!available(panel, MAINTENANCE_INK_LEVELS) || snapshot->queue_state != IPP_PSTATE_IDLE ||
	    now < panel->next_ink_update)
		return;
	/* Read-only ink jobs are throttled even after failure or a lost reply. */
	panel->next_ink_update = now + INK_UPDATE_INTERVAL;
	prepare_request(panel, MAINTENANCE_INK_LEVELS, MAINTENANCE_ALL);
	submit(panel);
}

void maintenance_panel_update_ink(struct maintenance_panel *panel,
				  const struct monitor_snapshot *snapshot)
{
	g_autofree char *message = NULL;

	if (!panel || panel->closing ||
	    g_strcmp0(panel->queue, snapshot->target.queue))
		return;
	if (!panel->ink_job_id) {
		synchronize_ink(panel, snapshot);
		return;
	}
	if (snapshot->target.ink_job_id != panel->ink_job_id)
		return;
	if (!snapshot->ink_job_valid) {
		message = g_strdup(_("Unable to confirm the ink-level update."));
	} else if (snapshot->ink_job.state == IPP_JSTATE_COMPLETED) {
		clear_ink_status(panel);
	} else if (snapshot->ink_job.state == IPP_JSTATE_CANCELED) {
		message = g_strdup(_("Ink-level update cancelled."));
	} else if (snapshot->ink_job.state == IPP_JSTATE_ABORTED) {
		message = g_strdup(_("Ink-level update failed."));
	} else {
		return;
	}
	panel->ink_job_id = 0;
	if (message) {
		gtk_label_set_text(panel->levels_status, message);
		gtk_widget_set_visible(GTK_WIDGET(panel->levels_status), TRUE);
	}
	update_controls(panel);
}

void maintenance_panel_free(struct maintenance_panel *panel)
{
	if (!panel)
		return;
	panel->closing = TRUE;
	g_cancellable_cancel(panel->dialog_cancel);
	/* Finish any in-flight submission before exiting, but do not wait for
	 * the printer to finish its maintenance job. Callbacks retain this panel
	 * until busy clears, including cancellation of a confirmation dialog.
	 */
	while (panel->busy)
		g_main_context_iteration(NULL, TRUE);
	g_signal_handlers_disconnect_by_data(panel->strength, panel);
	g_signal_handlers_disconnect_by_data(panel->clean, panel);
	g_signal_handlers_disconnect_by_data(panel->nozzles, panel);
	g_signal_handlers_disconnect_by_data(panel->parent, panel);
	gtk_window_destroy(panel->window);
	g_object_unref(panel->dialog_cancel);
	g_object_unref(panel->box);
	g_object_unref(panel->window);
	g_object_unref(panel->parent);
	g_object_unref(panel->levels_status);
	g_free(panel->pending_queue);
	g_free(panel->queue);
	g_free(panel);
}
