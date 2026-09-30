// Extract only the bounded playback fields needed by cdm from a YouTube SABR
// request. The signed URL and token must stay in memory and be cleared with the tab.
globalThis.CdmYouTubeSabrCapture = (() => {
  function fields(bytes) {
    const result = [];
    for (let pos = 0; pos < bytes.length;) {
      const tag = number(bytes, pos);
      if (!tag || !tag.value || tag.value > 8192) return null;
      pos = tag.end;
      const field = Math.floor(tag.value / 8), wire = tag.value % 8;
      let value;
      if (wire === 0) {
        const decoded = number(bytes, pos);
        if (!decoded) return null;
        value = decoded.value;
        pos = decoded.end;
      } else if (wire === 2) {
        const length = number(bytes, pos);
        if (!length || length.value > bytes.length - length.end) return null;
        pos = length.end;
        value = bytes.subarray(pos, pos + length.value);
        pos += length.value;
      } else if (wire === 1 || wire === 5) {
        const size = wire === 1 ? 8 : 4;
        if (size > bytes.length - pos) return null;
        value = bytes.subarray(pos, pos + size);
        pos += size;
      } else return null;
      result.push({field, wire, value});
    }
    return result;
  }

  function number(bytes, start) {
    let value = 0;
    for (let pos = start, power = 1, count = 0;
         pos < bytes.length && count < 10; pos++, power *= 128, count++) {
      const octet = bytes[pos];
      value += (octet & 127) * power;
      if (!(octet & 128))
        return {value: Number.isSafeInteger(value) ? value : null,
          end: pos + 1};
    }
    return null;
  }

  function encode(bytes) {
    let raw = "";
    for (const octet of bytes) raw += String.fromCharCode(octet);
    return btoa(raw);
  }

  function parseRequest(details) {
    let url;
    try { url = new URL(details.url); } catch (_) { return null; }
    if (details.method !== "POST" || details.tabId < 0 ||
        url.protocol !== "https:" ||
        !url.hostname.endsWith(".googlevideo.com") ||
        url.pathname !== "/videoplayback" ||
        !Array.isArray(details.requestBody?.raw)) return null;
    const chunks = details.requestBody.raw;
    let length = 0;
    for (const chunk of chunks) {
      if (Object.prototype.toString.call(chunk.bytes) !== "[object ArrayBuffer]")
        return null;
      length += chunk.bytes.byteLength;
      if (length > 16000) return null;
    }
    if (!length || details.url.length > 2048) return null;
    const body = new Uint8Array(length);
    let offset = 0;
    for (const chunk of chunks) {
      body.set(new Uint8Array(chunk.bytes), offset);
      offset += chunk.bytes.byteLength;
    }
    const parts = fields(body);
    if (!parts) return null;
    const config = parts.find(part => part.field === 5 && part.wire === 2)?.value;
    const context = parts.find(part => part.field === 19 && part.wire === 2)?.value;
    if (!config?.length || config.length > 11000 ||
        !context?.length || context.length > 2048) return null;
    const contextFields = fields(context);
    const token = contextFields?.find(part => part.field === 2 && part.wire === 2)?.value;
    if (!token || token.length < 32 || token.length > 1024) return null;
    const formats = new Map();
    const audioFormats = new Set();
    for (const part of parts) {
      if ((part.field !== 16 && part.field !== 17) || part.wire !== 2 ||
          part.value.length > 560) continue;
      const id = fields(part.value)?.find(field => field.field === 1 &&
        field.wire === 0)?.value;
      if (!Number.isInteger(id) || id < 1 || id > 100000) continue;
      if (part.field === 16) audioFormats.add(id);
      else if (formats.size < 128) formats.set(id, encode(part.value));
    }
    if (![140, 251, 250].some(itag => audioFormats.has(itag))) return null;
    return {url: details.url, request: encode(body), formats};
  }

  function select(session, itag) {
    if (!session || !Number.isInteger(itag) ||
        itag < 1 || itag > 100000) return null;
    return {url: session.url, request: session.request};
  }

  return {parseRequest, select};
})();
