const HOST_NAME = "org.cdm.browser";
let hostPort = null;
const pending = new Map();
// The background context owns these in-memory values; restart resets opt-in.
const enabledOrigins = new Set();
const observedHeaders = new Map();
const HEADER_WINDOW_MS = 10000;

function originFor(url) {
  try {
    const parsed = new URL(url);
    return ["http:", "https:"].includes(parsed.protocol) ? parsed.origin : null;
  } catch (_) {
    return null;
  }
}

function safeValue(value, limit) {
  return typeof value === "string" && !/[\r\n\0]/.test(value) &&
    new TextEncoder().encode(value).length <= limit ? value : "";
}

function forgetOldHeaders() {
  const now = Date.now();
  for (const [url, samples] of observedHeaders) {
    const recent = samples.filter(sample => now - sample.at <= HEADER_WINDOW_MS);
    if (recent.length) observedHeaders.set(url, recent);
    else observedHeaders.delete(url);
  }
}

browser.action.setTitle({title:
  "Click to use browser session on this site; cookies may authenticate as you"});
browser.action.onClicked.addListener(async tab => {
  const origin = originFor(tab.url || "");
  if (!origin) return;
  const pattern = `${origin}/*`;
  if (enabledOrigins.has(origin)) {
    enabledOrigins.delete(origin);
    for (const candidate of mediaCandidates.values())
      if (originFor(candidate.url) === origin) candidate.context = {};
    for (const url of observedHeaders)
      if (originFor(url) === origin) observedHeaders.delete(url);
    await browser.permissions.remove({origins: [pattern]}).catch(() => false);
    browser.action.setBadgeText({text: enabledOrigins.size ? "ON" : ""});
    browser.action.setTitle({title:
      enabledOrigins.size ? "Browser session on for selected sites; click to toggle"
        : "Click to use browser session on this site; cookies may authenticate as you"});
    return;
  }
  const granted = await browser.permissions.request({
    permissions: ["cookies", "webRequest"], origins: [pattern]
  }).catch(() => false);
  if (!granted) return;
  enabledOrigins.add(origin);
  browser.action.setBadgeText({text: "ON"});
  browser.action.setTitle({title: "Browser session on for this site; click to turn off"});
});

function observeSendHeaders(details) {
  if (details.incognito) return;
  if (!enabledOrigins.has(originFor(details.url || ""))) return;
  forgetOldHeaders();
  const fields = {};
  for (const header of details.requestHeaders || []) {
    const name = header.name.toLowerCase();
    if (name === "user-agent") fields.user_agent = safeValue(header.value, 256);
    if (name === "referer") fields.referer = safeValue(header.value, 2048);
  }
  if (!fields.user_agent && !fields.referer) return;
  const samples = observedHeaders.get(details.url) || [];
  samples.push({at: Date.now(), fields});
  observedHeaders.set(details.url, samples.slice(-2));
  if (observedHeaders.size > 128)
    observedHeaders.delete(observedHeaders.keys().next().value);
}

async function offerContext(item, url) {
  if (item.incognito) return {}; // No cookie-store correlation for incognito.
  const origin = originFor(url);
  if (!origin || !enabledOrigins.has(origin)) return {};
  const allowed = await browser.permissions.contains({
    permissions: ["cookies", "webRequest"], origins: [`${origin}/*`]
  }).catch(() => false);
  if (!allowed || !enabledOrigins.has(origin)) return {};
  forgetOldHeaders();
  const samples = observedHeaders.get(url) || [];
  observedHeaders.delete(url);
  const fields = samples.length === 1 ? samples[0].fields : {};
  const result = {};
  const userAgent = safeValue(fields.user_agent, 256);
  const referer = safeValue(fields.referer || item.referrer, 2048);
  if (userAgent) result.user_agent = userAgent;
  if (referer) result.referer = referer;
  try {
    const cookies = await browser.cookies.getAll({url});
    if (!enabledOrigins.has(origin)) return {};
    const parts = [];
    let bytes = 0;
    for (const entry of cookies) {
      if (!/^[A-Za-z0-9_.$-]+$/.test(entry.name) ||
          typeof entry.value !== "string" || /[;\r\n\0]/.test(entry.value))
        continue;
      const part = `${entry.name}=${entry.value}`;
      bytes += new TextEncoder().encode(part).length + (parts.length ? 2 : 0);
      if (bytes > 4096) { parts.length = 0; break; }
      parts.push(part);
    }
    if (parts.length) result.cookie = parts.join("; ");
  } catch (_) {
    // Revocation or browser cookie restrictions leave the offer URL-only.
  }
  return result;
}

