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

browser.webRequest.onSendHeaders.addListener(details => {
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
}, {urls: ["http://*/*", "https://*/*"]}, ["requestHeaders"]);

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
    } else if (message.type === "offer_registered") {
      clearError();
    } else if (message.type === "offer_state") {
      if (["complete", "dismissed", "error"].includes(message.state)) {
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

browser.downloads.onCreated.addListener(async item => {
  const url = item.finalUrl || item.url || "";
  if (!/^https?:\/\//i.test(url) || item.state !== "in_progress") return;
  const requestId = crypto.randomUUID();
  const filename = (item.filename || "").split(/[\\/]/).pop() || "";
  const context = await offerContext(item, url);
  const port = connectHost();
  pending.set(requestId, item.id);
  try {
    port.postMessage({
      type: "download_offer",
      request_id: requestId,
      url,
      referrer: context.referer || "",
      filename,
      mime: item.mime || "",
      total_bytes: item.totalBytes > 0 ? item.totalBytes : 0,
      browser: "firefox",
      ...context
    });
  } catch (error) {
    pending.delete(requestId);
    showError(error.message || "could not contact native host");
    return;
  }
  browser.downloads.cancel(item.id).catch(error => {
    showError(`could not cancel browser download: ${error.message}`);
  });
});
