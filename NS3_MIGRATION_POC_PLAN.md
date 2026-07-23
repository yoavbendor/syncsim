# syncsim → ns-3 migration — risk-first POC work plan

**Status: execution plan. No code changed, no ns-3 built.** Companion to
`NS3_MIGRATION_SURVEY.md` (the feasibility survey). The survey answers *whether*
to migrate; this doc answers *how*, and deliberately front-loads the two
make-or-break risks into a proof-of-concept so the effort fails fast and cheap
if ns-3 can't carry syncsim's premise.

## Context

syncsim today is **OMNeT++ 6.4 + INET 4.7**, green on M1–M5. The migration
driver is licensing: the OMNeT++ *kernel* is the Academic Public License
(non-commercial); ns-3 is GPLv2 (commercial-OK). The survey established the two
hard truths that shape any migration — and they are exactly the pieces INET
gives for free, so they are exactly where a port is most likely to fail:

1. **ns-3 has no native per-node clock/drift** — every node reads one global
   `Simulator::Now()`. syncsim's whole premise (independent drifting clocks)
   does not exist out of the box.
2. **ns-3 has no maintained gPTP/802.1AS** — it must be (largely) clean-room
   reimplemented.

So: **yes, start with a POC**, structured so those two risks are Phase 1 and
Phase 2, each behind a hard go/no-go gate that can send the whole effort back to
"stay on INET" having spent weeks, not months.

## Non-negotiable constraints (inherited from the project's culture)

- **Non-destructive.** OMNeT++/INET stays authoritative and green throughout; the
  ns-3 track is a *parallel* Docker target + directory tree. Nothing in
  `simulations/`, `scripts/*.py`, or the existing Dockerfile stages changes until
  a cutover is explicitly decided.
- **CI is the authority.** A milestone isn't "done" until green in real CI (clean
  container), not just the sandbox.
- **Gate on model-correctness, not magnitude.** Mirror `analyze.py --strict`: the
  ns-3 track must reproduce the *mechanism* (independent drift converging to ~0
  offset, peak scaling with drift), not identical microsecond values.
- **Honest findings.** A numeric divergence from the INET baseline is a result to
  document, not silently paper over.

---

## The two killer risks (POC targets)

| Risk | Why it's fatal | POC that proves or kills it |
|---|---|---|
| **R-CLOCK** — per-node steerable drifting clock in ns-3 | Without independent, *adjustable* local clocks there is nothing for gPTP to correct — the phenomenon cannot exist at all | **Phase 1**: two nodes at +200 / −350 ppm demonstrably drift apart in local time vs. global sim time, and a servo call steers one back |
| **R-GPTP** — 802.1AS servo + peer-delay reproduces M1 convergence | The sync half of the feedback loop; largely clean-room; routing timestamps through the node-local clock is the invasive part | **Phase 2**: GM + 1 bridge + 2 clients reproduce M1's signature (final ≈ 0, peak scales with drift) deterministically in CI |

Secondary risks (real, but *not* fail-fast — ns-3 is strong here): multi-hop
residence-time bridges (**R-BRIDGE**, M2) and fidelity divergence from INET
(**R-FIDELITY**). Both are handled after the two gates pass.

---

## Phase 0 — ns-3 environment, parallel & non-destructive *(~1–2 days)*

- New `ns3` Docker stage (or a second Dockerfile) pinning a specific ns-3 release
  (e.g. ns-3.44), GPLv2, **without touching** the existing `headless`/`gui`/`ide`
  stages. New source tree `ns3/` for the C++ model + scenario drivers.
- Smoke test proving ns-3's easy half: CSMA + `BridgeNetDevice` + `DropTailQueue`
  + pcap, headless, deterministic (fixed `RngSeed`/`RngRun`).
- **Gate 0:** trivial data-plane run reproducible in a clean CI container.

## Phase 1 — R-CLOCK spike: per-node steerable clock *(~1–3 weeks, MAKE-OR-BREAK)*

- **Clean-room, permissively-licensed** `Clock` object written from scratch. The
  three known community implementations — *Clock Skew Models for ns-3* (2025 ns-3
  conf), Lagwankar's `bounded-clock-skew` branch, LCA2-EPFL's local-time module —
  are read **as reference / API-shape guidance only, not copied**, so our
  contribution's copyright stays ours to license permissively (e.g. Apache-2.0 /
  MIT) with no fork-specific obligations or ns-3-version lock. Model:
  `local = offset + (1 + ppm/1e6)·(Now − t0)`, with `AdjustRate` / `AdjustOffset`
  for the servo. This is INET's `ConstantDriftOscillator` reimplemented.
- **Honest licensing caveat (stated, not glossed):** clean-room lets us put a
  permissive license on *our own files*, but a clock module that links against
  **ns-3 core is a derivative work under GPLv2** — so the *combined, distributed*
  work is still governed by GPLv2 regardless of the header we stamp on our
  sources. What clean-room actually buys: (a) we own the copyright and *may*
  dual-license / reuse our contribution elsewhere, and (b) we avoid inheriting any
  single fork's terms and version coupling. It does **not** make the ns-3 build
  itself permissive. If a truly permissive *combined* artifact is the real
  requirement, that is a finding that argues against ns-3 as the base at all —
  surface it at Gate 2, not silently.
- Prove: two nodes at +200 / −350 ppm diverge in local time at the configured
  rate; local time is readable, distinct from `Simulator::Now()`, and steerable.
- **Gate 1 (hard):** if no per-node steerable clock can be made to work cleanly on
  the pinned ns-3, **STOP** — document why, fall back to OMNeT++/INET. Every later
  phase depends on this.

