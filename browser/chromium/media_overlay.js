// Runs only on pages granted media-detection host access. This document owns its UI state.
(() => {
  const api = typeof browser !== "undefined" ? browser : chrome;
  const pageId = crypto.randomUUID();
  let host, button, panel, title, rows = [], site = {title: "", formats: []};
  let refreshing = false, rendered = "";

  function playingVideo() {
    let chosen = null, area = 0;
    for (const video of document.querySelectorAll("video")) {
      if (video.paused || video.ended) continue;
      const rect = video.getBoundingClientRect();
      const size = rect.width * rect.height;
      if (rect.width >= 160 && rect.height >= 90 && rect.bottom > 0 &&
          rect.top < (window.innerHeight || 100000) && size > area) {
        chosen = video;
        area = size;
      }
    }
    return chosen;
  }

  function ensureUi() {
    if (host) return;
    host = document.createElement("div");
    host.style.position = "fixed";
    host.style.zIndex = "2147483647";
    const shadow = host.attachShadow({mode: "closed"});
    const style = document.createElement("style");
    style.textContent = "button{font:13px sans-serif;cursor:pointer;border:0;border-radius:5px;padding:8px 10px;background:#2276a5;color:#fff;box-shadow:0 2px 8px #0008}" +
      "button:hover,button:focus{background:#155980}" +
      ".panel{width:280px;max-height:260px;overflow:auto;margin-top:4px;padding:8px;background:#20262c;color:#fff;border-radius:5px;box-shadow:0 2px 12px #0009;font:13px sans-serif}" +
      ".panel[hidden]{display:none}.title{overflow-wrap:anywhere;margin:0 0 6px}" +
      ".panel button{display:block;width:100%;margin:3px 0;text-align:left;overflow-wrap:anywhere}";
    button = document.createElement("button");
    button.type = "button";
    button.textContent = "Download with cdm";
    button.addEventListener("click", () => { panel.hidden = !panel.hidden; });
    panel = document.createElement("div");
    panel.className = "panel";
    panel.hidden = true;
    title = document.createElement("p");
    title.className = "title";
    shadow.append(style, button, panel);
    document.documentElement.appendChild(host);
  }

  function position() {
    if (!host || (!rows.length && !site.formats.length)) return;
    const video = playingVideo();
    if (!video) { host.style.display = "none"; return; }
    const rect = video.getBoundingClientRect();
    host.style.left = `${Math.max(0, rect.right - 170)}px`;
    host.style.top = `${Math.max(0, rect.top + 10)}px`;
    host.style.display = "block";
  }

  function render() {
    title.textContent = (site.title || document.title || "Video").slice(0, 120);
    panel.replaceChildren(title);
    for (const item of rows) {
      const choice = document.createElement("button");
      choice.type = "button";
      choice.textContent = `${item.kind.toUpperCase()} · ${item.filename || "media"}`;
      choice.addEventListener("click", async () => {
        choice.disabled = true;
        try {
          const result = await api.runtime.sendMessage({type: "cdm_media_offer_tab", id: item.id});
          button.textContent = result?.ok ? "Offered to cdm" : "Could not offer media";
          if (result?.ok) panel.hidden = true;
        } catch (_) {
          button.textContent = "Could not offer media";
        } finally { choice.disabled = false; }
      });
      panel.appendChild(choice);
    }
    for (const format of site.formats) {
      const choice = document.createElement("button");
      choice.type = "button";
      const size = Number(format.size_bytes);
      const sizeLabel = size > 0
        ? (format.size_estimated ? "~" : "") + (size / 1000000).toFixed(1) + " MB"
        : "size unknown";
      choice.textContent = (format.height || "?") + "p · " + format.ext.toUpperCase() +
        " · " + sizeLabel + (format.has_audio ? "" : " · video only");
      choice.addEventListener("click", async () => {
        choice.disabled = true;
        try {
          const result = await api.runtime.sendMessage(
            {type: "cdm_site_select_tab", id: format.id, page_id: pageId});
          button.textContent = result?.ok ? "Format selected" : "Could not select format";
          if (result?.ok) panel.hidden = true;
        } catch (_) {
          button.textContent = "Could not select format";
        } finally { choice.disabled = false; }
      });
      panel.appendChild(choice);
    }
  }

  async function refresh() {
    if (refreshing) return;
    if (!playingVideo()) {
      if (host) host.style.display = "none";
      if (panel) panel.hidden = true;
      return;
    }
    refreshing = true;
    try {
      const [found, siteResult] = await Promise.all([
        api.runtime.sendMessage({type: "cdm_media_list_tab"}),
        api.runtime.sendMessage({type: "cdm_site_probe_tab", page_id: pageId})]);
      rows = Array.isArray(found) ? found.filter(item =>
        typeof item.id === "string" && item.id.length <= 128 &&
        ["hls", "dash", "video"].includes(item.kind) &&
        typeof item.filename === "string" && item.filename.length <= 511).slice(0, 64) : [];
      site = siteResult && typeof siteResult.title === "string" &&
        Array.isArray(siteResult.formats) ? siteResult : {title: "", formats: []};
      if (!rows.length && !site.formats.length) {
        rendered = "";
        if (host) host.style.display = "none";
        if (panel) panel.hidden = true;
        return;
      }
      ensureUi();
      const signature = JSON.stringify([document.title, rows, site]);
      if (signature !== rendered) {
        button.textContent = "Download with cdm";
        render();
        rendered = signature;
      }
      position();
    } catch (_) {
      rows = [];
      site = {title: "", formats: []};
      if (host) host.style.display = "none";
    } finally { refreshing = false; }
  }

  document.addEventListener("play", refresh, true);
  document.addEventListener("pause", refresh, true);
  document.addEventListener("ended", refresh, true);
  window.addEventListener("scroll", position, true);
  window.addEventListener("resize", position);
  setInterval(refresh, 2000);
  refresh();
})();
