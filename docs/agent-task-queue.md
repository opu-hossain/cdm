# Agent task queue

Status + model-tier tracker for `PLAN.md`. The orchestrator reads *this*
file to pick the next task and its tier, then builds a scoped brief from
`docs/agent-task-brief-template.md` — it does not hand the executor the
whole `PLAN.md`.

Tiers: **S** = small/cheap model, spec is already fully mechanical.
**L** = strong model, real design/concurrency/research judgment needed.
Reasoning for each L is given so you can re-tier a task if it turns out
easier or harder in practice than expected — this table is a starting
estimate, not a law.

Completed tasks (0.1.1–2.1.3) are omitted; see `PLAN.md` Appendix A for the
full historical checklist and `docs/agent-log.md` for what was actually
done and why.

## Phase 0 / 1 — remaining gate only

- [ ] **0.6.3** Merge phase 0 — **S** (checklist-run, no new code; verifier confirms Appendix H post-phase list)
- [ ] **1.6.2** Merge phase 1 — **S** (same, checklist-run)

Both are blocked on the branching-model open question (see `AGENTS.md` —
work currently stays on `cdm`). Don't force these until that's resolved
with the user; skip to Phase 2 in the meantime.

## Phase 2 — Queue and automation (current phase)

- [x] **2.1.4** GUI queues tab — **S** — table/inline-edit/drag-reorder over an existing modal pattern; use `src/gui/AGENTS.md`
- [x] **2.2.1** Schedule model (daemon tick, ALWAYS/ACTIVE/IDLE states, pause/resume) — **L** — touches the scheduler tick and queue mutex together; wall-clock state machine needs a real design, not just wiring
- [x] **2.2.2** UI schedule fields — **S** — two `HH:MM` fields, inline validation
- [x] **2.3.1** Post-actions on queue completion (shutdown/sleep/command, debounce, "queue complete" detection) — **L** — cross-cuts scheduler state, has a security-sensitive default-off command path
- [x] **2.3.2** Post-action UI — **S**
- [x] **2.4.1** Tray backend — **L** — plan explicitly requires researching the current recommended Linux tray API and choosing one; new platform abstraction
- [x] **2.4.2** Wire tray to daemon — **S** — once 2.4.1's API exists, this is wiring into the existing daemon loop
- [ ] **2.5.1** Clipboard monitor — **S** — opt-in config flag, regex-shaped URL check, debounce
- [ ] **2.5.2** Batch add (CLI + GUI) — **S** — file parsing, reuses `MSG_ADD_DOWNLOAD_V2`
- [ ] **2.6.1** Categories schema — **S** — same migration pattern already used in 2.1.1
- [ ] **2.6.2** Category routing — **S** — pure function, test cases given in the plan
- [ ] **2.6.3** Editable categories UI — **S** — mirrors the queues tab; use `src/gui/AGENTS.md`
- [ ] **2.7.1** Phase 2 wrap-up (docs + full CTest/ASan/TSan gate) — **S** to draft, but the ASan/TSan run itself is the verifier's job, not a coding task

## Phase 3 — Browser parity

- [ ] **3.1.1** Request-context forwarding design doc — **L** — privacy/security threat-model writing, sets the contract every later 3.1.x task follows exactly
- [ ] **3.1.2** Extension captures context — **S** *after 3.1.1 lands* — becomes a mechanical translation of the design doc into `optional_permissions` + capture code
- [ ] **3.1.3** Native host passes context — **S** *after 3.1.1* — extend offer JSON, enforce size limits
- [ ] **3.1.4** Daemon applies context — **S**, but verifier must specifically check the "memory-only, cleared after finalize, never logged" requirement against the design doc before approving
- [ ] **3.2.1** Context menu — **S** — JS, mechanical
- [ ] **3.2.2** Filters — **S** — JS, mechanical, test cases implied by the spec
- [ ] **3.2.3** Site exclusions — **S** — JS, mechanical
- [ ] **3.3.1** Link refresh — **S** — new IPC message reusing the existing probe path
- [ ] **3.4.1** Installer support for Edge/Brave/Opera/Vivaldi — **L** — plan requires searching for current per-browser Linux config paths; research-heavy, mechanical once found
- [ ] **3.5.1** Phase 3 wrap-up — **S**

## Phase 4 — Media

- [ ] **4.1.1** Extension media detector — **S** — JS, mechanical
- [ ] **4.1.2** Native host media offer — **S**
- [ ] **4.2.1** HLS playlist parser — **L** — pure function with exhaustive fixtures (good test-loop fit), but M3U8 edge cases (AES-128, master-playlist selection, `#EXT-X-MAP`) need real parsing judgment
- [ ] **4.2.2** HLS segment downloader — **L** — reuses `worker_pool` but adds resume-state-file design and AES decryption; concurrency-adjacent
- [ ] **4.2.3** Optional ffmpeg remux — **S** — spawn wrapper + presence check, mechanical
- [ ] **4.3.1** DASH MPD parser — **L** — same reasoning as 4.2.1, XML + template substitution schemes
- [ ] **4.3.2** DASH download and merge — **S** *after 4.2.x patterns exist* — mechanical reuse of the same download+ffmpeg shape
- [ ] **4.4.1** yt-dlp integration — **S** — child-process spawn + line-format parsing, fully specified allowlist/config surface
- [ ] **4.5.1** Phase 4 wrap-up — **S**

## Phase 5 — Polish and platforms

- [ ] **5.1.1** JSON export — **S** — schema given in the plan
- [ ] **5.1.2** JSON import — **S** — validation rules given in the plan
- [ ] **5.1.3** GUI import/export buttons — **S** — `src/gui/AGENTS.md`
- [ ] **5.2.1** Extract theme constants — **S**, but it's a large mechanical diff across `gui_nuklear.c`; verifier should screenshot-diff before/after per the plan's own test note
- [ ] **5.2.2** Dark theme — **S** — `src/gui/AGENTS.md`, WCAG AA is a checkable constraint, not a judgment call
- [ ] **5.2.3** Settings theme dropdown — **S**
- [ ] **5.3.1** `tr()` helper — **S** — small self-contained module
- [ ] **5.3.2** Extract strings (one commit per subsystem) — **S** — repetitive, many small commits, good parallel-subagent candidate since each subsystem file is disjoint
- [ ] **5.3.3** Add one locale — **S**
- [ ] **5.4.1** Antivirus scanner config key — **S**
- [ ] **5.4.2** Run scanner after finalize — **S** — quarantine-move logic is mechanical once the config key exists
- [ ] **5.5.1** Platform port plan document — **L** — synthesizes the whole codebase's platform-specific surface into a design document
- [ ] **5.6.1** Phase 5 wrap-up — **S**

## Parallelization notes

These groups touch disjoint files and can run as separate subagent calls
instead of serially, within the constraint that DB/IPC/queue-manager tasks
inside one step must still land in their written order (schema → API → IPC
→ UI) because each depends on the previous one's types:

- Within Phase 2: 2.4.1/2.4.2 (tray) is fully independent of 2.5.x
  (clipboard/batch) and of 2.6.x (categories) until the UI wrap-up — three
  parallel lanes are possible once 2.1.4/2.2.x/2.3.x land.
- Within Phase 3: 3.2.x (context menu/filters/exclusions) is independent of
  3.3.1 (link refresh) and 3.4.1 (installer) — all three can run in
  parallel once 3.1.x is merged.
- 5.3.2's per-subsystem commits are independent of each other by
  construction — this is the best parallel-fan-out candidate in the whole
  remaining plan.
