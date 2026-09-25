// SPDX-License-Identifier: MIT
#include "tray.h"

#ifdef CDM_HAVE_TRAY

#include <gio/gio.h>
#include <libdbusmenu-glib/menuitem.h>
#include <libdbusmenu-glib/server.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define TRAY_ITEM_PATH "/StatusNotifierItem"
#define TRAY_MENU_PATH "/MenuBar"
#define TRAY_MENU_MAX 32

typedef struct {
  GMutex mutex;
  GCond ready_cond;
  GThread *thread;
  GMainContext *context;
  GMainLoop *loop;
  GDBusConnection *bus;
  GDBusNodeInfo *node;
  DbusmenuServer *menu;
  guint registration;
  bool ready;
  int result;
  char *icon;
  char *tooltip;
  char name[96];
} TrayState;

/* API callers are daemon-loop owned; the mutex covers startup/shutdown and
 * posting work. D-Bus/menu objects are owned only by the GLib tray thread. */
static TrayState g_tray;

static const char item_xml[] =
    "<node><interface name='org.freedesktop.StatusNotifierItem'>"
    "<method name='Activate'><arg type='i' direction='in'/>"
    "<arg type='i' direction='in'/></method>"
    "<method name='SecondaryActivate'><arg type='i' direction='in'/>"
    "<arg type='i' direction='in'/></method>"
    "<method name='ContextMenu'><arg type='i' direction='in'/>"
    "<arg type='i' direction='in'/></method>"
    "<property name='Category' type='s' access='read'/>"
    "<property name='Id' type='s' access='read'/>"
    "<property name='Title' type='s' access='read'/>"
    "<property name='Status' type='s' access='read'/>"
    "<property name='WindowId' type='u' access='read'/>"
    "<property name='IconName' type='s' access='read'/>"
    "<property name='IconPixmap' type='a(iiay)' access='read'/>"
    "<property name='ToolTip' type='(sa(iiay)ss)' access='read'/>"
    "<property name='ItemIsMenu' type='b' access='read'/>"
    "<property name='Menu' type='o' access='read'/>"
    "<signal name='NewToolTip'/><signal name='NewIcon'/>"
    "</interface></node>";

static void item_method(GDBusConnection *bus, const char *sender,
                        const char *path, const char *interface,
                        const char *method, GVariant *parameters,
                        GDBusMethodInvocation *invocation, void *user_data) {
  (void)bus;
  (void)sender;
  (void)path;
  (void)interface;
  (void)method;
  (void)parameters;
  (void)user_data;
  g_dbus_method_invocation_return_value(invocation, NULL);
}

static GVariant *empty_pixmaps(void) {
  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE("a(iiay)"));
  return g_variant_builder_end(&builder);
}

static GVariant *item_property(GDBusConnection *bus, const char *sender,
                               const char *path, const char *interface,
                               const char *property, GError **error,
                               void *user_data) {
  (void)bus;
  (void)sender;
  (void)path;
  (void)interface;
  (void)error;
  TrayState *tray = user_data;
  if (strcmp(property, "Category") == 0)
    return g_variant_new_string("ApplicationStatus");
  if (strcmp(property, "Id") == 0)
    return g_variant_new_string("cdm");
  if (strcmp(property, "Title") == 0)
    return g_variant_new_string("cdm Download Manager");
  if (strcmp(property, "Status") == 0)
    return g_variant_new_string("Active");
  if (strcmp(property, "WindowId") == 0)
    return g_variant_new_uint32(0);
  if (strcmp(property, "IconName") == 0)
    return g_variant_new_string(tray->icon);
  if (strcmp(property, "IconPixmap") == 0)
    return empty_pixmaps();
  if (strcmp(property, "ToolTip") == 0)
    return g_variant_new("(s@a(iiay)ss)", tray->icon, empty_pixmaps(),
                         "cdm", tray->tooltip);
  if (strcmp(property, "ItemIsMenu") == 0)
    return g_variant_new_boolean(true);
  if (strcmp(property, "Menu") == 0)
    return g_variant_new_object_path(TRAY_MENU_PATH);
  return NULL;
}

