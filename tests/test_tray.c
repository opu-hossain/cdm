#include "../src/platform/tray.h"
#include <criterion/criterion.h>
#include <stdlib.h>
#include <unistd.h>

#ifdef CDM_HAVE_TRAY
#include <gio/gio.h>

typedef struct {
  GMutex mutex;
  GCond ready_cond;
  GMainContext *context;
  GMainLoop *loop;
  gboolean ready;
  gboolean registered;
} FakeWatcher;

static gint menu_clicks;

static void count_menu_click(void *user_data) {
  (void)user_data;
  g_atomic_int_inc(&menu_clicks);
}

static GVariant *watcher_property(GDBusConnection *connection,
                                  const char *sender, const char *path,
                                  const char *interface, const char *property,
                                  GError **error, gpointer user_data) {
  (void)connection;
  (void)sender;
  (void)path;
  (void)interface;
  (void)error;
  (void)user_data;
  if (g_strcmp0(property, "IsStatusNotifierHostRegistered") == 0)
    return g_variant_new_boolean(TRUE);
  return NULL;
}

static void watcher_method(GDBusConnection *connection, const char *sender,
                           const char *path, const char *interface,
                           const char *method, GVariant *parameters,
                           GDBusMethodInvocation *invocation,
                           gpointer user_data) {
  (void)connection;
  (void)sender;
  (void)path;
  (void)interface;
  (void)parameters;
  FakeWatcher *watcher = user_data;
  if (g_strcmp0(method, "RegisterStatusNotifierItem") == 0)
    g_atomic_int_set(&watcher->registered, TRUE);
  g_dbus_method_invocation_return_value(invocation, NULL);
}

static gpointer fake_watcher_thread(gpointer data) {
  FakeWatcher *watcher = data;
  watcher->context = g_main_context_new();
  g_main_context_push_thread_default(watcher->context);
  watcher->loop = g_main_loop_new(watcher->context, FALSE);
  GError *error = NULL;
  GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
  cr_assert_null(error);
  const char *xml =
      "<node><interface name='org.kde.StatusNotifierWatcher'>"
      "<method name='RegisterStatusNotifierItem'>"
      "<arg type='s' direction='in'/></method>"
      "<property name='IsStatusNotifierHostRegistered' type='b' "
      "access='read'/></interface></node>";
  GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(xml, &error);
  cr_assert_null(error);
  const GDBusInterfaceVTable vtable = {
      .method_call = watcher_method, .get_property = watcher_property};
  guint registration = g_dbus_connection_register_object(
      bus, "/StatusNotifierWatcher", node->interfaces[0], &vtable,
      watcher, NULL, &error);
  cr_assert_gt(registration, 0);
  cr_assert_null(error);
  GVariant *reply = g_dbus_connection_call_sync(
      bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", "RequestName",
      g_variant_new("(su)", "org.kde.StatusNotifierWatcher", 0u),
      G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &error);
  cr_assert_not_null(reply);
  cr_assert_null(error);
  g_variant_unref(reply);
  g_mutex_lock(&watcher->mutex);
  watcher->ready = TRUE;
  g_cond_signal(&watcher->ready_cond);
  g_mutex_unlock(&watcher->mutex);
  g_main_loop_run(watcher->loop);
  g_dbus_connection_unregister_object(bus, registration);
  g_dbus_node_info_unref(node);
  g_object_unref(bus);
  g_main_loop_unref(watcher->loop);
  g_main_context_pop_thread_default(watcher->context);
  g_main_context_unref(watcher->context);
  return NULL;
}

static gboolean stop_fake_watcher(gpointer data) {
  g_main_loop_quit(data);
  return G_SOURCE_REMOVE;
}

