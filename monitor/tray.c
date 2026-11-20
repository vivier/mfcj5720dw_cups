/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Laurent Vivier */
#include "tray.h"

#include <glib/gi18n.h>
#include <libdbusmenu-glib/menuitem.h>
#include <libdbusmenu-glib/server.h>
#include <string.h>

#define ITEM_PATH "/StatusNotifierItem"
#define ITEM_INTERFACE "org.kde.StatusNotifierItem"
#define WATCHER_NAME "org.kde.StatusNotifierWatcher"
#define WATCHER_PATH "/StatusNotifierWatcher"
#define MENU_PATH "/StatusNotifierMenu"
#define ICON_NAME "printer-symbolic"

/* StatusNotifierItem works with GTK 4 without loading GTK 3 AppIndicator code.
 * The menu is exported by the GTK-independent part of libdbusmenu.
 */
static const char item_xml[] =
	"<node><interface name='" ITEM_INTERFACE "'>"
	"<property name='Category' type='s' access='read'/>"
	"<property name='Id' type='s' access='read'/>"
	"<property name='Title' type='s' access='read'/>"
	"<property name='Status' type='s' access='read'/>"
	"<property name='WindowId' type='i' access='read'/>"
	"<property name='IconName' type='s' access='read'/>"
	"<property name='IconThemePath' type='s' access='read'/>"
	"<property name='IconPixmap' type='a(iiay)' access='read'/>"
	"<property name='OverlayIconName' type='s' access='read'/>"
	"<property name='OverlayIconPixmap' type='a(iiay)' access='read'/>"
	"<property name='AttentionIconName' type='s' access='read'/>"
	"<property name='AttentionIconPixmap' type='a(iiay)' access='read'/>"
	"<property name='AttentionMovieName' type='s' access='read'/>"
	"<property name='ToolTip' type='(sa(iiay)ss)' access='read'/>"
	"<property name='ItemIsMenu' type='b' access='read'/>"
	"<property name='Menu' type='o' access='read'/>"
	"<method name='Activate'><arg type='i' direction='in'/>"
	"<arg type='i' direction='in'/></method>"
	"<method name='SecondaryActivate'><arg type='i' direction='in'/>"
	"<arg type='i' direction='in'/></method>"
	"<method name='ContextMenu'><arg type='i' direction='in'/>"
	"<arg type='i' direction='in'/></method>"
	"<method name='Scroll'><arg type='i' direction='in'/>"
	"<arg type='s' direction='in'/></method>"
	"<method name='ProvideXdgActivationToken'>"
	"<arg type='s' direction='in'/></method>"
	"<signal name='NewTitle'/><signal name='NewIcon'/>"
	"<signal name='NewOverlayIcon'/><signal name='NewAttentionIcon'/>"
	"<signal name='NewToolTip'/>"
	"<signal name='NewStatus'><arg type='s'/></signal>"
	"</interface></node>";

struct monitor_tray {
	GtkApplication *application;
	GtkWindow *window;
	GDBusConnection *connection;
	GCancellable *cancellable;
	DbusmenuServer *menu;
	guint object_id;
	guint watcher_id;
};

static GVariant *empty_pixmaps(void)
{
	return g_variant_new_array(G_VARIANT_TYPE("(iiay)"), NULL, 0);
}

static GVariant *get_property(GDBusConnection *connection, const char *sender,
			      const char *path, const char *interface,
			      const char *property, GError **error, gpointer data)
{
	const char *title = _("Brother MFC-J5720DW — Status monitor");
	const struct {
		const char *name;
		const char *value;
	} strings[] = {
		{ "Category", "Hardware" },
		{ "Id", "mfcj5720dw-monitor" },
		{ "Title", title },
		/* Active keeps the icon visible even when the window is hidden. */
		{ "Status", "Active" },
		{ "IconName", ICON_NAME },
		{ "IconThemePath", "" },
		{ "OverlayIconName", "" },
		{ "AttentionIconName", "" },
		{ "AttentionMovieName", "" },
	};
	size_t i;

	(void)connection; (void)sender; (void)path; (void)interface; (void)data;
	for (i = 0; i < G_N_ELEMENTS(strings); i++)
		if (!strcmp(property, strings[i].name))
			return g_variant_new_string(strings[i].value);
	if (!strcmp(property, "WindowId"))
		return g_variant_new_int32(0);
	if (!strcmp(property, "ItemIsMenu"))
		return g_variant_new_boolean(FALSE);
	if (!strcmp(property, "Menu"))
		return g_variant_new_object_path(MENU_PATH);
	if (!strcmp(property, "IconPixmap") ||
	    !strcmp(property, "OverlayIconPixmap") ||
	    !strcmp(property, "AttentionIconPixmap"))
		return empty_pixmaps();
	if (!strcmp(property, "ToolTip"))
		return g_variant_new("(s@a(iiay)ss)", ICON_NAME, empty_pixmaps(),
				     title, "");
	g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY,
		    "Unknown tray property: %s", property);
	return NULL;
}

