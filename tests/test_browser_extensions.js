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
  const messages = [];
  const canceled = [];
  const badges = [];
  let granted = true;
  let cookieReads = 0;
  let cookieRows = [{name: "session", value: "fixture"}];
  const permissionRequests = [];
  const port = {
    postMessage(message) { messages.push(message); },
    onMessage: { addListener(listener) { nativeListener = listener; } },
    onDisconnect: { addListener(listener) { disconnectListener = listener; } }
  };
  const api = {
    downloads: {
      onCreated: { addListener(listener) { downloadListener = listener; } },
      cancel(id) { canceled.push(id); return Promise.resolve(); }
    },
    runtime: { connectNative(name) {
      assert.equal(name, "org.cdm.browser");
      return port;
    } },
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
    console, URL, TextEncoder
  };
  vm.runInNewContext(fs.readFileSync(file, "utf8"), context);
  assert.equal(typeof downloadListener, "function");
  assert.equal(typeof actionListener, "function");
  assert.equal(typeof headerListener, "function");
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
  nativeListener({ type: "error", request_id: "browser-test-request",
    error: "host unavailable" });
  assert.equal(badges.at(-1), "!");
  nativeListener({ type: "offer_registered", request_id: "browser-test-request" });
  assert.equal(badges.at(-1), "");
  assert.equal(typeof disconnectListener, "function");
}

for (const file of [process.argv[2], process.argv[3]]) {
  const manifest = JSON.parse(fs.readFileSync(
    path.join(path.dirname(file), "manifest.json"), "utf8"));
  assert(manifest.permissions.includes("activeTab"));
  assert.deepEqual(manifest.optional_permissions, ["cookies", "webRequest"]);
  assert(manifest.optional_host_permissions.includes("https://*/*"));
}
Promise.all([
  verify(process.argv[2], "chrome", "chromium"),
  verify(process.argv[3], "browser", "firefox")
]).catch(error => { console.error(error); process.exitCode = 1; });
