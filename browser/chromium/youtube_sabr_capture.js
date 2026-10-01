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
        if (!length || length.value === null ||
            length.value > bytes.length - length.end) return null;
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

  function appendBytes(out, field, value) {
    function varint(value) {
      do {
        const byte = value % 128;
        value = Math.floor(value / 128);
        out.push(byte | (value ? 128 : 0));
      } while (value);
    }
    varint(field * 8 + 2);
    varint(value.length);
    for (const byte of value) out.push(byte);
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
      if (length > 128 * 1024) return null;
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
    let audio = null;
    for (const part of parts) {
      if (![2, 16, 17].includes(part.field) || part.wire !== 2 ||
          part.value.length > 560) continue;
      const id = fields(part.value)?.find(field => field.field === 1 &&
        field.wire === 0)?.value;
      if (!Number.isInteger(id) || id < 1 || id > 100000) continue;
      if ((part.field === 2 || part.field === 16) &&
          [140, 251, 250, 249].includes(id)) {
        // Prefer the audio already initialized by the player (its chosen language).
        if (!audio || (part.field === 2 && audio.field !== 2))
          audio = {field: part.field, value: part.value};
      } else if (part.field === 17 && formats.size < 128)
        formats.set(id, part.value);
    }
    if (!audio) return null;
    return {url: details.url, config, context, audio: audio.value, formats};
  }

  function advertisedFormat(itag, metadata) {
    // Same FormatId fields used by the native SABR writer: itag, lastModified, xtags.
    if (!metadata || typeof metadata.lastModified !== "string" ||
        !/^[0-9]{1,20}$/.test(metadata.lastModified)) return null;
    const modified = BigInt(metadata.lastModified);
    if (modified <= 0n || modified > 18446744073709551615n) return null;
    const tags = new TextEncoder().encode(metadata.xtags || "");
    if (tags.length > 512) return null;
    const out = [];
    function number(field, value) {
      out.push(field * 8);
      do {
        const byte = Number(value & 127n);
        value >>= 7n;
        out.push(byte | (value ? 128 : 0));
      } while (value);
    }
    number(1, BigInt(itag));
    number(2, modified);
    if (tags.length) appendBytes(out, 3, tags);
    return new Uint8Array(out);
  }

  function select(session, itag, metadata) {
    if (!session || !Number.isInteger(itag) ||
        itag < 1 || itag > 100000) return null;
    const request = [];
    appendBytes(request, 5, session.config);
    appendBytes(request, 16, session.audio);
    const video = session.formats.get(itag) || advertisedFormat(itag, metadata);
    if (video) appendBytes(request, 17, video);
    appendBytes(request, 19, session.context);
    if (request.length > 16000) return null;
    return {url: session.url, request: encode(request)};
  }

  return {parseRequest, select};
})();