function showError(message) {
  browser.action.setBadgeBackgroundColor({color: "#a34448"});
  browser.action.setBadgeText({text: "!"});
  browser.action.setTitle({title: `cdm: ${message}`});
  console.error(`cdm: ${message}`);
}

function clearError() {
  browser.action.setBadgeText({text: enabledOrigins.size ? "ON" : ""});
  browser.action.setTitle({title: enabledOrigins.size
    ? "Browser session on; click on a site to toggle it"
    : "Click to use browser session on this site; cookies may authenticate as you"});
}

function connectHost() {
  if (hostPort) return hostPort;
  const port = browser.runtime.connectNative(HOST_NAME);
  port.onMessage.addListener(message => {
    if (message.type === "error") {
      showError(message.error || "native host error");
      pending.delete(message.request_id);
    } else if (message.type === "offer_skipped") {
      pending.delete(message.request_id);
      browser.action.setBadgeText({text: "SKIP"});
      browser.action.setTitle({title: "cdm: left in browser (site is excluded)"});
    } else if (message.type === "offer_registered") {
      clearError();
    } else if (message.type === "offer_state") {
      if (["complete", "dismissed", "canceled", "error"].includes(message.state)) {
        pending.delete(message.request_id);
      }
      if (message.state === "error") showError(message.error || "download failed");
    }
  });
  port.onDisconnect.addListener(() => {
    if (hostPort === port) hostPort = null;
    const reason = browser.runtime.lastError?.message || "native host disconnected";
    if (pending.size) showError(reason);
    pending.clear();
  });
  hostPort = port;
  return port;
}

async function automaticAllowed(item) {
  try {
    const stored = await browser.storage.sync.get(["interceptionFilters", "siteExclusions"]);
    const sites = CdmFilters.normalizeSites(stored.siteExclusions);
    const reason = CdmFilters.excluded(item.finalUrl || item.url, sites) ? "site is excluded"
      : CdmFilters.reason(item, CdmFilters.normalize(stored.interceptionFilters));
    if (!reason) return true;
    browser.action.setTitle({title: `cdm: left in browser (${reason})`});
  } catch (_) {
    browser.action.setTitle({title: "cdm: left in browser because filters could not be loaded"});
  }
  browser.action.setBadgeBackgroundColor({color: "#687785"});
  browser.action.setBadgeText({text: "SKIP"});
  return false;
}

async function offerDownload(item, url, automatic = false, media = null) {
  const requestId = crypto.randomUUID();
  const filename = (item.filename || "").split(/[\\/]/).pop() || "";
  pending.set(requestId, item.id);
  try {
    const stored = await browser.storage.sync.get("siteExclusions");
    const sites = CdmFilters.normalizeSites(stored.siteExclusions);
    if (automatic && CdmFilters.excluded(url, sites)) {
      pending.delete(requestId);
      browser.action.setBadgeText({text: "SKIP"});
      browser.action.setTitle({title: "cdm: left in browser (site is excluded)"});
      return false;
    }
    let context = media ? media.context : await offerContext(item, url);
    if (media && (!enabledOrigins.has(originFor(url)) ||
        !await browser.permissions.contains({permissions: ["cookies", "webRequest"],
            origins: [`${originFor(url)}/*`]}))) {
      media.context = {};
      context = {};
    }
    const port = connectHost();
    // Configure each new native-host connection before its first offer;
    // refreshing per offer also picks up sync edits without cached policy.
    port.postMessage({type: "set_site_exclusions", sites});
    port.postMessage({
      type: media ? "media_offer" : "download_offer",
      ...(media ? {kind: media.kind} : {}),
      automatic,
      request_id: requestId,
      url,
      referrer: media?.pageReferrer || context.referer || "",
      filename,
      mime: item.mime || "",
      total_bytes: item.totalBytes > 0 ? item.totalBytes : 0,
      browser: "firefox",
      ...context
    });
  } catch (error) {
    pending.delete(requestId);
    showError(error.message || "could not contact native host");
    return false;
  }
  return true;
}

