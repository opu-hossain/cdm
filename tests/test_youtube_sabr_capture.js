const assert = require("node:assert/strict");
const fs = require("node:fs");
const vm = require("node:vm");

function varint(value) {
  const out = [];
  while (value >= 128) { out.push((value % 128) | 128); value = Math.floor(value / 128); }
  out.push(value);
  return out;
}
const number = (field, value) => [
  ...varint(field * 8), ...varint(value)];
const bytes = (field, value) => [
  ...varint(field * 8 + 2), ...varint(value.length), ...value];
const format = (field, itag) => bytes(field, number(1, itag));
const context = token => bytes(19, bytes(2, token));

for (const file of process.argv.slice(2)) {
  const sandbox = vm.createContext({URL, btoa});
  vm.runInContext(fs.readFileSync(file, "utf8"), sandbox);
  const capture = sandbox.CdmYouTubeSabrCapture;
  const makeRequest = (raw, overrides = {}) => {
    const body = new Uint8Array(raw);
    return {
      method: "POST", tabId: 7,
      url: "https://rr1.googlevideo.com/videoplayback?sabr=1",
      requestBody: {raw: [{bytes: body.buffer}]}, ...overrides};
  };
  const payload = [
    ...bytes(5, [1, 2, 3]), ...format(16, 140), ...format(17, 401),
    ...context(Array(90).fill(7))];
  const session = capture.parseRequest(makeRequest(payload));
  assert.ok(session, file);
  const chosen = capture.select(session, 401);
  assert.equal(chosen.url, "https://rr1.googlevideo.com/videoplayback?sabr=1");
  assert.equal(chosen.request, btoa(String.fromCharCode(...payload)));
  // A player request may advertise only the currently playing quality.
  // The Android player metadata validates other offered itags in the daemon.
  assert.ok(capture.select(session, 137));
  assert.equal(capture.select(session, -1), null);
  const opus = capture.parseRequest(makeRequest([
    ...bytes(5, [1, 2, 3]), ...format(16, 251),
    ...format(17, 401), ...context(Array(90).fill(7))]));
  assert.ok(capture.select(opus, 401));
  assert.ok(capture.select(capture.parseRequest(makeRequest([
    ...bytes(5, [1]), ...format(16, 251),
    ...context(Array(90).fill(7))])), 401));
  const largeModified = [0x10, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0x7f];
  const withLargeFormatField = capture.parseRequest(makeRequest([
    ...bytes(5, [1]), ...format(16, 251),
    ...bytes(17, [...number(1, 401), ...largeModified]),
    ...context(Array(90).fill(7))]));
  assert.ok(capture.select(withLargeFormatField, 401));
  assert.equal(capture.parseRequest(makeRequest(payload, {
    url: "https://rr1.googlevideo.com.evil.invalid/videoplayback?sabr=1"})), null);
  assert.equal(capture.parseRequest(makeRequest([
    ...bytes(5, [1]), ...format(16, 140), ...format(17, 401),
    ...context(Array(10).fill(7))])), null);
  assert.equal(capture.parseRequest(makeRequest(payload.slice(0, -1))), null);
}