static const GDBusInterfaceVTable item_vtable = {
    .method_call = item_method, .get_property = item_property};

static bool watcher_ready(GDBusConnection *bus, const char *name,
                          const char *interface) {
  GError *error = NULL;
  GVariant *reply = g_dbus_connection_call_sync(
      bus, name, "/StatusNotifierWatcher", "org.freedesktop.DBus.Properties",
      "Get", g_variant_new("(ss)", interface,
                            "IsStatusNotifierHostRegistered"),
      G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 1500, NULL, &error);
  if (!reply) {
    g_clear_error(&error);
    return false;
  }
  GVariant *value = NULL;
  g_variant_get(reply, "(v)", &value);
  bool ready = g_variant_get_boolean(value);
  g_variant_unref(value);
  g_variant_unref(reply);
  return ready;
}

static bool register_watcher(TrayState *tray) {
  static const char *names[] = {"org.kde.StatusNotifierWatcher",
                                "org.freedesktop.StatusNotifierWatcher"};
  for (size_t i = 0; i < 2; ++i) {
    if (!watcher_ready(tray->bus, names[i], names[i]))
      continue;
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_sync(
        tray->bus, names[i], "/StatusNotifierWatcher", names[i],
        "RegisterStatusNotifierItem", g_variant_new("(s)", tray->name),
        NULL, G_DBUS_CALL_FLAGS_NONE, 1500, NULL, &error);
    if (reply) {
      g_variant_unref(reply);
      return true;
    }
    g_clear_error(&error);
  }
  return false;
}

static bool request_name(TrayState *tray) {
  GError *error = NULL;
  GVariant *reply = g_dbus_connection_call_sync(
      tray->bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", "RequestName",
      g_variant_new("(su)", tray->name, 0u), G_VARIANT_TYPE("(u)"),
      G_DBUS_CALL_FLAGS_NONE, 1500, NULL, &error);
  if (!reply) {
    g_clear_error(&error);
    return false;
  }
  guint result = 0;
  g_variant_get(reply, "(u)", &result);
  g_variant_unref(reply);
  return result == 1;
}

static void tray_worker_cleanup(TrayState *tray) {
  if (tray->registration)
    g_dbus_connection_unregister_object(tray->bus, tray->registration);
  if (tray->menu)
    g_object_unref(tray->menu);
  if (tray->node)
    g_dbus_node_info_unref(tray->node);
  if (tray->bus)
    g_object_unref(tray->bus);
  if (tray->loop)
    g_main_loop_unref(tray->loop);
  g_free(tray->tooltip);
  tray->tooltip = NULL;
  g_main_context_pop_thread_default(tray->context);
  g_main_context_unref(tray->context);
}

static gpointer tray_worker(gpointer data) {
  TrayState *tray = data;
  /* libdbusmenu-glib dispatches its D-Bus signals on the default context. */
  tray->context = g_main_context_ref(g_main_context_default());
  g_main_context_push_thread_default(tray->context);
  tray->loop = g_main_loop_new(tray->context, FALSE);
  tray->tooltip = g_strdup("No active downloads");
  GError *error = NULL;
  tray->bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
  if (!tray->bus)
    goto ready;
  tray->node = g_dbus_node_info_new_for_xml(item_xml, &error);
  if (!tray->node)
    goto ready;
  tray->registration = g_dbus_connection_register_object(
      tray->bus, TRAY_ITEM_PATH, tray->node->interfaces[0], &item_vtable,
      tray, NULL, &error);
  if (!tray->registration)
    goto ready;
  tray->menu = dbusmenu_server_new(TRAY_MENU_PATH);
  if (!tray->menu)
    goto ready;
  DbusmenuMenuitem *root = dbusmenu_menuitem_new();
  dbusmenu_menuitem_set_root(root, TRUE);
  dbusmenu_server_set_root(tray->menu, root);
  g_object_unref(root);
  if (!request_name(tray) || !register_watcher(tray))
    goto ready;
  tray->result = 0;

ready:
  if (error)
    g_error_free(error);
  g_mutex_lock(&tray->mutex);
  tray->ready = true;
  g_cond_signal(&tray->ready_cond);
  g_mutex_unlock(&tray->mutex);
  if (tray->result == 0)
    g_main_loop_run(tray->loop);
  tray_worker_cleanup(tray);
  return NULL;
}

