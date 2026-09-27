const api = typeof browser !== "undefined" ? browser : chrome;
const form = document.getElementById("filters");
const status = document.getElementById("status");
const save = document.getElementById("save");
let loaded = false; // Options document owns this state.
save.disabled = true;
api.storage.sync.get("interceptionFilters").then(stored => {
  const filters = CdmFilters.normalize(stored.interceptionFilters);
  document.getElementById("minSizeBytes").value = String(filters.minSizeBytes);
  for (const key of CdmFilters.keys)
    document.getElementById(key).value = filters[key].join(", ");
  loaded = true;
  save.disabled = false;
}).catch(() => { status.textContent = "Could not load saved filters. Reopen this page to retry."; });
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
    save.disabled = true;
    await api.storage.sync.set({interceptionFilters: filters});
    status.textContent = "Filters saved. Explicit menu choices bypass these filters.";
  } catch (error) {
    status.textContent = error.message || "Could not save filters";
  } finally { save.disabled = false; }
});
