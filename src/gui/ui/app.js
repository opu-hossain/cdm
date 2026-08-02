window.addEventListener("DOMContentLoaded", () => {
  if (window.c_ready) window.c_ready();
});

const downloadsContainer = document.getElementById("downloads-container");

async function pickFolder() {
  const folder = await window.c_pick_folder();
  if (folder) document.getElementById("dest").value = folder;
}

async function startDownload() {
  const btn = document.getElementById("download-btn");
  const url = document.getElementById("url").value;
  if (!url) return;

  btn.disabled = true;
  btn.textContent = "Adding...";

  try {
    const dest = document.getElementById("dest").value;
    const cookie = document.getElementById("opt-cookie").value;
    const referrer = document.getElementById("opt-referrer").value;
    const headers = document.getElementById("opt-headers").value;
    const sha256 = document.getElementById("opt-sha256").value;
    const speedLimitRaw = document.getElementById("opt-speed-limit").value;
    const speedLimit = speedLimitRaw ? parseInt(speedLimitRaw, 10) : 0;

    const resultJson = await window.c_add_download(
      url,
      dest,
      cookie,
      referrer,
      headers,
      sha256,
      speedLimit,
    );
    const result = JSON.parse(resultJson);
    if (!result.ok) {
      alert(
        "Failed to add download — check the destination folder or daemon connection.",
      );
      return;
    }

    document.getElementById("url").value = "";
    document.getElementById("opt-cookie").value = "";
    document.getElementById("opt-referrer").value = "";
    document.getElementById("opt-headers").value = "";
    document.getElementById("opt-sha256").value = "";
    document.getElementById("opt-speed-limit").value = "";
  } finally {
    btn.disabled = false;
    btn.textContent = "DOWNLOAD";
  }
}

function actionBtn(action, id) {
  if (action === "cancel") {
    if (!confirm("Cancel this download? The partial file will be deleted.")) {
      return;
    }
  }
  window.c_action_download(action, id);
}

window.setConnectionState = function (connected) {
  document.getElementById("connection-banner").style.display = connected
    ? "none"
    : "block";
};

const rowElements = new Map(); // id -> refs, lives for the page's lifetime
const expandedRows = new Set();
const loadingDetails = new Set();

function renderButtons(status, id) {
  const isActivelike = status === "ACTIVE" || status === "QUEUED";
  const isResumable = status === "PAUSED" || status === "ERROR";
  const isTerminal = status === "DONE" || status === "CANCELED";

  let html = "";
  if (isActivelike)
    html += `<button class="secondary" onclick="actionBtn('pause', ${id})">Pause</button>`;
  if (isResumable)
    html += `<button class="secondary" onclick="actionBtn('resume', ${id})">Resume</button>`;
  if (!isTerminal)
    html += `<button class="danger" onclick="actionBtn('cancel', ${id})">Cancel</button>`;
  return html;
}

function detailLine(key, value, masked) {
  if (!value) {
    return `<div class="detail-line"><span class="detail-key">${key}</span><span class="detail-empty">not set</span></div>`;
  }
  if (masked) {
    return `<div class="detail-line">
            <span class="detail-key">${key}</span>
            <span class="detail-val" data-masked="true">••••••••</span>
            <button class="reveal-btn" onclick="toggleReveal(this, '${escapeAttr(value)}')">show</button>
        </div>`;
  }
  return `<div class="detail-line"><span class="detail-key">${key}</span><span class="detail-val">${escapeHtml(value)}</span></div>`;
}

