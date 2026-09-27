const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");

async function verify(file, globalName, expectedBrowser) {
  let downloadListener;
  let actionListener;
  let headerListener;
  let headerOptions;
  let nativeListener;
  let disconnectListener;
  let installListener;
  let menuListener;
  const menuItems = [];
  const messages = [];
  const canceled = [];
  const badges = [];
  let granted = true;
  let filterSettings;
  let storageFails = false;
  let cookieReads = 0;
  let cookieRows = [{name: "session", value: "fixture"}];
  const permissionRequests = [];
  const port = {
    postMessage(message) { messages.push(message); },
    onMessage: { addListener(listener) { nativeListener = listener; } },
    onDisconnect: { addListener(listener) { disconnectListener = listener; } }
  };
  const api = {
    storage: {sync: {get() {
      return storageFails ? Promise.reject(new Error("fixture storage failure"))
        : Promise.resolve({interceptionFilters: filterSettings});
    }}},
    downloads: {
      onCreated: { addListener(listener) { downloadListener = listener; } },
      cancel(id) { canceled.push(id); return Promise.resolve(); }
    },
    runtime: { onInstalled: {addListener(listener) { installListener = listener; }},
      connectNative(name) {
      assert.equal(name, "org.cdm.browser");
      return port;
    } },
    contextMenus: {
      removeAll(callback) { menuItems.length = 0; callback?.(); return Promise.resolve(); },
      create(item, callback) { menuItems.push(item); callback?.(); return item.id; },
      onClicked: {addListener(listener) { menuListener = listener; }}
    },
    action: {
      onClicked: { addListener(listener) { actionListener = listener; } },
      setBadgeText(value) { badges.push(value.text); },
      setBadgeBackgroundColor() {},
      setTitle() {}
    },
    permissions: {
      request(request) { permissionRequests.push(request);
        return Promise.resolve(granted); },
      contains() { return Promise.resolve(granted); },
      remove() { return Promise.resolve(true); }
    },
    cookies: {
      getAll() { cookieReads++; return Promise.resolve(cookieRows); }
    },
    webRequest: { onSendHeaders: { addListener(listener, _filter, options) {
      headerListener = listener;
      headerOptions = options;
    } } }
  };
  const context = {
    [globalName]: api,
    crypto: { randomUUID: () => "browser-test-request" },
    console, URL, TextEncoder,
    importScripts(name) {
      vm.runInContext(fs.readFileSync(path.join(path.dirname(file), name), "utf8"), sandbox);
    }
  };
  const sandbox = vm.createContext(context);
  if (globalName === "browser")
    vm.runInContext(fs.readFileSync(path.join(path.dirname(file), "filters.js"), "utf8"), sandbox);
  vm.runInContext(fs.readFileSync(file, "utf8"), sandbox);
  assert.equal(typeof downloadListener, "function");
  assert.equal(typeof actionListener, "function");
  assert.equal(typeof headerListener, "function");
  assert.equal(typeof installListener, "function");
  assert.equal(typeof menuListener, "function");
  await installListener();
  assert.equal(menuItems.length, 2);
  assert.deepEqual(Array.from(menuItems[0].contexts), ["link"]);
  assert.deepEqual(Array.from(menuItems[1].contexts), ["page"]);
  assert.deepEqual(Array.from(menuItems[0].targetUrlPatterns),
                   ["http://*/*", "https://*/*"]);
  await installListener();
  assert.equal(menuItems.length, 2, "install/update must replace menu items");
  assert(headerOptions.includes("requestHeaders"));
  assert.equal(headerOptions.includes("extraHeaders"),
               expectedBrowser === "chromium");
  await downloadListener({ id: 7, state: "in_progress",
    url: "https://example.invalid/file.zip", filename: "/tmp/file.zip",
    totalBytes: 120, referrer: "https://example.invalid/" });
  assert.deepEqual(canceled, [7]);
  assert.equal(messages.length, 1);
  assert.equal(messages[0].type, "download_offer");
  assert.equal(messages[0].filename, "file.zip");
  assert.equal(messages[0].browser, expectedBrowser);
  assert.equal(messages[0].request_id, "browser-test-request");
  assert.equal(permissionRequests.length, 0);
  assert.equal(cookieReads, 0);
  assert.equal(messages[0].cookie, undefined);
  assert.equal(messages[0].user_agent, undefined);
  assert.equal(messages[0].referer, undefined);
  assert.equal(messages[0].referrer, "");
  await actionListener({url: "https://example.invalid/page"});
  assert.equal(permissionRequests.length, 1);
  assert.equal(permissionRequests[0].origins[0], "https://example.invalid/*");
  headerListener({url: "https://example.invalid/private.zip",
    requestHeaders: [{name: "User-Agent", value: "Browser UA"},
      {name: "Referer", value: "https://example.invalid/page"}]});
  await downloadListener({id: 9, state: "in_progress",
    url: "https://example.invalid/private.zip", filename: "private.zip"});
  assert.equal(messages.at(-1).cookie, "session=fixture");
  assert.equal(messages.at(-1).user_agent, "Browser UA");
  assert.equal(messages.at(-1).referer, "https://example.invalid/page");
  assert.equal(messages.at(-1).referrer, "https://example.invalid/page");
  assert.equal(cookieReads, 1);
  headerListener({url: "https://example.invalid/other.zip",
    requestHeaders: [{name: "User-Agent", value: "Wrong URL"}]});
  await downloadListener({id: 12, state: "in_progress",
    url: "https://example.invalid/no-header.zip"});
  assert.equal(messages.at(-1).user_agent, undefined);
  cookieRows = [{name: "session", value: "x".repeat(4097)}];
  headerListener({url: "https://example.invalid/unsafe.zip",
    requestHeaders: [{name: "User-Agent", value: "bad\r\nheader"},
      {name: "Referer", value: "x".repeat(2049)}]});
  await downloadListener({id: 13, state: "in_progress",
    url: "https://example.invalid/unsafe.zip"});
  assert.equal(messages.at(-1).cookie, undefined);
  assert.equal(messages.at(-1).user_agent, undefined);
  assert.equal(messages.at(-1).referer, undefined);
  await downloadListener({id: 14, state: "in_progress", incognito: true,
    url: "https://example.invalid/incognito.zip"});
  assert.equal(messages.at(-1).cookie, undefined);
  assert.equal(cookieReads, 3);
  granted = false; // Browser permissions revoked while the toggle is on.
  await downloadListener({id: 15, state: "in_progress",
    url: "https://example.invalid/revoked.zip"});
  assert.equal(messages.at(-1).cookie, undefined);
  assert.equal(cookieReads, 3);
  granted = true;
  await actionListener({url: "https://example.invalid/page"});
  await downloadListener({id: 10, state: "in_progress",
    url: "https://example.invalid/off.zip"});
  assert.equal(messages.at(-1).cookie, undefined);
  assert.equal(cookieReads, 3);
  granted = false;
  await actionListener({url: "https://denied.invalid/page"});
  await downloadListener({id: 11, state: "in_progress",
    url: "https://denied.invalid/file.zip"});
  assert.equal(messages.at(-1).cookie, undefined);
  await downloadListener({ id: 8, state: "in_progress", url: "blob:unsupported" });
  assert.equal(messages.length, 8);
  const cancelCount = canceled.length;
  await menuListener({menuItemId: "cdm-download-link",
    linkUrl: "https://example.invalid/manual.zip",
    pageUrl: "https://example.invalid/page"}, {incognito: false});
  assert.equal(messages.at(-1).url, "https://example.invalid/manual.zip");
  assert.equal(messages.at(-1).type, "download_offer");
  assert.equal(messages.at(-1).total_bytes, 0);
  assert.equal(messages.at(-1).cookie, undefined);
  await menuListener({menuItemId: "cdm-download-page",
    pageUrl: "https://example.invalid/page"}, {});
  assert.equal(messages.at(-1).url, "https://example.invalid/page");
  assert.equal(messages.at(-1).filename, "");
  assert.equal(canceled.length, cancelCount, "explicit offers must not cancel downloads");
  const offered = messages.length;
  await menuListener({menuItemId: "cdm-download-link", linkUrl: "javascript:alert(1)"}, {});
  await menuListener({menuItemId: "unknown", linkUrl: "https://example.invalid/file"}, {});
  assert.equal(messages.length, offered);
  granted = true;
  cookieRows = [{name: "session", value: "manual-fixture"}];
  await actionListener({url: "https://example.invalid/page"});
  await menuListener({menuItemId: "cdm-download-link",
    linkUrl: "https://example.invalid/session.zip",
    pageUrl: "https://example.invalid/page"}, {});
  assert.equal(messages.at(-1).cookie, "session=manual-fixture");
  assert.equal(messages.at(-1).referer, "https://example.invalid/page");
  await menuListener({menuItemId: "cdm-download-page",
    pageUrl: "https://example.invalid/private"}, {incognito: true});
  assert.equal(messages.at(-1).cookie, undefined);
  nativeListener({ type: "error", request_id: "browser-test-request",
    error: "host unavailable" });
  assert.equal(badges.at(-1), "!");
  nativeListener({ type: "offer_registered", request_id: "browser-test-request" });
  assert.equal(badges.at(-1), "ON");
  assert.equal(typeof disconnectListener, "function");
  async function automatic(item, expected) {
    const before = messages.length;
    const cancels = canceled.length;
    await downloadListener({id: 30, state: "in_progress",
      url: "https://example.invalid/file.zip", totalBytes: 128, ...item});
    assert.equal(messages.length - before, expected ? 1 : 0, JSON.stringify(item));
    assert.equal(canceled.length - cancels, expected ? 1 : 0);
    if (!expected) assert.equal(badges.at(-1), "SKIP");
  }
  filterSettings = {minSizeBytes: 128};
  await automatic({totalBytes: 127}, false);
  await automatic({totalBytes: 128}, true);
  await automatic({totalBytes: -1}, false);
  await automatic({totalBytes: 0}, false);
  filterSettings = {extensionsAllow: [".ZIP", "tar.gz"], mimeAllow: ["application/pdf"],
    extensionsDeny: ["exe"], mimeDeny: ["video/*"]};
  await automatic({filename: "file.ZIP"}, true);
  await automatic({url: "https://example.invalid/file%2Etar.gz?ignore=.exe"}, true);
  await automatic({filename: "file.pdf", mime: "Application/PDF; charset=utf-8"}, true);
  await automatic({filename: "file.exe", mime: "application/pdf"}, false);
  await automatic({filename: "file.zip", mime: "video/mp4"}, false);
  await automatic({filename: "file.txt", mime: "text/plain"}, false);
  const reads = cookieReads;
  filterSettings = {minSizeBytes: 1000000, extensionsDeny: ["zip"]};
  await automatic({}, false);
  assert.equal(cookieReads, reads, "skipped downloads must not collect context");
  const manual = messages.length;
  await menuListener({menuItemId: "cdm-download-link",
    linkUrl: "https://example.invalid/explicit.zip"}, {});
  assert.equal(messages.length, manual + 1, "manual choices bypass filters");
  storageFails = true;
  await automatic({}, false);
  storageFails = false;
  filterSettings = {minSizeBytes: -1};
  await automatic({}, false);
  filterSettings = undefined;
  await automatic({totalBytes: -1}, true);
}