## Phase 2 — R-GPTP spike = M1 equivalent *(~2–4 weeks, MAKE-OR-BREAK)*

- Minimal 802.1AS: GM + 1 bridge + 2 clients, peer-delay (Pdelay_Req/Resp),
  Sync/Follow_Up, a servo steering the Phase-1 clock. **Route all message
  timestamps through node-local time**, not global time (the invasive plumbing the
  survey flagged).
- Reproduce M1's signature (INET baseline: sw 80 ppm peak ≈ 10 µs; client1 200 ppm
  peak ≈ 25 µs; client2 −350 ppm peak ≈ 44 µs; all final ≈ 0). Gate on the
  *mechanism* per `analyze.py`'s philosophy, not identical values.
- **Gate 2:** M1 reproduces on ns-3 (or the divergence is understood and
  acceptable). Passing Gate 1 **and** Gate 2 is the real "migration is viable"
  decision point — the analogue of the survey's decision gate.

## Phase 3 — Data plane + feedback = M2/M3/M4 *(~3–6 weeks)*

- M2 multi-hop time-aware bridges with **residence-time** correction (R-BRIDGE,
  the riskiest gPTP piece); M3 finite queues (native, easy); M4 background +
  **clock-aligned** burst traffic (bursts scheduled on each node's *local* clock —
  depends on Phase 1).
- Reproduce the core finding: congestion degrades sync **only for whoever shares
  the congested egress queue**, not globally.

## Phase 4 — Observability = M5, port the analysis/report *(~1–2 weeks)*

- Replace the `opp_scavetool` CSV export (`scripts/simdata.py`'s
  `export_vectors_to_csv` / `export_scalars_to_csv`) with ns-3 trace-sink /
  `FlowMonitor` output. Re-target `analyze.py`'s offset derivation
  (`parse_offset_series`, keyed on `timeChanged:vector` + `.clock` modules) and the
  congestion summary (queue scalars) at the ns-3 result format.
- Time-windowed report, parameter sweep (`sweep.ini` → an ns-3 `CommandLine`
  attribute loop), and the Mermaid/Pages pipeline (`gen_mermaid.py` /
  `plot_results.py` / `build_site.py`) — input format changes, structure survives.
- **Gate 4:** a `--strict`-equivalent sanity gate green in CI on the ns-3 track.

## Phase 5 — Decision: dual-track vs. cut over *(~days)*

- If Gates 1–4 held and the license demands it: retire INET, make ns-3
  authoritative, update README's decision record.
- Otherwise (recommended lower-risk end state): keep **both** — INET authoritative,
  ns-3 as a standing GPL cross-check (this also closes out M6).

---

## What carries over vs. gets rebuilt (scope honesty)

**Rebuilt (model layer — a rewrite, unlike the ini-only 6.0→6.4 bump):**

- `simulations/*.ned` → C++ ns-3 topology builders.
- `simulations/*.ini` → ns-3 `CommandLine` / config attributes.
- `scripts/run.sh` (`opp_run`) + `simdata.py`'s `opp_scavetool` export → ns-3 run
  wrappers + trace parsing.
- The `Gptp` / clock model → the Phase 1/2 C++.

**Reusable with rework:**

- `configs/topology/*.yaml` as source of truth — `gen_topology.py` emits ns-3
  builder code instead of NED/ini.
- `analyze.py`'s reporting/sanity logic (`print_offset_report`,
  `run_sanity_checks`, hop-count maps, `--strict`) — only the *input parsing*
  changes; the offset-from-GM trick (GM at 0 ppm ⇒ offset = clock_time − sim_time)
  ports directly.
- `plot_results.py` / `gen_mermaid.py` / `build_site.py` + the CI/Pages shape.
- The conceptual scenarios (drift ppm, hop layout, queue depths, sweep points).

## Effort & risk summary

| Phase | Effort | Risk |
|---|---|---|
| 0 env + data plane | 1–2 days | low |
| **1 R-CLOCK** | **1–3 wk** | **HIGH — make-or-break** |
| **2 R-GPTP = M1** | **2–4 wk** | **HIGH — make-or-break** |
| 3 M2/M3/M4 | 3–6 wk | medium–high (residence-time bridges) |
| 4 M5 + observability | 1–2 wk | low–medium |
| 5 decision | days | low |

**Bottom line:** POC-first is the right call. Phases 1–2 concentrate ~90% of the
risk into the first few weeks behind two hard gates; if either can't be made to
work cleanly on the pinned ns-3, we stop having spent weeks, not months, and fall
back to the already-green INET stack.

## Verification (per phase)

- **Phase 0:** `docker build` the ns-3 stage in clean CI; the CSMA+queue+pcap smoke
  run produces a non-empty pcap and identical output across two seeded runs.
- **Phase 1:** a unit-style driver dumps local-vs-global time for two nodes and
  asserts the drift slope matches the configured ppm and that a servo call changes
  it.
- **Phase 2:** run the M1-equivalent; a ported / `--strict`-equivalent check
  asserts every node's final offset ≈ 0 and peak scales with drift; compare the
  three peaks against the INET M1 table and document any divergence.
- **Phases 3–4:** reproduce the M2/M3/M4/M5 findings; the ns-3 `--strict` gate is
  green in CI on every branch, *alongside* (not replacing) the INET jobs.

---

## Next step

This document is the plan only. **Building ns-3 and the Phase 1 clock spike is a
separate, explicitly-requested next step** (multi-week) — approving/merging this
doc does not start it. When that step is taken, Phase 0 is the entry point and
Gate 1 is the first place the effort can be cheaply abandoned.
</content>