function escapeHtml(s) {
  const d = document.createElement("div");
  d.textContent = s;
  return d.innerHTML;
}
function escapeAttr(s) {
  return s.replace(/'/g, "\\'").replace(/\n/g, "\\n");
}

function toggleReveal(btn, value) {
  const span = btn.previousElementSibling;
  const revealed = span.dataset.masked === "false";
  span.textContent = revealed ? "••••••••" : value;
  span.dataset.masked = revealed ? "true" : "false";
  btn.textContent = revealed ? "show" : "hide";
}

const detailsCache = new Map(); // id -> { cookie, referrer, extra_headers, expected_sha256, speed_limit_bps }

function detailLine(key, value, masked) {
  if (!value) {
    return `<div class="detail-line"><span class="detail-key">${key}</span><span class="detail-empty">not set</span></div>`;
  }
  if (masked) {
    return `<div class="detail-line">
            <span class="detail-key">${key}</span>
            <span class="detail-val" data-masked="true">••••••••</span>
            <button class="reveal-btn" onclick="toggleReveal(this, '${escapeAttr(value)}')">show</button>
        </div>`;
  }
  return `<div class="detail-line"><span class="detail-key">${key}</span><span class="detail-val">${escapeHtml(value)}</span></div>`;
}

function escapeHtml(s) {
  const d = document.createElement("div");
  d.textContent = s;
  return d.innerHTML;
}
function escapeAttr(s) {
  return s.replace(/'/g, "\\'").replace(/\n/g, "\\n");
}

function toggleReveal(btn, value) {
  const span = btn.previousElementSibling;
  const revealed = span.dataset.masked === "false";
  span.textContent = revealed ? "••••••••" : value;
  span.dataset.masked = revealed ? "true" : "false";
  btn.textContent = revealed ? "show" : "hide";
}

function renderDetailsContent(details) {
  const speedLimit =
    details.speed_limit_bps > 0
      ? `${details.speed_limit_bps.toLocaleString()} B/s`
      : "";
  return `
        ${detailLine("Cookie", details.cookie, true)}
        ${detailLine("Referrer", details.referrer, false)}
        ${detailLine("Headers", details.extra_headers, false)}
        ${detailLine("SHA-256", details.expected_sha256, false)}
        ${detailLine("Speed limit", speedLimit, false)}
    `;
}

async function toggleDetails(id) {
  const refs = rowElements.get(id);
  if (!refs) return;

  refs.detailsOpen = !refs.detailsOpen;
  if (refs.detailsOpen) {
    expandedRows.add(id);
  } else {
    expandedRows.delete(id);
    loadingDetails.delete(id);
  }
  refs.detailsEl.classList.toggle("open", refs.detailsOpen);
  refs.toggleEl.textContent = refs.detailsOpen
    ? "Hide details ▴"
    : "Show details ▾";

  if (!refs.detailsOpen) return; // collapsing needs no fetch

  // Lazy fetch — only hits the daemon the first time a row is opened,
  // then serves from detailsCache on subsequent opens. This is the
  // memory/bandwidth win: nothing heavy is fetched until the user asks.
  if (detailsCache.has(id)) {
    refs.detailsEl.innerHTML = renderDetailsContent(detailsCache.get(id));
    return;
  }

  loadingDetails.add(id);
  refs.detailsEl.innerHTML =
    '<div class="detail-line"><span class="detail-empty">Loading…</span></div>';
  try {
    const resultJson = await Promise.race([
      window.c_get_details(id),
      new Promise((_, reject) => {
        setTimeout(() => reject(new Error("details request timed out")), 5000);
      }),
    ]);
    const result =
      typeof resultJson === "string" ? JSON.parse(resultJson) : resultJson;
    if (!result || !result.ok) {
      throw new Error("details request rejected");
    }
    detailsCache.set(id, result);
    // Re-check in case the panel was closed while the fetch was in flight.
    if (refs.detailsOpen) {
      refs.detailsEl.innerHTML = renderDetailsContent(result);
    }
  } catch (err) {
    if (refs.detailsOpen) {
      refs.detailsEl.innerHTML =
        '<div class="detail-line"><span class="detail-empty">Failed to load details</span></div>';
    }
  } finally {
    loadingDetails.delete(id);
  }
}

function createRow(dl) {
  const el = document.createElement("div");
  el.className = "download-item";
  el.innerHTML = `
        <div class="dl-header">
            <span class="dl-title">ID: ${dl.id} | ${dl.url}</span>
            <span class="status-badge status-${dl.status}">${dl.status}</span>
        </div>
        <div class="progress-bar">
            <div class="progress-fill"></div>
        </div>
        <div class="dl-controls">
            <span class="status-text"></span>
            <div class="btn-group"></div>
        </div>
        <button class="row-details-toggle" onclick="toggleDetails(${dl.id})">Show details ▾</button>
        <div class="row-details" id="details-${dl.id}"></div>
    `;
  return {
    el,
    badgeEl: el.querySelector(".status-badge"),
    fillEl: el.querySelector(".progress-fill"),
    pctEl: el.querySelector(".status-text"),
    btnGroupEl: el.querySelector(".btn-group"),
    detailsEl: el.querySelector(".row-details"),
    toggleEl: el.querySelector(".row-details-toggle"),
    lastStatus: null,
    detailsOpen: false,
  };
}

function updateRow(refs, dl) {
  const pct = dl.status === "DONE" ? 100 : Math.round(dl.progress * 100);
  refs.fillEl.style.width = pct + "%";
  refs.pctEl.textContent = pct + "%";

  if (refs.lastStatus !== dl.status) {
    refs.badgeEl.textContent = dl.status;
    refs.badgeEl.className = `status-badge status-${dl.status}`;
    refs.btnGroupEl.innerHTML = renderButtons(dl.status, dl.id);
    refs.lastStatus = dl.status;
  }
  // No details rendering here at all now — purely lazy, driven by
  // toggleDetails() on click.
}

window.updateState = function (downloads) {
  const seen = new Set();
  const fragment = document.createDocumentFragment();

  for (const dl of Array.isArray(downloads) ? downloads : []) {
    seen.add(dl.id);
    let refs = rowElements.get(dl.id);
    if (!refs) {
      refs = createRow(dl);
      rowElements.set(dl.id, refs);
    }
    updateRow(refs, dl);

    if (expandedRows.has(dl.id)) {
      refs.detailsOpen = true;
      refs.detailsEl.classList.add("open");
      refs.toggleEl.textContent = "Hide details ▴";
      if (detailsCache.has(dl.id)) {
        refs.detailsEl.innerHTML = renderDetailsContent(detailsCache.get(dl.id));
      } else if (loadingDetails.has(dl.id)) {
        refs.detailsEl.innerHTML =
          '<div class="detail-line"><span class="detail-empty">Loading…</span></div>';
      }
    }

    fragment.appendChild(refs.el);
  }

  for (const [id, refs] of rowElements.entries()) {
    if (!seen.has(id)) {
      rowElements.delete(id);
      expandedRows.delete(id);
      loadingDetails.delete(id);
      if (refs.el.parentNode) {
        refs.el.parentNode.removeChild(refs.el);
      }
    }
  }

  downloadsContainer.innerHTML = "";
  downloadsContainer.appendChild(fragment);

  if (rowElements.size === 0) {
    downloadsContainer.innerHTML = '<div class="empty-state">No downloads yet.</div>';
  }
};
