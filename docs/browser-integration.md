# Browser integration

cdm offers HTTP(S) browser downloads to a native host and a confirmation popup.
Automatic interception applies the extension's filters and exclusions first;
only allowed downloads are canceled in the browser. Cancellation is best effort
and can leave a partial browser file. Link/page **Download with cdm** menu choices
use the same confirmation flow without canceling browser downloads.

URL-only offers are the default. Click the extension action on a site to enable
browser-session capture for that origin; the browser requests optional cookie,
webRequest, and site permissions. Click again to disable it. Cookie, User-Agent,
and Referer capture is bounded and best effort; ambiguous request correlation,
incognito, or unavailable permissions omit context. Authorization headers,
POST bodies, Blob/data URLs, and browser-internal URLs are unsupported.

Captured values stay in memory and are not saved in SQLite, logs, or sync
settings. A persisted presence marker makes affected downloads require a fresh
confirmed browser offer after daemon restart. Captured context is not forwarded
through redirects; re-offer the final URL. The popup reports header presence
without exposing values. See [browser-context.md](browser-context.md).

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

After rebuilding and reinstalling cdm from source, reload the unpacked Chromium
extension in `chrome://extensions` or `chromium://extensions`. For a Firefox
temporary add-on, remove it and load its installed `manifest.json` again in
`about:debugging`. If a download stays in the browser without a cdm badge or
popup, inspect the extension's background/service-worker console first: a
startup error can prevent the download listener from registering. The optional
`webRequest` API is used only for consented header capture and media detection;
ordinary download handoff does not require granting it.

## Install Edge, Brave, Opera, or Vivaldi

Load the same Chromium extension directory through the browser's extensions
page, then pass its actual 32-character extension ID:

```sh
cdm browser install --edge --id <extension-id>
cdm browser install --brave --id <extension-id>
cdm browser install --opera --id <extension-id>
cdm browser install --vivaldi --id <extension-id>
```

Start the stable browser once before registering: these flags require its
existing default config directory. The manifest paths below are relative to
HOME and use the file name `org.cdm.browser.json`:

| Flag | Required config directory | Manifest directory |
|---|---|---|
| `--edge` | `.config/microsoft-edge` | `.config/microsoft-edge/NativeMessagingHosts` |
| `--brave` | `.config/BraveSoftware/Brave-Browser` | `.config/BraveSoftware/Brave-Browser/NativeMessagingHosts` |
| `--opera` | `.config/opera` | `.config/google-chrome/NativeMessagingHosts` |
| `--vivaldi` | `.config/vivaldi` | `.config/vivaldi/NativeMessagingHosts` |

Opera shares Chrome's registration. Installing or uninstalling that entry also
affects Chrome's cdm host registration; install again with the desired extension
ID if switching browsers. Custom/XDG profile roots, beta/dev channels, Flatpak,
and Snap registration are not covered by these flags. See installer source
comments for the researched path references. Real browser handshakes remain
unverified for the four added flags in this environment.

## Automatic interception settings

Open the extension's options page. `storage.sync` stores configuration only:

- Minimum file size is a nonnegative integer in **bytes**, default 0. Unknown
  sizes remain in the browser when a positive minimum is set.
- Extension and MIME allow/deny lists are comma-separated, case-insensitive,
  and default empty. Extension tokens omit the dot; MIME supports exact types,
  `type/*`, and `*/*`. Deny matches win. When allowlists exist, matching either
  list allows the download. Each list accepts at most 64 entries.
- Site exclusions accept ASCII/punycode hostnames and a leading `*.` wildcard;
  no regular expressions, URLs, or ports. Exact hosts match only that host;
  `*.example.invalid` matches both the base host and its subdomains, using label
  boundaries. At most 64 entries are accepted.

Explicit menu choices bypass automatic filters and exclusions. Invalid settings
or read failures leave automatic downloads in the browser. A `SKIP` badge/title
explains a bypass. Options writes are validated before saving; failed writes are
reported. Filter/site JSON is separately capped at 6000 UTF-8 bytes.

The extension sends `{type:"set_site_exclusions",sites:[...]}` before each native
offer, including a new host connection's first offer. The host validates and
acknowledges it with `site_exclusions_set`; invalid updates preserve its prior
policy. Offers carry `automatic:true` for interception or `false` for menu
choices (missing mode defaults to automatic). Excluded automatic offers return
`offer_skipped` with the request ID before daemon/popup startup. This policy is
independent of the daemon IPC protocol.

## Refresh a download URL

Use **Refresh URL** in a non-active row menu or a stopped browser popup. With
protocol v9, cdm probes the stored URL with its request options and saves the
final redirected URL, size, ETag, and Last-Modified together. It does not resume
the transfer automatically. Probe/database failure leaves the record unchanged.
Active downloads, lost browser context, concurrent changes, and size/validator
changes for an existing partial download are refused. Changed partial content
requires re-downloading. This follows existing redirects; it cannot obtain a
new expired signed URL from a site or recreate browser login state.

## Remove browser integration

Disable or remove the extension in the browser, then remove its host manifest:

```sh
cdm browser uninstall --chrome
cdm browser uninstall --chromium
cdm browser uninstall --firefox
cdm browser uninstall --edge
cdm browser uninstall --brave
cdm browser uninstall --opera
cdm browser uninstall --vivaldi
```

The selected per-user `org.cdm.browser.json` manifest is removed; Opera and
Chrome share that entry. Existing downloads and the cdm daemon are unaffected. Package
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

Required permissions are `downloads`, `nativeMessaging`, `activeTab`,
`contextMenus`, and `storage`. Optional `cookies`, `webRequest`, and HTTP(S)
host access are requested only by the site-specific user gesture. Extension
context enablement and correlated headers are session memory, not sync settings.

## Phase 3 verification status

On 2026-09-28, the current Debug and Release suites each pass 41/41 targets,
including Node extension/options tests, native-host JSON/context tests, local
HTTP/IPC refresh tests, and isolated registration tests for all seven flags.
The staged native-host binary also passes its protocol smoke. Chrome/Chromium
and Firefox are the release browser targets; Edge, Brave, Opera, and Vivaldi
registration flags are shipped but their extension smoke is deferred by user
decision. Installed-browser visual/handshake smoke for Chromium and Firefox
remains a manual release acceptance check: load each extension, register its
host, verify confirmation and progress, test consent on/off,
filters/exclusions and explicit menu bypass, and verify refresh success/failure
against a local HTTP fixture.