Test(tray, registers_with_a_local_status_notifier_host) {
  GTestDBus *session = g_test_dbus_new(G_TEST_DBUS_NONE);
  g_test_dbus_up(session);
  FakeWatcher watcher = {0};
  GThread *thread = g_thread_new("fake-watcher", fake_watcher_thread,
                                 &watcher);
  g_mutex_lock(&watcher.mutex);
  while (!watcher.ready)
    g_cond_wait(&watcher.ready_cond, &watcher.mutex);
  g_mutex_unlock(&watcher.mutex);
  cr_assert_eq(tray_init("folder-download"), 0);
  cr_assert(g_atomic_int_get(&watcher.registered));
  GError *error = NULL;
  GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
  cr_assert_not_null(bus);
  cr_assert_null(error);
  char *item_name = g_strdup_printf(
      "org.freedesktop.StatusNotifierItem-%ld-1", (long)getpid());
  GVariant *icon_reply = g_dbus_connection_call_sync(
      bus, item_name, "/StatusNotifierItem", "org.freedesktop.DBus.Properties",
      "Get", g_variant_new("(ss)", "org.freedesktop.StatusNotifierItem",
                             "IconName"), G_VARIANT_TYPE("(v)"),
      G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &error);
  cr_assert_not_null(icon_reply);
  cr_assert_null(error);
  GVariant *icon_value = NULL;
  g_variant_get(icon_reply, "(v)", &icon_value);
  cr_assert_str_eq(g_variant_get_string(icon_value, NULL), "folder-download");
  g_variant_unref(icon_value);
  g_variant_unref(icon_reply);
  GVariant *layout_reply = g_dbus_connection_call_sync(
      bus, item_name, "/MenuBar", "com.canonical.dbusmenu", "GetLayout",
      g_variant_new("(ii@as)", 0, -1, g_variant_new_strv(NULL, 0)),
      NULL, G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &error);
  cr_assert_not_null(layout_reply);
  cr_assert_null(error);
  g_variant_unref(layout_reply);
  tray_set_progress(12, 24);
  g_atomic_int_set(&menu_clicks, 0);
  TrayMenuItem menu = {.label = "Quit", .enabled = TRUE,
                       .activate = count_menu_click};
  tray_set_menu(&menu, 1);
  gint menu_id = -1;
  for (int attempt = 0; attempt < 100 && menu_id < 0; ++attempt) {
    layout_reply = g_dbus_connection_call_sync(
        bus, item_name, "/MenuBar", "com.canonical.dbusmenu", "GetLayout",
        g_variant_new("(ii@as)", 0, -1, g_variant_new_strv(NULL, 0)),
        NULL, G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &error);
    cr_assert_not_null(layout_reply);
    GVariant *root = g_variant_get_child_value(layout_reply, 1);
    GVariant *children = g_variant_get_child_value(root, 2);
    if (g_variant_n_children(children) > 0) {
      GVariant *wrapped = g_variant_get_child_value(children, 0);
      GVariant *child = g_variant_get_variant(wrapped);
      GVariant *id = g_variant_get_child_value(child, 0);
      menu_id = g_variant_get_int32(id);
      g_variant_unref(id);
      g_variant_unref(child);
      g_variant_unref(wrapped);
    }
    g_variant_unref(children);
    g_variant_unref(root);
    g_variant_unref(layout_reply);
    if (menu_id < 0)
      g_usleep(10000);
  }
  cr_assert_geq(menu_id, 0);
  GVariant *event_reply = g_dbus_connection_call_sync(
      bus, item_name, "/MenuBar", "com.canonical.dbusmenu", "Event",
      g_variant_new("(isvu)", menu_id, "clicked", g_variant_new_string(""),
                    0u), NULL, G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &error);
  cr_assert_not_null(event_reply);
  g_variant_unref(event_reply);
  for (int attempt = 0; attempt < 100 && !g_atomic_int_get(&menu_clicks);
       ++attempt)
    g_usleep(10000);
  cr_assert_eq(g_atomic_int_get(&menu_clicks), 1);
  g_free(item_name);
  g_object_unref(bus);
  tray_shutdown();
  g_main_context_invoke(watcher.context, stop_fake_watcher, watcher.loop);
  g_thread_join(thread);
  g_test_dbus_down(session);
  g_object_unref(session);
}
#endif

Test(tray, missing_session_bus_is_nonfatal_and_reentrant) {
  cr_assert_eq(setenv("DBUS_SESSION_BUS_ADDRESS",
                      "unix:path=/tmp/cdm-tray-no-session-bus", 1), 0);
  cr_assert_eq(tray_init("folder-download"), -1);
  tray_set_progress(100, 200);
  TrayMenuItem item = {.label = "Quit", .enabled = true};
  tray_set_menu(&item, 1);
  tray_shutdown();
  cr_assert_eq(tray_init("folder-download"), -1);
  tray_shutdown();
}
