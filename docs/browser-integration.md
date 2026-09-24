# Browser integration

cdm can receive HTTP(S) browser downloads from Chrome, Chromium, or Firefox.
The extension cancels the browser download when it is created, sends its URL to
the native host, and opens a compact cdm confirmation window. After you choose
a filename and folder, cdm shows progress and offers Open file, Open folder,
and Close on completion.

Browser cancellation is best effort. The browser can write a partial file
before the extension sees `downloads.onCreated`. URL-only GET downloads are
supported in this release. Downloads that need browser cookies or authorization,
POST bodies, Blob/data URLs, or browser-internal URLs cannot be handed off
reliably. The extension does not forward cookies or credentials.

## Install Chrome or Chromium

1. Install cdm from a DEB, RPM, Arch package, or CMake install. Both `cdm` and
   `cdm_native_host` must be executable in the same binary directory.
2. Open `chrome://extensions` or `chromium://extensions`, enable Developer mode,
   and choose **Load unpacked**. Select the installed
   `share/cdm/browser/chromium` directory (`/usr/share/cdm/browser/chromium`
   for distro packages or `/usr/local/share/cdm/browser/chromium` for a
   default source install). The source tree's `browser/chromium` directory
   also works for development.
3. Copy the 32-character extension ID shown by the browser. Register the host:

   ```sh
   cdm browser install --chrome --id <extension-id>
   # or: cdm browser install --chromium --id <extension-id>
   ```

4. Try an ordinary HTTP(S) download. The native host starts the cdm daemon if
   needed.

The command writes `org.cdm.browser.json` under
`~/.config/google-chrome/NativeMessagingHosts/` for Chrome or
`~/.config/chromium/NativeMessagingHosts/` for Chromium. It records the exact
extension ID and absolute path to `cdm_native_host`. If you move the unpacked
extension and its ID changes, run the install command again with the new ID.

## Install Firefox

The Firefox MV3 extension uses the fixed Gecko ID `browser@cdm.local`. For
development, open `about:debugging#/runtime/this-firefox`, choose **Load
Temporary Add-on**, and select the installed
`share/cdm/browser/firefox/manifest.json` file (or the source-tree copy). Then
register its host:

```sh
cdm browser install --firefox --id browser@cdm.local
```

Firefox writes the host manifest under
`~/.mozilla/native-messaging-hosts/`. Temporary add-ons are removed on Firefox
restart, so reload the extension after restarting. Persistent Firefox
installation requires a signed extension package; `cdm-firefox.zip` is an
unsigned build artifact for signing and testing.

## Remove browser integration

Disable or remove the extension in the browser, then remove its host manifest:

```sh
cdm browser uninstall --chrome
cdm browser uninstall --chromium
cdm browser uninstall --firefox
```

Only the selected browser's per-user `org.cdm.browser.json` manifest is
removed. Existing downloads and the cdm daemon are unaffected. Package
installation never edits a browser profile automatically.

## Troubleshooting

- **Red `!` badge or no confirmation window:** click the extension button to
  read its error title and inspect its background/service-worker console. Make
  sure `cdm_native_host` exists beside `cdm`, and
  the host manifest path and extension ID match the installed extension.
- **Host not found or access denied:** rerun `cdm browser install` with the
  correct browser flag and ID. Restart the browser after registering the host.
- **Popup cannot open:** cdm needs a graphical desktop session for SDL2. Run
  the daemon and browser as the same user and check
  `~/.local/share/cdm/daemon.log` for IPC or download errors.
- **Browser still has a partial file:** the browser may start writing before
  cancellation completes. Remove that partial browser file if it remains;
  cdm's chosen destination is separate.
- **Download fails after confirmation:** the URL may require browser-only
  credentials, a POST body, or a Blob URL. Try the regular browser download
  for that file, or use cdm's normal GUI/CLI for a transferable URL.
- **Firefox extension disappears:** temporary add-ons expire when Firefox
  restarts. Reload it through `about:debugging` or install a signed package.

The extension asks for `downloads` to observe/cancel browser downloads and
`nativeMessaging` to contact the locally installed cdm host. It sends the URL,
referrer URL, suggested filename, MIME type, and reported size to cdm; it does
not send cookies, authorization headers, or request bodies.
