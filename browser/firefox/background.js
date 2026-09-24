const HOST_NAME = "org.cdm.browser";
let hostPort = null;
const pending = new Map();

function showError(message) {
  browser.action.setBadgeBackgroundColor({color: "#a34448"});
  browser.action.setBadgeText({text: "!"});
  browser.action.setTitle({title: `cdm: ${message}`});
  console.error(`cdm: ${message}`);
}

function clearError() {
  browser.action.setBadgeText({text: ""});
  browser.action.setTitle({title: "Core Download Manager"});
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

browser.downloads.onCreated.addListener(item => {
  const url = item.finalUrl || item.url || "";
  if (!/^https?:\/\//i.test(url) || item.state !== "in_progress") return;
  const requestId = crypto.randomUUID();
  const filename = (item.filename || "").split(/[\\/]/).pop() || "";
  const port = connectHost();
  pending.set(requestId, item.id);
  try {
    port.postMessage({
      type: "download_offer",
      request_id: requestId,
      url,
      referrer: item.referrer || "",
      filename,
      mime: item.mime || "",
      total_bytes: item.totalBytes > 0 ? item.totalBytes : 0,
      browser: "firefox"
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
