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
  let youtubeProbes = 0;
  let youtubeMediaUrl = "https://media.example.invalid/videoplayback?token=fixture";
  let clock = Date.now();
  let storageListener;
  let permissionAddedListener;
  let permissionRemovedListener;
  const registeredMediaScripts = [];
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
    scripting: {
      getRegisteredContentScripts() { return Promise.resolve(registeredMediaScripts); },
      registerContentScripts(scripts) { registeredMediaScripts.push(...scripts); return Promise.resolve(); },
      unregisterContentScripts() { registeredMediaScripts.length = 0; return Promise.resolve(); },
      executeScript({target, world, func}) {
        assert.equal(world, "MAIN");
        assert.equal(target.tabId, 7);
        assert.deepEqual(Array.from(target.frameIds), [0]);
        assert.equal(typeof func, "function");
        youtubeProbes++;
        return Promise.resolve([{result: {videoId: "fixture123", title: "Fixture video",
          formats: [{itag: 18, quality: "360p", mime: "video/mp4",
            url: `${youtubeMediaUrl}${youtubeProbes}`,
            totalBytes: 2048}]}}]);
      }
    },
    permissions: {
      onAdded: {addListener(listener) { permissionAddedListener = listener; }},
      onRemoved: {addListener(listener) { permissionRemovedListener = listener; }},
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
    console, URL, TextEncoder, setTimeout, clearTimeout,
    importScripts(...names) {
      for (const name of names)
        vm.runInContext(fs.readFileSync(path.join(path.dirname(file), name), "utf8"), sandbox);
    }
  };
  const sandbox = vm.createContext(context);
  if (globalName === "browser")
    vm.runInContext(fs.readFileSync(path.join(path.dirname(file), "filters.js"), "utf8"), sandbox);
  if (globalName === "browser")
    vm.runInContext(fs.readFileSync(path.join(path.dirname(file), "youtube_probe.js"), "utf8"), sandbox);
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
  storageListener({mediaDetection: {newValue: true}}, "sync");
  await new Promise(setImmediate);
  assert.equal(registeredMediaScripts.length, 1);
  assert.deepEqual(Array.from(registeredMediaScripts[0].js), ["media_overlay.js"]);
  assert.equal(registeredMediaScripts[0].allFrames, true,
    "embedded video frames need their own in-page control");
  registeredMediaScripts[0].allFrames = false; // Persisted registration from an older extension.
  storageListener({mediaDetection: {newValue: true}}, "sync");
  await new Promise(setImmediate);
  assert.equal(registeredMediaScripts.length, 1);
  assert.equal(registeredMediaScripts[0].allFrames, true);
  assert.equal(typeof permissionAddedListener, "function");
  assert.equal(typeof permissionRemovedListener, "function");
  await mediaListener(response);
  await mediaListener(response);
  let candidates = await callRuntime({type: "cdm_media_list"});
  assert.equal(candidates.length, 1);
  assert.equal(candidates[0].kind, "hls");
  assert.equal(messages.length, beforeMedia, "detection must not open native offers");
  assert(!Object.hasOwn(candidates[0], "cookie"));
  function callTabRuntime(message, tabId = 7, frameId = 0,
                          url = "https://example.invalid/page") {
    return new Promise(resolve => {
      const payload = message.type?.startsWith("cdm_site_")
        ? {page_id: "page-1", ...message} : message;
      const keep = runtimeListener(payload,
        {url, tab: {id: tabId}, frameId}, resolve);
      if (keep !== true) resolve(undefined);
    });
  }
  const inPage = await callTabRuntime({type: "cdm_media_list_tab"});
  assert.equal(inPage.length, 1);
  assert.equal(inPage[0].id, candidates[0].id);
  assert(!Object.hasOwn(inPage[0], "url"), "page overlay must not receive signed URLs");
  assert.equal((await callTabRuntime({type: "cdm_media_list_tab"}, 8)).length, 0);
  assert.equal((await callTabRuntime({type: "cdm_media_list_tab"}, 7, 1)).length, 0);
  assert.equal((await callTabRuntime({type: "cdm_media_offer_tab", id: candidates[0].id}, 8)).ok, false);
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
  assert.equal(await callTabRuntime({type: "cdm_site_probe_tab", explicit: true}), undefined);
  assert.equal(await callTabRuntime({type: "cdm_site_select_tab", id: "18"}), undefined);
  assert.equal(wireMessages.some(message => message.type === "site_probe"), false);
  const youtubePage = "https://www.youtube.com/watch?v=fixture123";
  const ytRows = await callTabRuntime({type: "cdm_youtube_formats_tab"}, 7, 0,
    youtubePage);
  assert.equal(ytRows.length, 1);
  assert.equal(ytRows[0].quality, "360p");
  assert.equal(ytRows[0].totalBytes, 2048);
  assert(!Object.hasOwn(ytRows[0], "url"));
  assert.equal((await callTabRuntime({type: "cdm_youtube_formats_tab"}, 7, 0,
    "https://example.invalid/watch?v=fixture123")).length, 0);
  assert.equal((await callTabRuntime({type: "cdm_media_offer_tab", id: ytRows[0].id},
    7, 0, youtubePage)).ok, true);
  assert.equal(youtubeProbes, 2, "refresh the signed URL before offering it");
  assert.equal(messages.at(-1).kind, "video");
  assert.equal(messages.at(-1).url,
    "https://media.example.invalid/videoplayback?token=fixture2");
  assert.equal(messages.at(-1).filename, "Fixture video 360p.mp4");
  assert.equal(messages.at(-1).referrer, youtubePage);
  assert.equal(messages.at(-1).cookie, undefined);
  youtubeMediaUrl = "https://127.0.0.1/private?token=fixture";
  assert.equal((await callTabRuntime({type: "cdm_youtube_formats_tab"}, 7, 0,
    youtubePage)).length, 0, "player data must not offer loopback URLs");
  await mediaListener({...response, url: "https://example.invalid/overlay.mp4"});
  const pageRows = await callTabRuntime({type: "cdm_media_list_tab"});
  assert.equal((await callTabRuntime({type: "cdm_media_offer_tab", id: pageRows[0].id})).ok, true);
  assert.equal(messages.at(-1).type, "media_offer");
  assert.equal(messages.at(-1).kind, "video");
  assert.equal((await callTabRuntime({type: "cdm_media_list_tab"})).length, 0);
  await mediaListener({...response, frameId: 1,
    url: "https://example.invalid/embedded.m3u8"});
  const embeddedRows = await callTabRuntime({type: "cdm_media_list_tab"}, 7, 1);
  assert.equal(embeddedRows.length, 1);
  assert.equal((await callTabRuntime({type: "cdm_media_list_tab"})).length, 0);
  assert.equal((await callTabRuntime({type: "cdm_media_offer_tab",
    id: embeddedRows[0].id})).ok, false);
  assert.equal((await callTabRuntime({type: "cdm_media_offer_tab",
    id: embeddedRows[0].id}, 7, 1)).ok, true);
  await mediaListener({...response, url: "https://example.invalid/manifest",
    responseHeaders: [{name: "Content-Type", value: "Application/Dash+XML; charset=utf-8"}]});
  candidates = await callRuntime({type: "cdm_media_list"});
  assert.equal(candidates[0].kind, "dash");
  await mediaListener({...response, url: "https://example.invalid/movie.mp4"});
  await mediaListener({...response, url: "https://example.invalid/stream",
    responseHeaders: [{name: "content-type", value: "video/webm"}]});
  assert.equal((await callRuntime({type: "cdm_media_list"})).length, 3);
  await mediaListener({...response, url: "https://example.invalid/ranged.mp4",
    statusCode: 206, responseHeaders: [{name: "Content-Range", value: "bytes 0-999/5000"}]});
  await mediaListener({...response, url: "https://example.invalid/chunk.m4s",
    responseHeaders: [{name: "Content-Type", value: "video/mp4"}]});
  await mediaListener({...response, url: "https://example.invalid/tiny.mp4",
    responseHeaders: [{name: "Content-Type", value: "video/mp4"},
      {name: "Content-Length", value: "8192"}]});
  assert.equal((await callRuntime({type: "cdm_media_list"})).length, 3,
    "playback fragments must not appear as complete videos");
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
  assert.equal(candidates.length, 3);
  assert(candidates.some(c => c.kind === "dash"), "segments must not crowd out manifests");
  clock += 600001;
  assert.equal((await callRuntime({type: "cdm_media_list"})).length, 0);
  await mediaListener(response);
  granted = false;
  permissionRemovedListener({permissions: ["webRequest"]});
  await new Promise(setImmediate);
  assert.equal(registeredMediaScripts.length, 0);
  assert.equal((await callTabRuntime({type: "cdm_media_list_tab"})).length, 0);
  granted = true;
  mediaDetection = false;
  storageListener({mediaDetection: {newValue: false}}, "sync");
  await new Promise(setImmediate);
  assert.equal(registeredMediaScripts.length, 0);
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
      return Promise.resolve(grantedMedia); }, contains() { return Promise.resolve(grantedMedia); }}, runtime: {sendMessage(message) {
        if (message.type === "cdm_media_list") return Promise.resolve(mediaRows);
        selectedMedia = message.id; mediaRows = []; return Promise.resolve({ok: true});
      }}, storage: {sync: {
      get() { return Promise.resolve({interceptionFilters: {minSizeBytes: 7,
          extensionsAllow: ["PDF"]}, siteExclusions: ["*.EXAMPLE.invalid"],
          mediaDetection: saved?.mediaDetection}); },
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
  await elements.mediaRefresh.listeners.click();
  assert.match(elements.mediaStatus.textContent, /enable media detection/i);
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
  await elements.mediaRefresh.listeners.click();
  assert.match(elements.mediaStatus.textContent, /YouTube.*not supported/i);
  grantedMedia = false;
  await elements.mediaRefresh.listeners.click();
  assert.match(elements.mediaStatus.textContent, /permission/i);
  const filters = context.CdmFilters;
  assert.equal(filters.mediaKind("https://example.invalid/part.m4s", "video/mp4"), "");
  assert.equal(filters.mediaKind("https://example.invalid/segment.ts", "video/mp2t"), "");
  assert.equal(filters.playbackFragment("https://example.invalid/short.mp4",
    "video", 200, [{name: "Content-Length", value: "8192"}], false), false);
  assert.equal(filters.mediaKind("https://example.invalid/file.MPD?q=1"), "dash");
  assert.equal(filters.mediaKind("https://example.invalid/video", "Video/MP4; charset=x"), "video");
  assert.equal(filters.mediaKind("https://example.invalid/videoplayback",
    "application/vnd.yt-ump"), "");
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

async function verifyMediaOverlay(file, globalName) {
  class Element {
    constructor(tag) {
      this.tagName = tag; this.children = []; this.listeners = {}; this.style = {};
      this.textContent = ""; this.hidden = false;
    }
    appendChild(child) { this.children.push(child); return child; }
    append(...children) { children.forEach(child => this.appendChild(child)); }
    replaceChildren(...children) { this.children = children; }
    addEventListener(type, listener) { this.listeners[type] = listener; }
    attachShadow() { this.shadow = new Element("shadow"); return this.shadow; }
    click() { return this.listeners.click?.({stopPropagation() {}}); }
  }
  const root = new Element("html");
  const video = {paused: false, ended: false, getBoundingClientRect() {
    return {left: 20, top: 30, right: 820, bottom: 480, width: 800, height: 450};
  }};
  let rows = [{id: "media-overlay-fixture", kind: "hls", filename: "master.m3u8"}];
  const offers = [];
  let tick;
  const sandbox = vm.createContext({
    [globalName]: {runtime: {sendMessage(message) {
      if (message.type === "cdm_media_list_tab") return Promise.resolve(rows);
      offers.push(message); return Promise.resolve({ok: true});
    }}},
    document: {documentElement: root, title: "Fixture video",
      querySelectorAll() { return [video]; }, createElement(tag) { return new Element(tag); },
      addEventListener() {}},
    window: {addEventListener() {}},
    crypto: {randomUUID() { return "overlay-page"; }},
    setInterval(callback) { tick = callback; },
    console
  });
  vm.runInContext(fs.readFileSync(path.join(path.dirname(file), "media_overlay.js"), "utf8"),
    sandbox);
  assert.equal(root.children.length, 1,
    "playing video should show its control before format probing finishes");
  assert.equal(root.children[0].style.display, "block");
  await new Promise(setImmediate);
  assert.equal(typeof tick, "function");
  assert.equal(root.children.length, 1);
  const host = root.children[0];
  assert.equal(host.style.position, "fixed");
  assert.equal(host.style.display, "block");
  assert.equal(host.style.top, "38px");
  assert.equal(host.style.right, "8px");
  const button = host.shadow.children[1];
  const panel = host.shadow.children[2];
  assert.match(button.textContent, /Download with cdm/);
  await button.click();
  assert.equal(panel.hidden, false);
  assert.match(panel.children[0].textContent, /Fixture video/);
  assert.match(panel.children[1].textContent, /master.m3u8/);
  const firstChoice = panel.children[1];
  await tick();
  assert.equal(panel.children[1], firstChoice, "polling must not replace a choice under the pointer");
  await panel.children[1].click();
  assert.equal(offers[0].type, "cdm_media_offer_tab");
  assert.equal(offers[0].id, "media-overlay-fixture");
  assert.equal(button.textContent, "Offered to cdm");
  rows = [];
  await tick();
  assert.equal(host.style.display, "block", "playing video keeps an in-frame control");
  assert.equal(button.textContent, "Download with cdm");
  await button.click();
  assert.match(panel.children[1].textContent, /No direct media detected/);
  assert.equal(offers.length, 1, "empty picker must not probe the site");
  rows = [{id: "media-overlay-fixture", kind: "hls", filename: "master.m3u8"}];
  video.paused = true;
  await tick();
  assert.equal(host.style.display, "none");
}

async function verifyYouTubeProbe(file) {
  const script = path.join(path.dirname(file), "youtube_probe.js");
  const response = {videoDetails: {videoId: "fixture123", title: "Fixture / video",
    isLive: false}, playabilityStatus: {status: "OK"}, streamingData: {formats: [
    {itag: 18, qualityLabel: "360p", mimeType: 'video/mp4; codecs="avc1, mp4a"',
      audioQuality: "AUDIO_QUALITY_MEDIUM", contentLength: "2048",
      url: "https://media.example.invalid/videoplayback?token=fixture"},
    {itag: 22, qualityLabel: "720p", mimeType: "video/mp4",
      audioQuality: "AUDIO_QUALITY_MEDIUM", signatureCipher: "s=encrypted"},
    {itag: 137, qualityLabel: "1080p", mimeType: "video/mp4",
      url: "https://media.example.invalid/video-only"}
  ]}};
  const webFormats = response.streamingData.formats;
  response.streamingData.formats = []; // Current watch pages can expose SABR only.
  const calls = [];
  let rejectAndroid = false;
  const sandbox = vm.createContext({
    location: {href: "https://www.youtube.com/watch?v=fixture123"},
    document: {getElementById: () => ({getPlayerResponse: () => response})},
    ytcfg: {get: key => key === "INNERTUBE_API_KEY" ? "synthetic-key" : ""},
    fetch: async (url, options) => {
      calls.push({url, options});
      if (rejectAndroid) throw new Error("fixture unavailable");
      return {ok: true, json: async () => ({videoDetails: response.videoDetails,
        playabilityStatus: {status: "OK"},
        streamingData: {formats: [{itag: 18, qualityLabel: "360p",
          mimeType: 'video/mp4; codecs="avc1, mp4a"',
          audioQuality: "AUDIO_QUALITY_MEDIUM", contentLength: "2048",
          url: "https://media.example.invalid/android-direct?token=fixture"}]}})};
    },
    AbortController, clearTimeout, setTimeout, URL
  });
  vm.runInContext(fs.readFileSync(script, "utf8"), sandbox);
  const result = await sandbox.CdmYouTubeProbe.pageProbe();
  assert.equal(result.videoId, "fixture123");
  assert.equal(result.title, "Fixture / video");
  assert.equal(result.formats.length, 1);
  assert.equal(result.formats[0].quality, "360p");
  assert.equal(result.formats[0].url,
    "https://media.example.invalid/android-direct?token=fixture");
  assert.equal(calls.length, 1);
  assert.equal(new URL(calls[0].url).pathname, "/youtubei/v1/player");
  assert.equal(calls[0].options.credentials, "omit");
  assert.equal(JSON.parse(calls[0].options.body).context.client.clientName,
    "ANDROID");
  response.streamingData.formats = webFormats;
  rejectAndroid = true;
  assert.equal((await sandbox.CdmYouTubeProbe.pageProbe()).formats[0].url,
    "https://media.example.invalid/videoplayback?token=fixture");
  response.videoDetails.videoId = "staleVideo";
  assert.equal((await sandbox.CdmYouTubeProbe.pageProbe()).formats.length, 0,
    "SPA navigation must not offer formats for the previous video");
  assert.equal(calls.length, 2, "stale pages must not probe another video");
}

async function verifyYouTubeOverlay(file, globalName) {
  class Element {
    constructor(tag) {
      this.tagName = tag; this.children = []; this.listeners = {}; this.style = {};
      this.textContent = ""; this.hidden = false;
    }
    appendChild(child) { this.children.push(child); return child; }
    append(...children) { children.forEach(child => this.appendChild(child)); }
    replaceChildren(...children) { this.children = children; }
    addEventListener(type, listener) { this.listeners[type] = listener; }
    attachShadow() { this.shadow = new Element("shadow"); return this.shadow; }
    click() { return this.listeners.click?.(); }
  }
  const root = new Element("html");
  const video = {paused: false, ended: false, getBoundingClientRect() {
    return {left: 20, top: 30, right: 820, bottom: 480, width: 800, height: 450};
  }};
  const calls = [];
  const sandbox = vm.createContext({
    [globalName]: {runtime: {sendMessage(message) {
      calls.push(message);
      if (message.type === "cdm_media_list_tab") return Promise.resolve([]);
      if (message.type === "cdm_youtube_formats_tab") return Promise.resolve([
        {id: "yt-fixture", kind: "video", filename: "Fixture video 360p.mp4",
          quality: "360p", totalBytes: 2048}]);
      return Promise.resolve({ok: true});
    }}},
    location: {href: "https://www.youtube.com/watch?v=fixture123"}, URL,
    document: {documentElement: root, title: "Fixture video - YouTube",
      querySelectorAll() { return [video]; }, createElement(tag) { return new Element(tag); },
      addEventListener() {}},
    window: {addEventListener() {}}, setInterval() {}, console
  });
  vm.runInContext(fs.readFileSync(path.join(path.dirname(file), "media_overlay.js"), "utf8"),
    sandbox);
  await new Promise(setImmediate);
  const host = root.children[0];
  const button = host.shadow.children[1];
  const panel = host.shadow.children[2];
  await button.click();
  assert.equal(panel.hidden, false);
  assert(calls.some(call => call.type === "cdm_youtube_formats_tab"));
  assert.match(panel.children[1].textContent, /360p/);
  assert.match(panel.children[1].textContent, /2\.0 KB/);
  await panel.children[1].click();
  assert.equal(calls.at(-1).type, "cdm_media_offer_tab");
  assert.equal(calls.at(-1).id, "yt-fixture");
  assert.equal(button.textContent, "Offered to cdm");
}

for (const file of [process.argv[2], process.argv[3]]) {
  const manifest = JSON.parse(fs.readFileSync(
    path.join(path.dirname(file), "manifest.json"), "utf8"));
  assert(manifest.permissions.includes("activeTab"));
  assert(manifest.permissions.includes("contextMenus"));
  assert(manifest.permissions.includes("storage"));
  assert(manifest.permissions.includes("scripting"));
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
  verifyOptions(process.argv[3], "browser"),
  verifyMediaOverlay(process.argv[2], "chrome"),
  verifyMediaOverlay(process.argv[3], "browser"),
  verifyYouTubeProbe(process.argv[2]),
  verifyYouTubeProbe(process.argv[3]),
  verifyYouTubeOverlay(process.argv[2], "chrome"),
  verifyYouTubeOverlay(process.argv[3], "browser")
]).catch(error => { console.error(error); process.exitCode = 1; });
