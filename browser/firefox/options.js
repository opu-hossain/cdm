const api = typeof browser !== "undefined" ? browser : chrome;
const form = document.getElementById("filters");
const status = document.getElementById("status");
const save = document.getElementById("save");
let loaded = false; // Options document owns this state.
save.disabled = true;
api.storage.sync.get(["interceptionFilters", "siteExclusions", "mediaDetection"]).then(stored => {
  const filters = CdmFilters.normalize(stored.interceptionFilters);
  document.getElementById("minSizeBytes").value = String(filters.minSizeBytes);
  for (const key of CdmFilters.keys)
    document.getElementById(key).value = filters[key].join(", ");
  document.getElementById("siteExclusions").value = CdmFilters.normalizeSites(stored.siteExclusions).join(", ");
  document.getElementById("mediaDetection").checked = stored.mediaDetection === true;
  loaded = true;
  save.disabled = false;
}).catch(() => { status.textContent = "Could not load saved filters or exclusions. Reopen this page to retry."; });
form.addEventListener("submit", async event => {
  event.preventDefault();
  if (!loaded) return;
  try {
    const size = document.getElementById("minSizeBytes").value;
    if (!/^\d+$/.test(size)) throw new Error("Enter a nonnegative whole number of bytes");
    const input = {minSizeBytes: Number(size)};
    for (const key of CdmFilters.keys)
      input[key] = document.getElementById(key).value.split(",").map(s => s.trim()).filter(Boolean);
    const filters = CdmFilters.normalize(input);
    const siteExclusions = CdmFilters.normalizeSites(document.getElementById("siteExclusions").value
      .split(",").map(s => s.trim()).filter(Boolean));
    save.disabled = true;
    const mediaDetection = document.getElementById("mediaDetection").checked;
    if (mediaDetection && !await api.permissions.request({permissions: ["webRequest"],
        origins: ["http://*/*", "https://*/*"]}))
      throw new Error("Media detection needs response-header permissions");
    await api.storage.sync.set({interceptionFilters: filters, siteExclusions, mediaDetection});
    await refreshMedia();
    status.textContent = "Filters and exclusions saved. Explicit menu choices bypass them.";
  } catch (error) {
    status.textContent = error.message || "Could not save filters";
  } finally { save.disabled = false; }
});

const mediaList = document.getElementById("mediaCandidates");
const mediaStatus = document.getElementById("mediaStatus");
const mediaOffer = document.getElementById("mediaOffer");
async function refreshMedia() {
  mediaOffer.disabled = true;
  mediaList.replaceChildren();
  try {
    const stored = await api.storage.sync.get("mediaDetection");
    if (stored.mediaDetection !== true) {
      mediaStatus.textContent = "Enable media detection and click Save filters and exclusions first.";
      return;
    }
    if (!await api.permissions.contains({permissions: ["webRequest"],
        origins: ["http://*/*", "https://*/*"]})) {
      mediaStatus.textContent = "Media detection permission is missing. Save again to request it.";
      return;
    }
    const candidates = await api.runtime.sendMessage({type: "cdm_media_list"});
    if (!Array.isArray(candidates) || candidates.length > 64)
      throw new Error("Could not load detected media");
    for (const candidate of candidates) {
      const option = document.createElement("option");
      option.value = candidate.id;
      option.textContent = `[${candidate.kind}, tab ${candidate.tabId}] ${candidate.url}`;
      mediaList.appendChild(option);
    }
    mediaOffer.disabled = candidates.length === 0;
    mediaStatus.textContent = candidates.length ? "Select a candidate to offer to cdm."
        : "No direct media candidates retained. Use the in-player picker for YouTube.";
  } catch (_) {
    mediaStatus.textContent = "Could not load detected media. Reopen this page to retry.";
  }
}
document.getElementById("mediaRefresh").addEventListener("click", refreshMedia);
mediaOffer.addEventListener("click", async () => {
  if (!mediaList.value) return;
  mediaOffer.disabled = true;
  try {
    const result = await api.runtime.sendMessage({type: "cdm_media_offer", id: mediaList.value});
    if (!result?.ok) throw new Error("Candidate expired or could not be offered");
    await refreshMedia();
    mediaStatus.textContent = "Offered to cdm. Review its confirmation popup.";
  } catch (_) {
    mediaOffer.disabled = false;
    mediaStatus.textContent = "Could not offer media. Refresh and try again.";
  }
});
