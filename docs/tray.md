# Linux tray smoke checklist

Backend: StatusNotifierItem over the session D-Bus, with a DBusMenu exported
by `libdbusmenu-glib`. This avoids a GTK runtime in the daemon. The library is
optional at build time; when unavailable, the tray API returns a nonfatal
failure. The daemon must also continue when no session bus or tray host exists.

1. Start a desktop session with a StatusNotifierHost and `cdm daemon`. Confirm
   the icon appears once, with a tooltip showing cdm progress.
2. Open its menu and activate Pause all, Resume all, and Quit. Confirm each
   action affects the daemon once, with no shell launch.
3. Start a download and confirm the tooltip advances, then shows idle after
   completion. Close the daemon and confirm the icon disappears.
4. Run the daemon with `DBUS_SESSION_BUS_ADDRESS` pointing to a nonexistent
   socket. Confirm it stays alive, downloads work, and no tray appears.
5. Rebuild with `-DDOWNLOADMGR_ENABLE_TRAY=OFF`. Confirm the build succeeds
   and the daemon behaves as in step 4.

Steps 1–3 cover daemon wiring in task 2.4.2. The backend alone exposes the
menu and progress API but is not yet called by the daemon.

Manual desktop/host verification: **UNKNOWN** in the current headless
workspace. Run these steps in a graphical Linux session before marking tray
integration complete.

Automated local coverage: `test_tray` starts a private D-Bus session with a
fake StatusNotifierWatcher, checks registration and SNI/DBusMenu replies,
activates a menu item over D-Bus, and checks the missing-session-bus failure
path. It does not render a real tray.

Protocol references: [StatusNotifierItem](https://specifications.freedesktop.org/status-notifier-item/latest-single/),
[GDBus object export](https://docs.gtk.org/gio/method.DBusConnection.register_object.html),
[libdbusmenu server](https://sources.debian.org/data/main/libd/libdbusmenu/12.10.2-2/docs/libdbusmenu-glib/reference/html/libdbusmenu-glib-DbusmenuServer.html).