static void post_source(GSourceFunc callback, gpointer data,
                        GDestroyNotify destroy) {
  GSource *source = g_idle_source_new();
  g_source_set_callback(source, callback, data, destroy);
  g_source_attach(source, g_tray.context);
  g_source_unref(source);
}

int tray_init(const char *icon_path) {
  g_mutex_lock(&g_tray.mutex);
  if (g_tray.thread) {
    int result = g_tray.result;
    g_mutex_unlock(&g_tray.mutex);
    return result;
  }
  g_tray.ready = false;
  g_tray.result = -1;
  g_tray.icon = g_strdup(icon_path && *icon_path ? icon_path :
                         "folder-download");
  int written = snprintf(g_tray.name, sizeof(g_tray.name),
                         "org.freedesktop.StatusNotifierItem-%ld-1",
                         (long)getpid());
  if (!g_tray.icon || written < 0 ||
      (size_t)written >= sizeof(g_tray.name)) {
    g_free(g_tray.icon);
    g_tray.icon = NULL;
    g_mutex_unlock(&g_tray.mutex);
    return -1;
  }
  g_tray.thread = g_thread_new("cdm-tray", tray_worker, &g_tray);
  if (!g_tray.thread) {
    g_free(g_tray.icon);
    g_tray.icon = NULL;
    g_mutex_unlock(&g_tray.mutex);
    return -1;
  }
  while (!g_tray.ready)
    g_cond_wait(&g_tray.ready_cond, &g_tray.mutex);
  int result = g_tray.result;
  GThread *failed = result == 0 ? NULL : g_tray.thread;
  g_mutex_unlock(&g_tray.mutex);
  if (failed) {
    g_thread_join(failed);
    g_mutex_lock(&g_tray.mutex);
    g_tray.thread = NULL;
    g_tray.context = NULL;
    g_tray.loop = NULL;
    g_tray.bus = NULL;
    g_tray.node = NULL;
    g_tray.menu = NULL;
    g_tray.registration = 0;
    g_free(g_tray.icon);
    g_tray.icon = NULL;
    g_mutex_unlock(&g_tray.mutex);
  }
  return result;
}

typedef struct {
  uint64_t received, total;
} ProgressUpdate;

static gboolean apply_progress(gpointer data) {
  ProgressUpdate *update = data;
  g_free(g_tray.tooltip);
  if (update->total)
    g_tray.tooltip = g_strdup_printf("%llu of %llu bytes downloaded",
        (unsigned long long)update->received,
        (unsigned long long)update->total);
  else
    g_tray.tooltip = g_strdup("No active downloads");
  g_dbus_connection_emit_signal(g_tray.bus, NULL, TRAY_ITEM_PATH,
      "org.freedesktop.StatusNotifierItem", "NewToolTip", NULL, NULL);
  return G_SOURCE_REMOVE;
}

void tray_set_progress(uint64_t received, uint64_t total) {
  ProgressUpdate *update = g_new(ProgressUpdate, 1);
  update->received = received;
  update->total = total;
  g_mutex_lock(&g_tray.mutex);
  if (g_tray.thread && g_tray.result == 0)
    post_source(apply_progress, update, g_free);
  else
    g_free(update);
  g_mutex_unlock(&g_tray.mutex);
}

