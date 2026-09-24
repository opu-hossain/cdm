# Daemon startup at login

The DEB, RPM, Arch, and system-wide source install put
`cdm-daemon.desktop` in `/etc/xdg/autostart`. A graphical session that
implements XDG autostart starts `cdm daemon` at the next login. This starts
the background download service, not the GUI. Package installation does not
start a daemon in an existing session or change any user's home directory.

The entry uses the installed binary's absolute path. A package configured
for `/usr` uses `/usr/bin/cdm`; a source build configured for `/usr/local`
uses `/usr/local/bin/cdm`. A source install needs permission to install the
system-wide entry in `/etc/xdg/autostart`.

## Control it for your account

Run these as your normal user, without `sudo`:

```sh
cdm daemon status
cdm daemon disable
cdm daemon enable
```

`status` reports the effective XDG entry and the serving daemon's PID, or
`Daemon: not running` when no daemon is reachable. `disable` prevents future
automatic starts at graphical login by
creating `$XDG_CONFIG_HOME/autostart/cdm-daemon.desktop` with `Hidden=true`
(by default, `~/.config/autostart/cdm-daemon.desktop`). `enable` removes the
override made by `cdm` or changes `Hidden` to false in a customized user
entry while keeping its other keys. Repeating either command is safe.

Successful `enable` and `disable` commands print the resulting autostart
state and the entry they wrote, updated, or removed. If a usable system entry
already enables autostart, `enable` reports that entry and says no file was
changed. The file path in this output shows where the current user's setting
lives.

`cdm daemon` stays in the foreground when started from a terminal. If a daemon
is already serving, it prints `cdm daemon: already running (pid PID)` to
stderr and exits with code 1. Lifecycle commands return 0 on success, 1 for
an already running daemon or other operational failure, and 2 for an invalid
`cdm daemon` argument. Errors and usage go to stderr; status and autostart
confirmations go to stdout.

Disabling autostart does not stop a daemon that is already running or cancel
downloads. It also does not disable on-demand startup: opening the GUI or
CLI, or sending a download from the browser native host, can start the
daemon when needed. `cdm daemon` continues to work manually.

The package owns only the system-wide entry. On upgrade, a per-user
disable override stays in effect. Uninstall leaves user settings and
download data alone. Debian `remove` preserves the system-wide conffile
until `purge`; RPM and Arch may save a modified global entry. Any retained
entry will fail its `TryExec` check once the `cdm` binary is gone.

## Other sessions

Headless logins and minimal window managers without XDG autostart do not
start the daemon at login. Run `cdm daemon` manually, add it to your session
startup mechanism, or let the GUI, CLI, or browser native host start it on
demand. No systemd user manager is required for the XDG entry or manual
daemon command.

The daemon takes a per-user advisory lock before opening its database or
IPC listener. If two login sessions or an on-demand request start it at the
same time, only one process becomes the serving daemon.

The IPC listener is `$XDG_RUNTIME_DIR/cdm.sock` when that variable names an
absolute path. Without it, the listener uses
`~/.local/share/cdm/ipc.sock`; without a usable home directory, it falls
back to `/tmp/cdm_UID.sock` for the numeric user ID. The lock follows the
same location choice, using `cdm.lock` beside the runtime socket or in
`~/.local/share/`, and `/tmp/cdm_UID.lock` for the final fallback.