browser.downloads.onCreated.addListener(async item => {
  const url = item.finalUrl || item.url || "";
  if (!/^https?:\/\//i.test(url) || item.state !== "in_progress") return;
  if (!await automaticAllowed(item)) return;
  if (!await offerDownload(item, url, true)) return;
  browser.downloads.cancel(item.id).catch(error => {
    showError(`could not cancel browser download: ${error.message}`);
  });
});

browser.runtime.onInstalled.addListener(async () => {
  try {
    await browser.contextMenus.removeAll();
    browser.contextMenus.create({id: "cdm-download-link", title: "Download link with cdm",
      contexts: ["link"], targetUrlPatterns: ["http://*/*", "https://*/*"]});
    browser.contextMenus.create({id: "cdm-download-page", title: "Download page with cdm",
      contexts: ["page"], documentUrlPatterns: ["http://*/*", "https://*/*"]});
  } catch (_) {
    showError("could not create context menu");
  }
});

browser.contextMenus.onClicked.addListener(async (info, tab) => {
  const url = info.menuItemId === "cdm-download-link" ? info.linkUrl
    : info.menuItemId === "cdm-download-page" ? info.pageUrl || tab?.url : "";
  if (!originFor(url)) return;
  await offerDownload({referrer: info.pageUrl || "", incognito: !!tab?.incognito}, url);
});

// Background/service worker is the sole owner; URLs/context never enter storage.
// Ten-minute TTL and 64 total entries bound memory; manifests survive video churn.
const mediaCandidates = new Map();
function pruneMedia() {
  for (const [key, value] of mediaCandidates)
    if (Date.now() - value.at > 600000) mediaCandidates.delete(key);
}

async function probeYouTube(sender) {
  if (!CdmYouTubeProbe.isYouTubePage(sender.url) ||
      !await browser.permissions.contains({permissions: ["webRequest"],
        origins: ["http://*/*", "https://*/*"]})) return null;
  const injected = await browser.scripting.executeScript({
    target: {tabId: sender.tab.id, frameIds: [sender.frameId]},
    world: "MAIN", func: CdmYouTubeProbe.pageProbe
  });
  const result = injected?.[0]?.result;
  const page = new URL(sender.url);
  const expectedId = page.pathname === "/watch" ? page.searchParams.get("v")
    : page.pathname.slice(7).split("/")[0];
  if (result?.videoId !== expectedId || !Array.isArray(result.formats)) return null;
  const title = safeValue(result.title, 180) || "Video";
  const formats = [];
  for (const format of result.formats.slice(0, 16)) {
    if (!Number.isInteger(format.itag) || format.itag < 0 ||
        !safeValue(format.quality, 32) || format.mime !== "video/mp4" ||
        !safeValue(format.url, 2047) ||
        !Number.isSafeInteger(format.totalBytes) || format.totalBytes < 0)
      continue;
    let endpoint;
    try { endpoint = new URL(format.url); } catch (_) { continue; }
    const host = endpoint.hostname;
    if (endpoint.protocol !== "https:" || endpoint.username || endpoint.password ||
        (endpoint.port && endpoint.port !== "443") ||
        host === "localhost" || host.endsWith(".localhost") ||
        host.endsWith(".local") || host.endsWith(".internal") ||
        /^[0-9.]+$/.test(host) || host.includes(":")) continue;
    formats.push(format);
  }
  return {videoId: expectedId, title, formats};
}

function youtubeFilename(title, quality) {
  const stem = title.replace(/[\\/\x00-\x1f\x7f]/g, "_").trim().slice(0, 120) || "Video";
  return `${stem} ${quality}.mp4`;
}
async function observeMediaHeaders(details) {
  if (details.incognito || details.tabId < 0 || details.method !== "GET" ||
      details.statusCode < 200 || details.statusCode >= 300 ||
      !originFor(details.url) || new TextEncoder().encode(details.url).length > 2047)
    return;
  const mime = safeValue((details.responseHeaders || []).find(h =>
      h.name.toLowerCase() === "content-type")?.value || "", 127);
  const kind = CdmFilters.mediaKind(details.url, mime);
  if (!kind) return;
  try {
    const stored = await browser.storage.sync.get(["mediaDetection", "siteExclusions"]);
    if (stored.mediaDetection !== true) { mediaCandidates.clear(); return; }
    if (CdmFilters.excluded(details.url, CdmFilters.normalizeSites(stored.siteExclusions)))
      return;
    if (!await browser.permissions.contains({permissions: ["webRequest"],
        origins: ["http://*/*", "https://*/*"]})) return;
    pruneMedia();
    const frameId = Number.isInteger(details.frameId) && details.frameId >= 0
      ? details.frameId : 0;
    const hasManifest = [...mediaCandidates.values()].some(value =>
      value.tabId === details.tabId && value.frameId === frameId &&
      (value.kind === "hls" || value.kind === "dash"));
    if (CdmFilters.playbackFragment(details.url, kind, details.statusCode,
                                    details.responseHeaders, hasManifest)) return;
    const key = `${details.tabId}\n${frameId}\n${details.url}`;
    if (mediaCandidates.has(key)) return;
    const context = await offerContext({incognito: false}, details.url);
    // Recheck enablement after asynchronous context capture/revocation.
    const latest = await browser.storage.sync.get("mediaDetection");
    if (latest.mediaDetection !== true) { mediaCandidates.clear(); return; }
    if (mediaCandidates.has(key)) return;
    if (mediaCandidates.size >= 64) {
      const video = [...mediaCandidates].find(([, entry]) => entry.kind === "video");
      if (!video && kind === "video") return;
      mediaCandidates.delete(video ? video[0] : mediaCandidates.keys().next().value);
    }
    const filename = safeValue(new URL(details.url).pathname.split("/").pop() || "", 511);
    mediaCandidates.set(key, {id: crypto.randomUUID(), url: details.url, kind, mime,
      filename, tabId: details.tabId, frameId, context, at: Date.now()});
  } catch (_) {
    // Unavailable storage/permissions fail closed; no native offer was sent.
  }
}

// webRequest is optional; without consent the namespace may not exist at all.
// Keep ordinary downloads registered even when header/media observation cannot.
let headerObserverRegistered = false;
let mediaObserverRegistered = false;
const MEDIA_SCRIPT_ID = "cdm-media-overlay";
// The background context serializes registration updates; browser scripting owns the registration.
let mediaScriptSync = Promise.resolve();
function queueMediaScriptSync() {
  mediaScriptSync = mediaScriptSync.then(async () => {
    const stored = await browser.storage.sync.get("mediaDetection");
    const enabled = stored.mediaDetection === true &&
      await browser.permissions.contains({permissions: ["webRequest"],
        origins: ["http://*/*", "https://*/*"]});
    const registered = await browser.scripting.getRegisteredContentScripts({ids: [MEDIA_SCRIPT_ID]});
    if (enabled && registered[0]?.allFrames !== true) {
      if (registered.length)
        await browser.scripting.unregisterContentScripts({ids: [MEDIA_SCRIPT_ID]});
      await browser.scripting.registerContentScripts([{id: MEDIA_SCRIPT_ID,
        js: ["media_overlay.js"], matches: ["http://*/*", "https://*/*"],
        allFrames: true, runAt: "document_idle"}]);
    } else if (!enabled && registered.length !== 0) {
      await browser.scripting.unregisterContentScripts({ids: [MEDIA_SCRIPT_ID]});
    }
  }).catch(() => { /* Missing permissions or scripting support leave the overlay disabled. */ });
}
function registerOptionalObservers() {
  const webRequest = browser.webRequest;
  if (!webRequest) return;
  if (!headerObserverRegistered && webRequest.onSendHeaders) {
    try {
      webRequest.onSendHeaders.addListener(observeSendHeaders,
        {urls: ["http://*/*", "https://*/*"]}, ["requestHeaders"]);
      headerObserverRegistered = true;
    } catch (_) { /* Retry if the optional permission is granted later. */ }
  }
  if (!mediaObserverRegistered && webRequest.onHeadersReceived) {
    try {
      webRequest.onHeadersReceived.addListener(observeMediaHeaders,
        {urls: ["http://*/*", "https://*/*"]}, ["responseHeaders"]);
      mediaObserverRegistered = true;
    } catch (_) { /* Retry if the optional permission is granted later. */ }
  }
}
registerOptionalObservers();
queueMediaScriptSync();
browser.permissions?.onAdded?.addListener(() => {
  registerOptionalObservers();
  queueMediaScriptSync();
});
browser.permissions?.onRemoved?.addListener(() => {
  mediaCandidates.clear();
  queueMediaScriptSync();
});

browser.runtime.onMessage.addListener((message, sender, reply) => {
  const fromOptions = sender.url === browser.runtime.getURL("options.html") &&
    ["cdm_media_list", "cdm_media_offer"].includes(message?.type);
  const fromPage = Number.isInteger(sender.tab?.id) && !sender.tab.incognito &&
    Number.isInteger(sender.frameId) && sender.frameId >= 0 &&
    !!originFor(sender.url) &&
    ["cdm_media_list_tab", "cdm_media_offer_tab",
      "cdm_youtube_formats_tab"].includes(message?.type);
  if (!fromOptions && !fromPage) return;
  (async () => {
    const stored = await browser.storage.sync.get("mediaDetection");
    if (stored.mediaDetection !== true) {
      mediaCandidates.clear();
    }
    pruneMedia();
    if (message.type === "cdm_youtube_formats_tab") {
      const found = stored.mediaDetection === true ? await probeYouTube(sender) : null;
      const current = await browser.storage.sync.get("mediaDetection");
      if (!found || current.mediaDetection !== true ||
          !await browser.permissions.contains({permissions: ["webRequest"],
            origins: ["http://*/*", "https://*/*"]})) { reply([]); return; }
      for (const [key, value] of mediaCandidates)
        if (value.youtube && value.tabId === sender.tab.id &&
            value.frameId === sender.frameId) mediaCandidates.delete(key);
      const rows = [];
      for (const format of found.formats) {
        if (mediaCandidates.size >= 64)
          mediaCandidates.delete(mediaCandidates.keys().next().value);
        const id = crypto.randomUUID();
        const filename = youtubeFilename(found.title, format.quality);
        const key = `youtube\n${sender.tab.id}\n${sender.frameId}\n${found.videoId}\n${format.itag}`;
        mediaCandidates.set(key, {id, url: format.url, kind: "video",
          mime: format.mime, filename, totalBytes: format.totalBytes,
          quality: format.quality, tabId: sender.tab.id, frameId: sender.frameId,
          context: {}, pageReferrer: sender.url,
          youtube: {videoId: found.videoId, itag: format.itag}, at: Date.now()});
        rows.push({id, kind: "video", filename, quality: format.quality,
          totalBytes: format.totalBytes});
      }
      reply(rows);
      return;
    }
    if (message.type === "cdm_media_list" || message.type === "cdm_media_list_tab") {
      reply([...mediaCandidates.values()].filter(value =>
        !value.youtube && (fromOptions || (value.tabId === sender.tab.id &&
          value.frameId === sender.frameId))).map(({id, url, kind, mime, filename, tabId}) =>
        fromOptions ? {id, url, kind, mime, filename, tabId} : {id, kind, mime, filename}));
      return;
    }
    const selected = [...mediaCandidates].find(([, value]) => value.id === message.id &&
      (!value.youtube || !fromOptions) &&
      (fromOptions || (value.tabId === sender.tab.id &&
        value.frameId === sender.frameId)));
    if (!selected) { reply({ok: false}); return; }
    const [key, candidate] = selected;
    if (!await browser.permissions.contains({permissions: ["webRequest"],
        origins: ["http://*/*", "https://*/*"]})) { reply({ok: false}); return; }
    if (candidate.youtube) {
      const fresh = await probeYouTube(sender);
      const format = fresh?.videoId === candidate.youtube.videoId &&
        fresh.formats.find(item => item.itag === candidate.youtube.itag);
      const current = await browser.storage.sync.get("mediaDetection");
      if (!format || current.mediaDetection !== true ||
          !await browser.permissions.contains({permissions: ["webRequest"],
            origins: ["http://*/*", "https://*/*"]})) {
        reply({ok: false}); return;
      }
      candidate.url = format.url;
      candidate.totalBytes = format.totalBytes;
    }
    const ok = await offerDownload(candidate, candidate.url, false, candidate);
    if (ok) mediaCandidates.delete(key);
    reply({ok});
  })().catch(() => reply(message.type === "cdm_media_list" ||
    message.type === "cdm_media_list_tab" ||
    message.type === "cdm_youtube_formats_tab" ? [] : {ok: false}));
  return true; // Callback reply works in Firefox and older Chrome releases.
});

browser.storage.onChanged.addListener((changes, area) => {
  if (area === "sync" && changes.mediaDetection && changes.mediaDetection.newValue !== true) {
    mediaCandidates.clear();
  }
  if (area === "sync" && changes.mediaDetection) queueMediaScriptSync();
});
