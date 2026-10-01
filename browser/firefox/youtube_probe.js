// Serialized into the page's MAIN world while a permitted YouTube video is playing.
// Return direct MP4 or advertised MP4 adaptive tracks; never eval player source.
globalThis.CdmYouTubeProbe = {
  isYouTubePage(url) {
    try {
      const page = new URL(url);
      return page.protocol === "https:" &&
        ["www.youtube.com", "m.youtube.com", "www.youtube-nocookie.com"]
          .includes(page.hostname) &&
        (page.pathname === "/watch" || page.pathname.startsWith("/embed/"));
    } catch (_) { return false; }
  },
  pageProbe: async function pageProbe() {
    const empty = {videoId: "", title: "", formats: []};
    let videoId = "";
    try {
      const page = new URL(location.href);
      if (page.hostname !== "www.youtube.com" &&
          page.hostname !== "m.youtube.com" &&
          page.hostname !== "www.youtube-nocookie.com") return empty;
      if (page.pathname === "/watch") videoId = page.searchParams.get("v") || "";
      else if (page.pathname.startsWith("/embed/"))
        videoId = page.pathname.slice(7).split("/")[0];
    } catch (_) { return empty; }
    if (!/^[A-Za-z0-9_-]{1,32}$/.test(videoId)) return empty;
    let response;
    try {
      response = document.getElementById("movie_player")?.getPlayerResponse?.() ||
        globalThis.ytInitialPlayerResponse;
    } catch (_) { return empty; }
    if (response?.videoDetails?.videoId !== videoId ||
        (response.playabilityStatus && response.playabilityStatus.status !== "OK") ||
        response.videoDetails.isLive === true ||
        response.videoDetails.isLiveContent === true) return empty;
    let streaming = response.streamingData;
    const key = globalThis.ytcfg?.get?.("INNERTUBE_API_KEY");
    if (!streaming?.adaptiveFormats?.some(format => format?.qualityLabel) &&
        typeof key === "string" && /^[A-Za-z0-9_-]{10,128}$/.test(key)) {
      const controller = new AbortController();
      const timeout = setTimeout(() => controller.abort(), 8000);
      try {
        const endpoint = new URL("/youtubei/v1/player", location.href);
        endpoint.searchParams.set("key", key);
        const result = await fetch(endpoint.href, {
          method: "POST", credentials: "omit", signal: controller.signal,
          headers: {"Content-Type": "application/json"},
          body: JSON.stringify({context: {client: {
            clientName: "ANDROID", clientVersion: "21.26.4",
            androidSdkVersion: 33, osName: "Android", osVersion: "13"
          }}, videoId})
        });
        if (result.ok) {
          const android = await result.json();
          if (android?.videoDetails?.videoId === videoId &&
              android.playabilityStatus?.status === "OK" &&
              (android.streamingData?.formats?.some(format => format?.url) ||
               (android.streamingData?.serverAbrStreamingUrl &&
                android.playerConfig?.mediaCommonConfig?.mediaUstreamerRequestConfig
                  ?.videoPlaybackUstreamerConfig &&
                Array.isArray(android.streamingData?.adaptiveFormats))))
            streaming = android.streamingData;
          if (streaming === android.streamingData)
            response = android;
        }
      } catch (_) { /* A failed optional probe leaves the page's own formats. */ }
      finally { clearTimeout(timeout); }
    }
    const title = typeof response.videoDetails.title === "string"
      ? response.videoDetails.title.slice(0, 180) : "Video";
    const formats = [];
    const offered = new Set();
    for (const format of Array.isArray(streaming?.formats) ? streaming.formats : []) {
      if (formats.length >= 16) break;
      if (!format || !Number.isInteger(format.itag) || format.itag < 0 ||
          !format.audioQuality || !format.qualityLabel ||
          typeof format.mimeType !== "string" ||
          !format.mimeType.startsWith("video/mp4") ||
          typeof format.url !== "string" ||
          !format.url.startsWith("https://") ||
          format.url.length > 8192) continue;
      const bytes = Number(format.contentLength);
      formats.push({itag: format.itag, quality: String(format.qualityLabel).slice(0, 32),
        mime: "video/mp4", url: format.url,
        totalBytes: Number.isSafeInteger(bytes) && bytes > 0 ? bytes : 0});
      offered.add(String(format.qualityLabel));
    }
    // The browser's SABR request supplies config and authorization separately.
    // getPlayerResponse often exposes adaptive metadata without either field.
    {
      const adaptive = Array.isArray(streaming?.adaptiveFormats)
        ? streaming.adaptiveFormats : [];
      const audio = adaptive.find(format => [140, 251, 250, 249].includes(format?.itag) &&
        typeof format.mimeType === "string" &&
        format.mimeType.startsWith("audio/"));
      const audioBytes = Number(audio?.contentLength);
      if (audio) for (const format of adaptive) {
        if (formats.length >= 16) break;
        if (!Number.isInteger(format?.itag) || format.itag < 1 ||
            format.itag > 100000 || !Number.isInteger(format.height) ||
            format.height < 1 || format.height > 4320 ||
            typeof format.mimeType !== "string" ||
            !format.mimeType.startsWith("video/mp4") ||
            typeof format.qualityLabel !== "string" ||
            offered.has(format.qualityLabel)) continue;
        const videoBytes = Number(format.contentLength);
        const total = Number.isSafeInteger(videoBytes) && videoBytes > 0 &&
            Number.isSafeInteger(audioBytes) && audioBytes > 0 &&
            Number.isSafeInteger(videoBytes + audioBytes)
          ? videoBytes + audioBytes : 0;
        formats.push({itag: format.itag, quality: format.qualityLabel.slice(0, 32),
          mime: "video/mp4", adaptive: true, totalBytes: total,
          lastModified: typeof format.lastModified === "string" &&
            /^[0-9]{1,20}$/.test(format.lastModified) ? format.lastModified : "",
          xtags: typeof format.xtags === "string" ? format.xtags.slice(0, 512) : ""});
        offered.add(format.qualityLabel);
      }
    }
    formats.sort((a, b) => (parseInt(b.quality, 10) || 0) -
      (parseInt(a.quality, 10) || 0));
    return {videoId, title, formats};
  }
};