static void call_method(GDBusConnection *connection, const char *sender,
			const char *path, const char *interface, const char *method,
			GVariant *parameters, GDBusMethodInvocation *invocation,
			gpointer data)
{
	struct monitor_tray *tray = data;
	const char *token;

	(void)connection; (void)sender; (void)path; (void)interface;
	if (!strcmp(method, "ProvideXdgActivationToken")) {
		/* Let Wayland grant focus when the desktop activates the window. */
		g_variant_get(parameters, "(&s)", &token);
		gtk_window_set_startup_id(tray->window, token);
	} else if (!strcmp(method, "Activate") ||
		   !strcmp(method, "SecondaryActivate") ||
		   !strcmp(method, "ContextMenu")) {
		/* Hosts normally display the exported menu themselves. A host
		 * requesting ContextMenu directly can still open the monitor.
		 */
		g_action_group_activate_action(G_ACTION_GROUP(tray->application),
					       "show", NULL);
	} else if (strcmp(method, "Scroll")) {
		g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR,
				G_DBUS_ERROR_UNKNOWN_METHOD, "Unknown tray method: %s", method);
		return;
	}
	g_dbus_method_invocation_return_value(invocation, NULL);
}

static void menu_activated(DbusmenuMenuitem *item, guint timestamp, gpointer data)
{
	struct monitor_tray *tray = data;
	const char *action = g_object_get_data(G_OBJECT(item), "monitor-action");

	(void)timestamp;
	g_action_group_activate_action(G_ACTION_GROUP(tray->application), action, NULL);
}

static void add_menu_item(struct monitor_tray *tray, DbusmenuMenuitem *root,
			  const char *label, const char *action)
{
	DbusmenuMenuitem *item = dbusmenu_menuitem_new();

	dbusmenu_menuitem_property_set(item, DBUSMENU_MENUITEM_PROP_LABEL, label);
	dbusmenu_menuitem_property_set_bool(item, DBUSMENU_MENUITEM_PROP_VISIBLE, TRUE);
	dbusmenu_menuitem_property_set_bool(item, DBUSMENU_MENUITEM_PROP_ENABLED, TRUE);
	g_object_set_data_full(G_OBJECT(item), "monitor-action", g_strdup(action), g_free);
	g_signal_connect(item, DBUSMENU_MENUITEM_SIGNAL_ITEM_ACTIVATED,
			 G_CALLBACK(menu_activated), tray);
	dbusmenu_menuitem_child_append(root, item);
	g_object_unref(item);
}

static void registered(GObject *object, GAsyncResult *result, gpointer data)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GVariant) reply = g_dbus_connection_call_finish(
					G_DBUS_CONNECTION(object), result, &error);

	/* No tray pointer: this callback may run after shutdown cancelled it. */
	(void)data;
	if (!reply && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
		g_debug("Cannot register tray icon: %s", error->message);
}

static void watcher_appeared(GDBusConnection *connection, const char *name,
			     const char *owner, gpointer data)
{
	struct monitor_tray *tray = data;

	(void)name;
	/* Watch for late host startup and re-register after a shell restart. */
	g_dbus_connection_call(connection, owner, WATCHER_PATH, WATCHER_NAME,
			       "RegisterStatusNotifierItem", g_variant_new("(s)", ITEM_PATH),
			       NULL, G_DBUS_CALL_FLAGS_NONE, -1, tray->cancellable,
			       registered, NULL);
}

struct monitor_tray *monitor_tray_new(GtkApplication *application,
				      GtkWindow *window, GError **error)
{
	const GDBusInterfaceVTable vtable = {
		.method_call = call_method,
		.get_property = get_property,
	};
	g_autoptr(GDBusNodeInfo) info = g_dbus_node_info_new_for_xml(item_xml, error);
	GDBusConnection *connection = g_application_get_dbus_connection(G_APPLICATION(application));
	struct monitor_tray *tray;
	DbusmenuMenuitem *root;

	if (!info)
		return NULL;
	if (!connection) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
				    "No session bus for the tray icon");
		return NULL;
	}
	tray = g_new0(struct monitor_tray, 1);
	tray->application = g_object_ref(application);
	tray->window = g_object_ref(window);
	tray->connection = g_object_ref(connection);
	tray->cancellable = g_cancellable_new();
	tray->object_id = g_dbus_connection_register_object(connection, ITEM_PATH,
				info->interfaces[0], &vtable, tray, NULL, error);
	if (!tray->object_id) {
		monitor_tray_free(tray);
		return NULL;
	}
	root = dbusmenu_menuitem_new();
	add_menu_item(tray, root, _("Show monitor"), "show");
	add_menu_item(tray, root, _("Settings"), "settings");
	add_menu_item(tray, root, _("Quit monitor"), "quit");
	tray->menu = dbusmenu_server_new(MENU_PATH);
	dbusmenu_server_set_root(tray->menu, root);
	g_object_unref(root);
	tray->watcher_id = g_bus_watch_name_on_connection(connection, WATCHER_NAME,
				G_BUS_NAME_WATCHER_FLAGS_NONE, watcher_appeared, NULL,
				tray, NULL);
	return tray;
}

void monitor_tray_free(struct monitor_tray *tray)
{
	if (!tray)
		return;
	if (tray->watcher_id)
		g_bus_unwatch_name(tray->watcher_id);
	g_cancellable_cancel(tray->cancellable);
	if (tray->object_id)
		g_dbus_connection_unregister_object(tray->connection, tray->object_id);
	g_clear_object(&tray->menu);
	g_object_unref(tray->cancellable);
	g_object_unref(tray->connection);
	g_object_unref(tray->window);
	g_object_unref(tray->application);
	g_free(tray);
}
