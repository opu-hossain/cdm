// Pure validation/matching shared by each extension's background and options page.
globalThis.CdmFilters = Object.freeze({
  mediaKind(url, mime = "") {
    let pathname;
    try {
      const parsed = new URL(url);
      if (!["http:", "https:"].includes(parsed.protocol)) return "";
      pathname = decodeURIComponent(parsed.pathname).toLowerCase();
    } catch (_) { return ""; }
    const type = mime.split(";", 1)[0].trim().toLowerCase();
    if (/\.(?:m4s|cmfv|cmfa|ts|m2ts)$/.test(pathname)) return "";
    if (pathname.endsWith(".m3u8") || type === "application/vnd.apple.mpegurl" ||
        type === "application/x-mpegurl") return "hls";
    if (pathname.endsWith(".mpd") || type === "application/dash+xml") return "dash";
    return /\.(mp4|webm|ogv|ogg|mov|m4v|mkv|avi)$/.test(pathname) ||
      /^video\/[a-z0-9.+-]+$/.test(type) ? "video" : "";
  },
  playbackFragment(url, kind, status, headers = [], hasManifest = false) {
    if (kind !== "video") return false;
    let path, parsed;
    try { parsed = new URL(url); path = decodeURIComponent(parsed.pathname).toLowerCase(); }
    catch (_) { return true; }
    if (/(?:^|[\/._-])(?:segment|chunk|frag|part)(?:[\/._-]|[0-9]|$)/.test(path))
      return true;
    if (status === 206 || headers.some(header =>
        header.name.toLowerCase() === "content-range")) {
      // A standalone MP4/WebM often plays via byte ranges. Do not confuse it
      // with a manifest segment or a URL that names one specific range.
      return hasManifest || !/\.(mp4|webm|ogv|ogg|mov|m4v|mkv|avi)$/.test(path) ||
        [...parsed.searchParams.keys()].some(key => /^(range|bytes|bytestart|byteend|start|end|sq)$/i.test(key)) ||
        this.mediaSize(kind, status, headers) === 0;
    }
    const length = headers.find(header =>
      header.name.toLowerCase() === "content-length")?.value;
    return hasManifest && /^[0-9]+$/.test(length || "") &&
      Number(length) < 512 * 1024;
  },
  mediaSize(kind, status, headers = []) {
    if (kind !== "video") return 0;
    let value;
    if (status === 206) {
      const range = headers.find(h => h.name.toLowerCase() === "content-range")?.value;
      const parts = /^bytes ([0-9]+)-([0-9]+)\/([0-9]+)$/i.exec(range || "");
      if (!parts || Number(parts[1]) > Number(parts[2]) ||
          Number(parts[2]) >= Number(parts[3])) return 0;
      value = Number(parts[3]);
    } else if (status === 200) {
      value = Number(headers.find(h => h.name.toLowerCase() === "content-length")?.value);
    }
    return Number.isSafeInteger(value) && value > 0 ? value : 0;
  },
  normalizeSites(input = []) {
    if (!Array.isArray(input) || input.length > 64)
      throw new Error("Use at most 64 excluded hostnames");
    const sites = [...new Set(input.map(value => {
      if (typeof value !== "string") throw new Error("Enter hostname patterns");
      const pattern = value.trim().toLowerCase().replace(/\.$/, "");
      const hostname = pattern.startsWith("*.") ? pattern.slice(2) : pattern;
      if (!hostname || hostname.length > 253 || hostname.split(".").some(label =>
          !/^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?$/.test(label)))
        throw new Error("Use ASCII/punycode hostnames, optionally prefixed with *.");
      return pattern;
    }))];
    if (new TextEncoder().encode(JSON.stringify(sites)).length > 6000)
      throw new Error("Excluded hostnames exceed the sync storage limit");
    return sites;
  },
  excluded(url, sites) {
    const hostname = new URL(url).hostname.toLowerCase().replace(/\.$/, "");
    return sites.some(pattern => pattern.startsWith("*.")
      ? hostname === pattern.slice(2) || hostname.endsWith(pattern.slice(1))
      : hostname === pattern);
  },
  keys: Object.freeze(["extensionsAllow", "extensionsDeny", "mimeAllow", "mimeDeny"]),
  normalize(input = {}) {
    if (!input || typeof input !== "object" || Array.isArray(input))
      throw new Error("Invalid interception settings");
    const minSizeBytes = input.minSizeBytes === undefined ? 0 : input.minSizeBytes;
    if (!Number.isSafeInteger(minSizeBytes) || minSizeBytes < 0)
      throw new Error("Minimum size must be a nonnegative whole number of bytes");
    const result = {minSizeBytes};
    for (const key of this.keys) {
      const values = input[key] === undefined ? [] : input[key];
      if (!Array.isArray(values) || values.length > 64)
        throw new Error("Each list supports at most 64 entries");
      result[key] = [...new Set(values.map(value => {
        if (typeof value !== "string") throw new Error("List entries must be text");
        const token = value.trim().toLowerCase();
        if (key.startsWith("extensions")) {
          const extension = token.replace(/^\./, "");
          if (!/^[a-z0-9][a-z0-9._+-]{0,63}$/.test(extension))
            throw new Error("Use extension names such as zip or tar.gz");
          return extension;
        }
        if (token !== "*/*" && !/^[a-z0-9!#$&^_.+-]+\/(?:[a-z0-9!#$&^_.+-]+|\*)$/.test(token))
          throw new Error("Use MIME types such as application/pdf or video/*");
        if (token.length > 128) throw new Error("MIME entry is too long");
        return token;
      }))];
    }
    if (new TextEncoder().encode(JSON.stringify(result)).length > 6000)
      throw new Error("Interception settings exceed the sync storage limit");
    return result;
  },
  reason(item, filters) {
    if (filters.minSizeBytes > 0 &&
        (!Number.isFinite(item.totalBytes) || item.totalBytes <= 0))
      return "size is unknown";
    if (filters.minSizeBytes > 0 && item.totalBytes < filters.minSizeBytes)
      return "below the minimum size";
    let name = (item.filename || "").split(/[\\/]/).pop();
    if (!name) {
      try {
        name = new URL(item.finalUrl || item.url).pathname.split("/").pop();
        try { name = decodeURIComponent(name); } catch (_) { /* Match raw name. */ }
      } catch (_) { name = ""; }
    }
    name = name.toLowerCase();
    const mime = (item.mime || "").split(";", 1)[0].trim().toLowerCase();
    const extensionMatches = list => list.some(ext => name.endsWith(`.${ext}`));
    const mimeMatches = list => !!mime && list.some(type => type === mime ||
      type === "*/*" || (type.endsWith("/*") && mime.startsWith(type.slice(0, -1))));
    if (extensionMatches(filters.extensionsDeny) || mimeMatches(filters.mimeDeny))
      return "file type is denied";
    if ((filters.extensionsAllow.length || filters.mimeAllow.length) &&
        !extensionMatches(filters.extensionsAllow) && !mimeMatches(filters.mimeAllow))
      return "file type is not allowed";
    return "";
  }
});
