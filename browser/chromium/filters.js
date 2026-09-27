// Pure validation/matching shared by each extension's background and options page.
globalThis.CdmFilters = Object.freeze({
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