async function verifyOptions(file, globalName) {
  const directory = path.dirname(file);
  const elements = {};
  for (const id of ["filters", "status", "save", "minSizeBytes", "extensionsAllow",
                    "extensionsDeny", "mimeAllow", "mimeDeny"])
    elements[id] = {value: "", textContent: "", disabled: false};
  let submit;
  let saved;
  let failSave = false;
  elements.filters.addEventListener = (name, listener) => {
    assert.equal(name, "submit"); submit = listener;
  };
  const context = vm.createContext({TextEncoder, URL,
    document: {getElementById(id) { assert(elements[id], id); return elements[id]; }},
    [globalName]: {storage: {sync: {
      get() { return Promise.resolve({interceptionFilters: {minSizeBytes: 7,
          extensionsAllow: ["PDF"]}}); },
      set(value) { if (failSave) return Promise.reject(new Error("fixture quota failure"));
        saved = JSON.parse(JSON.stringify(value)); return Promise.resolve(); }
    }}}
  });
  vm.runInContext(fs.readFileSync(path.join(directory, "filters.js"), "utf8"), context);
  vm.runInContext(fs.readFileSync(path.join(directory, "options.js"), "utf8"), context);
  await new Promise(setImmediate);
  assert.equal(elements.minSizeBytes.value, "7");
  assert.equal(elements.extensionsAllow.value, "pdf");
  assert.equal(elements.save.disabled, false);
  elements.minSizeBytes.value = "128";
  elements.extensionsAllow.value = ".ZIP, zip, tar.gz";
  elements.mimeDeny.value = "VIDEO/*";
  await submit({preventDefault() {}});
  assert.deepEqual(saved.interceptionFilters, {minSizeBytes: 128,
    extensionsAllow: ["zip", "tar.gz"], extensionsDeny: [],
    mimeAllow: [], mimeDeny: ["video/*"]});
  const previous = saved;
  for (const invalid of ["", "-1", "1.5", "9007199254740992"]) {
    elements.minSizeBytes.value = invalid;
    await submit({preventDefault() {}});
    assert.equal(saved, previous);
    assert.match(elements.status.textContent, /whole number/);
  }
  elements.minSizeBytes.value = "0";
  elements.mimeDeny.value = "regex:.*";
  await submit({preventDefault() {}});
  assert.equal(saved, previous);
  assert.match(elements.status.textContent, /MIME/);
  elements.mimeDeny.value = "";
  failSave = true;
  await submit({preventDefault() {}});
  assert.match(elements.status.textContent, /quota failure/);
  assert.equal(elements.save.disabled, false);
  const filters = context.CdmFilters;
  assert.throws(() => filters.normalize({extensionsAllow: Array(65).fill("zip")}), /64/);
  assert.throws(() => filters.normalize({mimeAllow: [3]}), /text/);
  assert.throws(() => filters.normalize({extensionsDeny: ["*"]}), /extension/);
  assert.throws(() => filters.normalize({mimeDeny: ["x".repeat(129) + "/pdf"]}), /too long/);
  assert.equal(filters.reason({url: "https://example.invalid/a", mime: "text/plain"},
    filters.normalize({mimeAllow: ["*/*"]})), "");
  assert.notEqual(filters.reason({url: "https://example.invalid/a"},
    filters.normalize({mimeAllow: ["*/*"]})), "");
}

for (const file of [process.argv[2], process.argv[3]]) {
  const manifest = JSON.parse(fs.readFileSync(
    path.join(path.dirname(file), "manifest.json"), "utf8"));
  assert(manifest.permissions.includes("activeTab"));
  assert(manifest.permissions.includes("contextMenus"));
  assert(manifest.permissions.includes("storage"));
  assert.equal(manifest.options_ui.page, "options.html");
  assert(fs.existsSync(path.join(path.dirname(file), "options.html")));
  assert.deepEqual(manifest.optional_permissions, ["cookies", "webRequest"]);
  assert(manifest.optional_host_permissions.includes("https://*/*"));
}
Promise.all([
  verify(process.argv[2], "chrome", "chromium"),
  verify(process.argv[3], "browser", "firefox"),
  verifyOptions(process.argv[2], "chrome"),
  verifyOptions(process.argv[3], "browser")
]).catch(error => { console.error(error); process.exitCode = 1; });
