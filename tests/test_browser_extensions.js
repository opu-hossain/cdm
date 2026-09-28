const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");

async function verify(file, globalName, expectedBrowser) {
  let downloadListener;
  let actionListener;
  let headerListener;
  let mediaListener;
  let runtimeListener;
  let mediaDetection;
  let mediaSequence = 0;
  let clock = Date.now();
  let storageListener;
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
  let siteExclusions;
  const configurations = [];
  const wireMessages = [];
  let storageFails = false;
  let cookieReads = 0;
  let cookieRows = [{name: "session", value: "fixture"}];
  const permissionRequests = [];
  const port = {
    postMessage(message) {
      wireMessages.push(message);
      if (message.type === "set_site_exclusions") configurations.push(message);
      else messages.push(message);
    },
    onMessage: { addListener(listener) { nativeListener = listener; } },
    onDisconnect: { addListener(listener) { disconnectListener = listener; } }
  };
  const api = {
    storage: {onChanged: {addListener(listener) { storageListener = listener; }}, sync: {get() {
      return storageFails ? Promise.reject(new Error("fixture storage failure"))
        : Promise.resolve({interceptionFilters: filterSettings, siteExclusions, mediaDetection});
    }}},
    downloads: {
      onCreated: { addListener(listener) { downloadListener = listener; } },
      cancel(id) { canceled.push(id); return Promise.resolve(); }
    },
    runtime: {getURL: name => `extension://cdm/${name}`,
      onMessage: {addListener(listener) { runtimeListener = listener; }}, onInstalled: {addListener(listener) { installListener = listener; }},
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
    webRequest: {onHeadersReceived: {addListener(listener, filter, options) {
      mediaListener = listener;
      assert.deepEqual(Array.from(options), ["responseHeaders"]);
    }}, onSendHeaders: { addListener(listener, _filter, options) {
      headerListener = listener;
      headerOptions = options;
    } } }
  };
  const context = {
    [globalName]: api,
    crypto: { randomUUID: () => mediaDetection ? `media-${++mediaSequence}` : "browser-test-request" },
    Date: {now: () => clock},
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
  siteExclusions = ["*.EXAMPLE.invalid."];
  const contextReads = cookieReads;
  await automatic({}, false);
  await automatic({url: "https://sub.example.invalid/file.zip"}, false);
  assert.equal(cookieReads, contextReads);
  await automatic({url: "https://notexample.invalid/file.zip"}, true);
  await automatic({url: "https://example.invalid.evil.invalid/file.zip"}, true);
  const explicit = messages.length;
  await menuListener({menuItemId: "cdm-download-link",
    linkUrl: "https://example.invalid/manual.zip"}, {});
  assert.equal(messages.length, explicit + 1);
  assert.equal(messages.at(-1).automatic, false);
  assert.equal(configurations.at(-1).sites[0], "*.example.invalid");
  assert.equal(wireMessages[0].type, "set_site_exclusions");
  assert.equal(wireMessages.at(-2).type, "set_site_exclusions");
  siteExclusions = ["sub.example.invalid"];
  await automatic({}, true);
  assert.equal(messages.at(-1).automatic, true);
  await automatic({url: "https://sub.example.invalid/file.zip"}, false);
  siteExclusions = [".*regex.invalid"];
  await automatic({}, false);
  assert.equal(typeof mediaListener, "function");
  assert.equal(typeof runtimeListener, "function");
  siteExclusions = undefined;
  filterSettings = undefined;
  const response = {url: "https://example.invalid/master.M3U8?fixture=1", tabId: 7,
    method: "GET", statusCode: 200, responseHeaders: []};
  const beforeMedia = messages.length;
  const beforeMediaCancel = canceled.length;
  function callRuntime(message, url = "extension://cdm/options.html") {
    return new Promise(resolve => {
      const keep = runtimeListener(message, {url}, resolve);
      if (keep !== true) resolve(undefined);
    });
  }
  await mediaListener(response);
  assert.equal((await callRuntime({type: "cdm_media_list"})).length, 0);
  mediaDetection = true;
  await mediaListener(response);
  await mediaListener(response);
  let candidates = await callRuntime({type: "cdm_media_list"});
  assert.equal(candidates.length, 1);
  assert.equal(candidates[0].kind, "hls");
  assert.equal(messages.length, beforeMedia, "detection must not open native offers");
  assert(!Object.hasOwn(candidates[0], "cookie"));
  // Existing site-specific consent is on; turning it off clears cached context.
  await actionListener({url: "https://example.invalid/page"});
  assert.equal(await callRuntime({type: "cdm_media_list"}, "https://example.invalid/"), undefined);
  const selected = await callRuntime({type: "cdm_media_offer", id: candidates[0].id});
  assert.equal(selected.ok, true);
  assert.equal(messages.at(-1).type, "media_offer");
  assert.equal(messages.at(-1).kind, "hls");
  assert.equal(messages.at(-1).automatic, false);
  assert.equal(messages.at(-1).cookie, undefined);
  assert.equal(canceled.length, beforeMediaCancel, "media selection must not cancel downloads");
  assert.equal((await callRuntime({type: "cdm_media_list"})).length, 0);
  await mediaListener({...response, url: "https://example.invalid/manifest",
    responseHeaders: [{name: "Content-Type", value: "Application/Dash+XML; charset=utf-8"}]});
  candidates = await callRuntime({type: "cdm_media_list"});
  assert.equal(candidates[0].kind, "dash");
  await mediaListener({...response, url: "https://example.invalid/movie.mp4"});
  await mediaListener({...response, url: "https://example.invalid/stream",
    responseHeaders: [{name: "content-type", value: "video/webm"}]});
  assert.equal((await callRuntime({type: "cdm_media_list"})).length, 3);
  for (const item of [{...response, incognito: true}, {...response, statusCode: 404},
      {...response, method: "POST"}, {...response, tabId: -1},
      {...response, url: "blob:https://example.invalid/fixture"}])
    await mediaListener(item);
  assert.equal((await callRuntime({type: "cdm_media_list"})).length, 3);
  siteExclusions = ["*.example.invalid"];
  await mediaListener({...response, url: "https://example.invalid/excluded.mpd"});
  assert.equal((await callRuntime({type: "cdm_media_list"})).length, 3);
  siteExclusions = undefined;
  for (let i = 0; i < 80; i++)
    await mediaListener({...response, url: `https://example.invalid/segment-${i}.mp4`});
  candidates = await callRuntime({type: "cdm_media_list"});
  assert.equal(candidates.length, 64);
  assert(candidates.some(c => c.kind === "dash"), "segments must not crowd out manifests");
  clock += 600001;
  assert.equal((await callRuntime({type: "cdm_media_list"})).length, 0);
  await mediaListener(response);
  mediaDetection = false;
  storageListener({mediaDetection: {newValue: false}}, "sync");
  assert.equal((await callRuntime({type: "cdm_media_list"})).length, 0);
  assert.equal((await callRuntime({type: "cdm_media_offer", id: candidates[0].id})).ok, false);

}

async function verifyOptions(file, globalName) {
  const directory = path.dirname(file);
  const elements = {};
  for (const id of ["filters", "status", "save", "minSizeBytes", "extensionsAllow",
                    "extensionsDeny", "mimeAllow", "mimeDeny", "siteExclusions", "mediaDetection",
                    "mediaCandidates", "mediaStatus", "mediaOffer", "mediaRefresh"])
    elements[id] = {value: "", textContent: "", disabled: false, checked: false,
      children: [], listeners: {}, addEventListener(name, listener) {this.listeners[name] = listener;}};
  elements.mediaCandidates.replaceChildren = () => {
    elements.mediaCandidates.children = []; elements.mediaCandidates.value = "";
  };
  elements.mediaCandidates.appendChild = option => {
    elements.mediaCandidates.children.push(option);
    elements.mediaCandidates.value ||= option.value;
  };
  let grantedMedia = true;
  const mediaPermissionRequests = [];
  let mediaRows = [{id: "media-option-fixture", kind: "hls", tabId: 7,
    url: "https://example.invalid/video.m3u8"}];
  let selectedMedia;
  let submit;
  let saved;
  let failSave = false;
  elements.filters.addEventListener = (name, listener) => {
    assert.equal(name, "submit"); submit = listener;
  };
  const context = vm.createContext({TextEncoder, URL,
    document: {getElementById(id) { assert(elements[id], id); return elements[id]; },
      createElement(tag) { assert.equal(tag, "option"); return {}; }},
    [globalName]: {permissions: {request(value) { mediaPermissionRequests.push(value);
      return Promise.resolve(grantedMedia); }}, runtime: {sendMessage(message) {
        if (message.type === "cdm_media_list") return Promise.resolve(mediaRows);
        selectedMedia = message.id; mediaRows = []; return Promise.resolve({ok: true});
      }}, storage: {sync: {
      get() { return Promise.resolve({interceptionFilters: {minSizeBytes: 7,
          extensionsAllow: ["PDF"]}, siteExclusions: ["*.EXAMPLE.invalid"]}); },
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
  assert.equal(elements.siteExclusions.value, "*.example.invalid");
  elements.minSizeBytes.value = "128";
  elements.extensionsAllow.value = ".ZIP, zip, tar.gz";
  elements.mimeDeny.value = "VIDEO/*";
  await submit({preventDefault() {}});
  assert.deepEqual(saved.interceptionFilters, {minSizeBytes: 128,
    extensionsAllow: ["zip", "tar.gz"], extensionsDeny: [],
    mimeAllow: [], mimeDeny: ["video/*"]});
  assert.deepEqual(saved.siteExclusions, ["*.example.invalid"]);
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
  failSave = false;
  grantedMedia = false;
  elements.mediaDetection.checked = true;
  await submit({preventDefault() {}});
  assert.equal(saved, previous);
  assert.match(elements.status.textContent, /permissions/);
  grantedMedia = true;
  await submit({preventDefault() {}});
  assert.equal(saved.mediaDetection, true);
  assert.deepEqual(Array.from(mediaPermissionRequests.at(-1).permissions), ["webRequest"]);
  assert(!mediaPermissionRequests.at(-1).permissions.includes("cookies"));
  assert.equal(elements.mediaCandidates.children.length, 1);
  assert.match(elements.mediaCandidates.children[0].textContent, /example.invalid/);
  await elements.mediaOffer.listeners.click();
  assert.equal(selectedMedia, "media-option-fixture");
  assert.equal(elements.mediaCandidates.children.length, 0);
  assert.equal(elements.mediaOffer.disabled, true);
  const filters = context.CdmFilters;
  assert.equal(filters.mediaKind("https://example.invalid/file.MPD?q=1"), "dash");
  assert.equal(filters.mediaKind("https://example.invalid/video", "Video/MP4; charset=x"), "video");
  assert.equal(filters.mediaKind("https://example.invalid/page", "text/html"), "");
  assert.equal(filters.mediaKind("data:video/mp4,fixture", "video/mp4"), "");
  assert.throws(() => filters.normalize({extensionsAllow: Array(65).fill("zip")}), /64/);
  assert.throws(() => filters.normalize({mimeAllow: [3]}), /text/);
  assert.throws(() => filters.normalize({extensionsDeny: ["*"]}), /extension/);
  assert.throws(() => filters.normalize({mimeDeny: ["x".repeat(129) + "/pdf"]}), /too long/);
  assert.equal(filters.reason({url: "https://example.invalid/a", mime: "text/plain"},
    filters.normalize({mimeAllow: ["*/*"]})), "");
  assert.throws(() => filters.normalizeSites(["https://example.invalid"]), /hostname/);
  assert.throws(() => filters.normalizeSites(["a".repeat(64) + ".invalid"]), /hostname/);
  assert.throws(() => filters.normalizeSites(Array(65).fill("example.invalid")), /64/);
  assert.equal(filters.excluded("https://EXAMPLE.invalid.:443/file",
    filters.normalizeSites(["*.example.invalid"])), true);
  assert.notEqual(filters.reason({url: "https://example.invalid/a"},
    filters.normalize({mimeAllow: ["*/*"]})), "");
}

async function verifyWithoutOptionalWebRequest(file, globalName) {
  let downloadListener;
  let permissionListener;
  let headerRegistrations = 0;
  let mediaRegistrations = 0;
  const sent = [];
  const canceled = [];
  const port = {
    postMessage(message) { sent.push(message); },
    onMessage: {addListener() {}}, onDisconnect: {addListener() {}}
  };
  const api = {
    action: {setTitle() {}, setBadgeText() {}, setBadgeBackgroundColor() {},
      onClicked: {addListener() {}}},
    downloads: {onCreated: {addListener(listener) { downloadListener = listener; }},
      cancel(id) { canceled.push(id); return Promise.resolve(); }},
    storage: {sync: {get() { return Promise.resolve({}); }},
      onChanged: {addListener() {}}},
    runtime: {connectNative() { return port; },
      onInstalled: {addListener() {}}, onMessage: {addListener() {}}},
    permissions: {onAdded: {addListener(listener) { permissionListener = listener; }}},
    contextMenus: {onClicked: {addListener() {}}}
  };
  const sandbox = vm.createContext({[globalName]: api, URL, TextEncoder, console,
    crypto: {randomUUID() { return "optional-api-fixture"; }},
    importScripts(name) {
      vm.runInContext(fs.readFileSync(path.join(path.dirname(file), name), "utf8"), sandbox);
    }});
  if (globalName === "browser")
    vm.runInContext(fs.readFileSync(path.join(path.dirname(file), "filters.js"), "utf8"), sandbox);
  vm.runInContext(fs.readFileSync(file, "utf8"), sandbox);
  assert.equal(typeof downloadListener, "function",
    "automatic downloads must register without optional webRequest permission");
  await downloadListener({id: 42, state: "in_progress",
    url: "https://example.invalid/test.zip", totalBytes: 100});
  assert.deepEqual(canceled, [42]);
  assert.equal(sent.at(-1).type, "download_offer");
  assert.equal(typeof permissionListener, "function");
  api.webRequest = {
    onSendHeaders: {addListener() { headerRegistrations++; }},
    onHeadersReceived: {addListener() { mediaRegistrations++; }}
  };
  permissionListener({permissions: ["webRequest"]});
  permissionListener({permissions: ["webRequest"]});
  assert.equal(headerRegistrations, 1);
  assert.equal(mediaRegistrations, 1);
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
  verifyWithoutOptionalWebRequest(process.argv[2], "chrome"),
  verifyWithoutOptionalWebRequest(process.argv[3], "browser"),
  verifyOptions(process.argv[2], "chrome"),
  verifyOptions(process.argv[3], "browser")
]).catch(error => { console.error(error); process.exitCode = 1; });
