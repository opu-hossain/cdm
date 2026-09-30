// Serialized into the page's MAIN world on an explicit picker click.
// Return only direct, already signed, muxed formats; never eval player source.
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
    if (typeof key === "string" && /^[A-Za-z0-9_-]{10,128}$/.test(key)) {
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
              Array.isArray(android.streamingData?.formats) &&
              android.streamingData.formats.some(format => format?.url))
            streaming = android.streamingData;
        }
      } catch (_) { /* A failed optional probe leaves the page's own formats. */ }
      finally { clearTimeout(timeout); }
    }
    const title = typeof response.videoDetails.title === "string"
      ? response.videoDetails.title.slice(0, 180) : "Video";
    const formats = [];
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
    }
    return {videoId, title, formats};
  }
};