typedef struct {
  size_t count;
  TrayMenuItem items[TRAY_MENU_MAX];
} MenuUpdate;

static void free_menu_update(gpointer data) {
  MenuUpdate *update = data;
  for (size_t i = 0; i < update->count; ++i)
    g_free((char *)update->items[i].label);
  g_free(update);
}

static void activate_menu_item(DbusmenuMenuitem *item, guint timestamp,
                               gpointer data) {
  (void)item;
  (void)timestamp;
  TrayMenuItem *choice = data;
  if (choice->activate)
    choice->activate(choice->user_data);
}

static void free_choice(gpointer data, GClosure *closure) {
  (void)closure;
  g_free(data);
}

static gboolean apply_menu(gpointer data) {
  MenuUpdate *update = data;
  DbusmenuMenuitem *root = dbusmenu_menuitem_new();
  dbusmenu_menuitem_set_root(root, TRUE);
  for (size_t i = 0; i < update->count; ++i) {
    DbusmenuMenuitem *item = dbusmenu_menuitem_new();
    dbusmenu_menuitem_property_set(item, DBUSMENU_MENUITEM_PROP_LABEL,
                                    update->items[i].label);
    dbusmenu_menuitem_property_set_bool(item, DBUSMENU_MENUITEM_PROP_ENABLED,
                                         update->items[i].enabled);
    TrayMenuItem *choice = g_new(TrayMenuItem, 1);
    *choice = update->items[i];
    choice->label = NULL;
    g_signal_connect_data(item, DBUSMENU_MENUITEM_SIGNAL_ITEM_ACTIVATED,
        G_CALLBACK(activate_menu_item), choice, free_choice, 0);
    /* child_append takes ownership of the item reference. */
    dbusmenu_menuitem_child_append(root, item);
  }
  dbusmenu_server_set_root(g_tray.menu, root);
  g_object_unref(root);
  return G_SOURCE_REMOVE;
}

void tray_set_menu(const TrayMenuItem *items, size_t count) {
  if ((count && !items) || count > TRAY_MENU_MAX)
    return;
  MenuUpdate *update = g_new0(MenuUpdate, 1);
  update->count = count;
  for (size_t i = 0; i < count; ++i) {
    update->items[i] = items[i];
    update->items[i].label = g_strdup(items[i].label ? items[i].label : "");
  }
  g_mutex_lock(&g_tray.mutex);
  if (g_tray.thread && g_tray.result == 0)
    post_source(apply_menu, update, free_menu_update);
  else
    free_menu_update(update);
  g_mutex_unlock(&g_tray.mutex);
}

static gboolean quit_loop(gpointer data) {
  g_main_loop_quit(data);
  return G_SOURCE_REMOVE;
}

void tray_shutdown(void) {
  g_mutex_lock(&g_tray.mutex);
  GThread *thread = g_tray.thread;
  if (thread && g_tray.result == 0)
    post_source(quit_loop, g_tray.loop, NULL);
  g_mutex_unlock(&g_tray.mutex);
  if (!thread)
    return;
  g_thread_join(thread);
  g_mutex_lock(&g_tray.mutex);
  g_tray.thread = NULL;
  g_tray.context = NULL;
  g_tray.loop = NULL;
  g_tray.bus = NULL;
  g_tray.node = NULL;
  g_tray.menu = NULL;
  g_tray.registration = 0;
  g_free(g_tray.icon);
  g_tray.icon = NULL;
  g_mutex_unlock(&g_tray.mutex);
}

#else

int tray_init(const char *icon_path) {
  (void)icon_path;
  return -1;
}
void tray_set_progress(uint64_t received, uint64_t total) {
  (void)received;
  (void)total;
}
void tray_set_menu(const TrayMenuItem *items, size_t count) {
  (void)items;
  (void)count;
}
void tray_shutdown(void) {}

#endif
