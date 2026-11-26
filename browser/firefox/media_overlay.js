// Runs only on pages granted media-detection host access. This document owns its UI state.
(() => {
  if (globalThis.__cdmMediaOverlay) {
    globalThis.__cdmMediaOverlay.refresh();
    return;
  }
  const api = typeof browser !== "undefined" ? browser : chrome;
  let host, button, panel, title, rows = [];
  let refreshing = false, rendered = "";
  let youtubePage = null, youtubeLoading = false, youtubePending = null;
  let nextYouTubeProbe = 0;

  function onYouTube() {
    try {
      const page = new URL(location.href);
      return ["www.youtube.com", "m.youtube.com", "www.youtube-nocookie.com"]
        .includes(page.hostname) &&
        (page.pathname === "/watch" || page.pathname.startsWith("/embed/"));
    } catch (_) { return false; }
  }

  function formatSize(bytes) {
    if (!Number.isSafeInteger(bytes) || bytes <= 0) return "size unknown";
    if (bytes >= 1048576) return `${(bytes / 1048576).toFixed(1)} MB`;
    return `${(bytes / 1024).toFixed(1)} KB`;
  }

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
    if (host) {
      if (host.isConnected === false) document.documentElement.appendChild(host);
      return;
    }
    host = document.createElement("div");
    host.style.position = "fixed";
    host.style.zIndex = "2147483647";
    const shadow = host.attachShadow({mode: "closed"});
    const style = document.createElement("style");
    style.textContent = "button{font:13px sans-serif;cursor:pointer;border:0;border-radius:5px;padding:8px 10px;background:#2276a5;color:#fff;box-shadow:0 2px 8px #0008;max-width:100%}" +
      "button:hover,button:focus{background:#155980}" +
      ".panel{box-sizing:border-box;width:100%;max-height:260px;overflow:auto;margin-top:4px;padding:8px;background:#20262c;color:#fff;border-radius:5px;box-shadow:0 2px 12px #0009;font:13px sans-serif}" +
      ".panel[hidden]{display:none}.title{overflow-wrap:anywhere;margin:0 0 6px}" +
      ".panel button{display:block;width:100%;margin:3px 0;text-align:left;overflow-wrap:anywhere}" +
      "button:first-of-type{display:block;margin-left:auto}";
    button = document.createElement("button");
    button.type = "button";
    button.textContent = "Download with cdm";
    button.addEventListener("click", async () => {
      const opening = panel.hidden;
      panel.hidden = !panel.hidden;
      if (!opening || !onYouTube()) return;
      await discoverYouTube();
    });
    panel = document.createElement("div");
    panel.className = "panel";
    panel.hidden = true;
    title = document.createElement("p");
    title.className = "title";
    shadow.append(style, button, panel);
    document.documentElement.appendChild(host);
    render();
  }

  async function discoverYouTube() {
    if (youtubePending) return youtubePending;
    const page = location.href;
    youtubePage = page;
    youtubeLoading = true;
    if (!rows.length) render();
    youtubePending = (async () => {
      try {
        const found = await api.runtime.sendMessage({type: "cdm_youtube_formats_tab"});
        if (location.href !== page) return;
        const next = Array.isArray(found) ? found.filter(item =>
          typeof item.id === "string" && item.id.length <= 128 &&
          item.kind === "video" && typeof item.filename === "string" &&
          item.filename.length <= 511 && typeof item.quality === "string" &&
          item.quality.length <= 32 && Number.isSafeInteger(item.totalBytes) &&
          item.totalBytes >= 0).slice(0, 16) : [];
        const changed = JSON.stringify(next) !== JSON.stringify(rows);
        rows = next;
        if (changed || !rows.length) render();
      } catch (_) {
        if (location.href === page) { rows = []; render(); }
      } finally {
        youtubeLoading = false;
        youtubePending = null;
        nextYouTubeProbe = location.href === page
          ? Date.now() + (rows.length ? 10000 : 2000) : 0;
        if (location.href === page && !rows.length) render();
      }
    })();
    return youtubePending;
  }

  function position() {
    if (!host) return;
    const video = playingVideo();
    if (!video) { host.style.display = "none"; return; }
    const rect = video.getBoundingClientRect();
    const viewportWidth = window.innerWidth || rect.right;
    host.style.right = `${Math.max(8, viewportWidth - Math.min(rect.right - 8, viewportWidth - 8))}px`;
    host.style.width = `${Math.min(296, Math.max(140, rect.width - 16), viewportWidth - 16)}px`;
    host.style.top = `${Math.max(0, rect.top + 8)}px`;
    host.style.display = "block";
  }

  function render() {
    const pageTitle = document.title || "Video";
    title.textContent = (onYouTube()
      ? pageTitle.replace(/^\(\d+\)\s*/, "").replace(/\s+-\s+YouTube$/, "")
      : pageTitle).slice(0, 120);
    panel.replaceChildren(title);
    for (const [index, item] of rows.entries()) {
      const choice = document.createElement("button");
      choice.type = "button";
      choice.textContent = item.quality
        ? `${item.quality} · ${formatSize(item.totalBytes)} · ${item.filename}`
        : `Video${rows.length > 1 ? ` ${index + 1}` : ""} · ${item.kind === "video" ? "Original quality" : "Automatic quality"} · ${formatSize(item.totalBytes)}`;
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
    if (rows.some(item => item.kind === "hls" || item.kind === "dash")) {
      const note = document.createElement("p");
      note.textContent = "Streaming video: cdm selects the available quality automatically and prepares the video file.";
      panel.appendChild(note);
    }
    if (!rows.length) {
      const unavailable = document.createElement("p");
      unavailable.textContent = youtubeLoading ? "Checking YouTube formats…" :
        youtubePage ? "Waiting for playback information… Keep the video playing; this list updates automatically." :
        "No downloadable video detected yet. Protected or unsupported streams cannot be offered.";
      panel.appendChild(unavailable);
    }
  }

  async function refresh() {
    if (!playingVideo()) {
      if (host) host.style.display = "none";
      if (panel) panel.hidden = true;
      return;
    }
    ensureUi();
    position();
    if (youtubePage && (!onYouTube() || location.href !== youtubePage)) {
      youtubePage = null;
      youtubeLoading = false;
      panel.hidden = true;
      rows = [];
      nextYouTubeProbe = 0;
    }
    if (onYouTube()) {
      if (Date.now() >= nextYouTubeProbe) await discoverYouTube();
      return;
    }
    if (refreshing) return;
    refreshing = true;
    try {
      const found = await api.runtime.sendMessage({type: "cdm_media_list_tab"});
      if (youtubePage && !panel.hidden) return;
      rows = Array.isArray(found) ? found.filter(item =>
        typeof item.id === "string" && item.id.length <= 128 &&
        ["hls", "dash", "video"].includes(item.kind) &&
        typeof item.filename === "string" && item.filename.length <= 511).slice(0, 64) : [];
      const signature = JSON.stringify([document.title, rows]);
      if (signature !== rendered) {
        button.textContent = "Download with cdm";
        render();
        rendered = signature;
      }
      position();
    } catch (_) {
      if (youtubePage && !panel.hidden) return;
      rows = [];
      render();
      button.textContent = "Download with cdm";
      position();
    } finally { refreshing = false; }
  }

  globalThis.__cdmMediaOverlay = {refresh};
  document.addEventListener("play", refresh, true);
  document.addEventListener("pause", refresh, true);
  document.addEventListener("ended", refresh, true);
  window.addEventListener("scroll", position, true);
  window.addEventListener("resize", position);
  setInterval(refresh, 2000);
  refresh();
})();
