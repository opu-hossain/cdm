const assert = require("node:assert/strict");
const fs = require("node:fs");
const vm = require("node:vm");

function verify(file, globalName, expectedBrowser) {
  let downloadListener;
  let nativeListener;
  let disconnectListener;
  const messages = [];
  const canceled = [];
  const badges = [];
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
      setBadgeText(value) { badges.push(value.text); },
      setBadgeBackgroundColor() {},
      setTitle() {}
    }
  };
  const context = {
    [globalName]: api,
    crypto: { randomUUID: () => "browser-test-request" },
    console
  };
  vm.runInNewContext(fs.readFileSync(file, "utf8"), context);
  assert.equal(typeof downloadListener, "function");
  downloadListener({ id: 7, state: "in_progress",
    url: "https://example.org/file.zip", filename: "/tmp/file.zip",
    totalBytes: 120, referrer: "https://example.org/" });
  assert.deepEqual(canceled, [7]);
  assert.equal(messages.length, 1);
  assert.equal(messages[0].type, "download_offer");
  assert.equal(messages[0].filename, "file.zip");
  assert.equal(messages[0].browser, expectedBrowser);
  assert.equal(messages[0].request_id, "browser-test-request");
  downloadListener({ id: 8, state: "in_progress", url: "blob:unsupported" });
  assert.equal(messages.length, 1);
  nativeListener({ type: "error", request_id: "browser-test-request",
    error: "host unavailable" });
  assert.equal(badges.at(-1), "!");
  nativeListener({ type: "offer_registered", request_id: "browser-test-request" });
  assert.equal(badges.at(-1), "");
  assert.equal(typeof disconnectListener, "function");
}

verify(process.argv[2], "chrome", "chromium");
verify(process.argv[3], "browser", "firefox");
