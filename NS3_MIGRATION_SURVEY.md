# Replacing OMNeT++ with ns-3 — survey & execution plan

**Status: survey / decision document. No code changed. Nothing here has been
built or run yet.** This is the analysis that precedes a go/no-go decision, not
a record of a migration in progress.

Motivation raised: the **OMNeT++ academic license may be too restrictive** for
syncsim, and ns-3 (GPLv2, no commercial restriction) is a candidate replacement.
A prior discussion with Gemini reportedly concluded a migration is *possible*
"with some considerable reworks." This document surveys exactly what those
reworks are, grounds each claim in what ns-3 does and does not provide today
(mid-2026), and lays out a phased, gated execution plan in the same style as the
project's existing milestone/decision-gate culture.

> The referenced Gemini share link (`share.gemini.google/77WjLU7flsvE`) is behind
> an auth wall and could not be fetched, so this survey is built from the syncsim
> codebase plus independent research rather than reproducing that conversation.

---

## 0. First, is the license actually a blocker? (cheapest gate)

Before any migration, pin down whether the license *actually* restricts
syncsim's use — because a full port is a large, research-grade effort and it
would be a waste to pay it if the current license already permits what syncsim
does.

The relevant facts:

- **The OMNeT++ *simulation kernel*** is under the **Academic Public License
  (APL)**. It grants free use "for any noncommercial purpose, including teaching
  and research at universities … research at non-profit research institutions,
  and personal non-profit purposes." Commercial use requires the paid **OMNEST**
  license. ([omnetpp.org/intro/license.html](https://omnetpp.org/intro/license.html),
  [OMNeT++ Wikipedia](https://en.wikipedia.org/wiki/OMNeT%2B%2B))
- **INET is *not* the problem.** As of 4.4 INET is fully **LGPL**
  ([inet-framework/inet LICENSE.md](https://github.com/inet-framework/inet/blob/master/LICENSE.md)).
  The restriction is exclusively the OMNeT++ kernel that `opp_run` embeds and
  that INET links against.
- **ns-3 is GPLv2** — free for any purpose *including commercial*, with a
  copyleft obligation to open-source modifications
  ([nsnam/ns-3-dev on GitLab](https://gitlab.com/nsnam/ns-3-dev/)). syncsim is
  already a public repo, so copyleft costs nothing here.

**Honest reading:** syncsim today is a public research/personal sandbox
(CI + GitHub Pages, no product, no sale). That use is *squarely inside* what the
APL already permits for free. The license only becomes a genuine blocker if
syncsim needs a **commercial / for-profit / closed** future, or if the mere
*existence* of the academic-only restriction (and its ambiguity for anything
that isn't clearly "non-profit") is judged unacceptable for the project's goals.

> **Gate 0 — decide this first.** If the answer is "academic/personal use,
> indefinitely" → the APL already covers it and this migration is a large cost
> for no present benefit; record that in README's decision section and stop. If
> the answer is "commercial or unrestricted use is a real requirement (now or
> plausibly later)" or "we want to be free of the restriction on principle" →
> proceed to the survey below. **This is the only decision that can make the
> whole effort unnecessary, so make it before spending anything.**

---

## 1. What syncsim actually requires from a simulator

Derived from the README and the M1–M5 results. The migration target must provide
*all* of these, because the project's entire thesis is the emergent
**sync ↔ congestion feedback loop**, and that loop only arises if every piece
below is mechanistic (not scripted):

| # | Requirement | Why it's load-bearing |
|---|---|---|
| R1 | **Independent per-node clocks with configurable drift (ppm)** | The premise. Sensors/MCUs each tick on their *own* drifting oscillator; good sync is what makes their bursts collide. |
| R2 | **IEEE 802.1AS gPTP** — grandmaster, peer-delay, Sync/Follow_Up, **time-aware bridges** (residence time), multi-hop boundary-clock propagation | The synchronization mechanism whose degradation under congestion *is* the signal. M2 specifically exercises hop-by-hop propagation through 17 nodes. |
| R3 | **Finite, really-dropping queues** (bounded capacity, DropTail) | M3/M5 depend on queues that overflow and drop, not idealized infinite buffers. |
| R4 | **Ethernet switches / bridges** with per-port egress queues | The congestion happens at a specific shared egress port. |
| R5 | **Traffic sources, including bursts aligned to a node's *own* clock** (`scheduleForAbsoluteTime` on the local clock) | M4's whole point: synced clients burst *simultaneously because they're synced*. Requires R1 to exist first. |
| R6 | **Deterministic, headless, CI-observable**; per-signal vector/scalar recording; **pcap capture/replay** | The project is "entirely observable from CI"; every result is reproducible and gated by `analyze.py --strict`. |
| R7 | *(deferred/stretch)* 802.1Qbv/Qav **priority shaping** | Already deferred in the OMNeT++ version (M3 stretch goal). A migration shouldn't regress the *option*. |

---

## 2. ns-3 capability gap analysis

The heart of the survey. Each requirement mapped to (a) what INET gives today,
(b) ns-3 **mainline**, (c) ns-3 **plus community modules**.

| Req | INET today | ns-3 mainline | ns-3 + community forks | Verdict |
|---|---|---|---|---|
| **R1 per-node drifting clocks** | ✅ native, maintained (`ClockBase` + `ConstantDriftOscillator`) | ❌ **absent** — every node reads one global `Simulator::Now()`; ns-3 has *no* node-local time and does not model drift | ⚠️ research forks only: *Clock Skew Models for ns-3* (2025 ns-3 conf), Lagwankar's `bounded-clock-skew-with-probability` branch, LCA2-EPFL's "local time" module | **Hard gap** |
| **R2 gPTP 802.1AS + time-aware bridges** | ✅ native, maintained (`Gptp`, `GptpBridge`, multi-domain) | ❌ **absent** | ⚠️ no maintained mainline gPTP. There are IEEE **1588** PTP research sims on ns-3 (e.g. "NS-3 based IEEE 1588 synchronization simulator for multi-hop network") but not 802.1AS L2 with residence-time bridges → largely a **clean-room reimplementation** | **Hard gap** |
| **R3 finite dropping queues** | ✅ native | ✅ **native** (`DropTailQueue`, `QueueDisc`, `TrafficControl`) | ✅ | **Strong** |
| **R4 Ethernet switches/bridges** | ✅ native | ✅ native (`CsmaNetDevice` + `BridgeNetDevice`, newer `EthernetNetDevice`) | ✅ | **Strong** |
| **R5 clock-aligned burst sources** | ✅ native (`ActivePacketSource.scheduleForAbsoluteTime`) | ⚠️ apps exist (`OnOff`, `BulkSend`) but "aligned to the node's own clock" is meaningless until R1 exists | depends entirely on R1 | **Blocked on R1** |
| **R6 headless/deterministic/pcap** | ✅ native | ✅ **native** — no GUI needed, seeded RNG, pcap built into every `NetDevice` | ✅ | **Strong** |
| **R7 802.1Qbv TAS (stretch)** | ✅ native | ❌ not mainline | ⚠️ `DenKrysos/Time-Aware-Shaper-TAS-in-ns-3` (built on ns-3.31, ported to 3.44); other academic forks | **Fork-only** |

**The shape of the gap is the whole story:** ns-3 is *excellent* exactly where
syncsim is easy (data plane: queues, switches, pcap, determinism — R3/R4/R6) and
*absent* exactly where syncsim is hard and where its research question lives
(R1 per-node clocks, R2 gPTP). The two things that must work are the two things
ns-3 does not provide out of the box.

### 2.1 The core architectural obstacle (why R1/R2 are "considerable reworks")

ns-3's design assumes **one global clock**: events are scheduled in, and every
timestamp is read from, `Simulator::Now()`. Nodes do not own a clock. The ns-3
community has repeatedly acknowledged this: *"ns-3 does not provide support for
node-local noisy clocks … clocks in ns-3 are synchronized with the top-level
Simulator and do not contain their own clock implementations,"* which is exactly
why *Clock Skew Models for ns-3* was written for the 2025 ns-3 conference
([ACM DL](https://dl.acm.org/doi/10.1145/3747204.3747208)).

To reproduce syncsim's phenomenon you must therefore:

1. Introduce a **per-node local-time abstraction** — a `Clock` mapping global
   time → local time with a rate/offset and configurable drift (ppm).
2. **Route all time-sensitive behavior through it** — timestamping of gPTP
   messages, scheduling of clock-aligned bursts (R5), any node-local timer.
3. Implement a **gPTP servo** that reads peer-delay/Sync measurements and adjusts
   each node's local clock — the closed loop that makes drift converge.
4. Model **time-aware bridge residence time** so multi-hop propagation (M2)
   behaves like 802.1AS, not like an idealized relay.

Every community artifact that does *part* of this is a **research fork on a
different ns-3 version, none merged to mainline** (thesis code, conference
papers). Integrating three such forks — a clock model, a gPTP/servo, and a TAS
shaper — each pinned to a different ns-3 release, then reconciling them, is the
"considerable rework." In INET all four points above are first-class, actively
maintained, and **syncsim already runs green on them (M1–M5 all pass)**.

### 2.2 The fidelity risk (research, not just engineering)

INET's `Gptp` is already a clean-room reimplementation whose servo/transients
don't match ptp4l (README's accepted cost, which M6 is meant to quantify). A
*second* clean-room reimplementation on ns-3 with a *different* clock model and a
*different* servo means the M1–M5 numbers (convergence transients, peak offsets,
the ~100× congestion coupling) **may not reproduce**. That's not automatically a
failure — under this project's "honest finding" standard a divergence is a
result to study — but it is a real risk that the ns-3 track becomes a *different
experiment* rather than a re-hosting of the same one.

---

## 3. Options ("flavors")

| Option | What it is | License outcome | Effort / risk | Fidelity |
|---|---|---|---|---|
| **0 — Stay on OMNeT++/INET** | Confirm APL permits syncsim's use (§0); do nothing | APL (academic/personal OK); INET LGPL | none | highest (proven green) |
| **A — Full port to ns-3** | Rebuild the entire model (clock + gPTP + TAS + topology + scenarios + analysis + CI) on GPLv2 | fully GPLv2, commercial-OK | **highest** — integrate 3+ unmaintained forks + reimplement gPTP; realistically weeks–months of research-grade work | at risk (§2.2) |
| **B — ns-3 as a parallel GPL cross-check** *(recommended if §0 says "proceed")* | Keep INET authoritative; build a **minimal** ns-3 track that reproduces M1 (± M3) as an independent GPLv2 implementation | a GPL-licensed artifact exists; INET stays the source of truth | **moderate, bounded** — same spirit as the already-planned M6 clknetsim cross-check | validates fidelity by construction |
| **C — Non-ns-3 GPL alternatives** | e.g. `clknetsim` (GPL, real ptp4l — already planned for M6) or netns+tc | GPL | low–moderate | **can't show the feedback loop** — clknetsim has no queue model, netns shares the host clock (see README's fidelity-triangle table) |

Option C is documented for completeness but is a poor fit: the README's own
fidelity-triangle table already shows clknetsim (no queue model, open loop) and
netns+tc (shared host clock, non-deterministic) each *lose the very phenomenon*
syncsim exists to study. Only INET and, in principle, a fully-built-out ns-3 can
close the loop deterministically.

---

## 4. Recommendation

1. **Resolve Gate 0 first.** If syncsim stays academic/personal, the APL already
   permits it — record that and stop. Migrating is otherwise paying a large,
   risky cost to solve a non-problem.
2. **If the license is a real requirement, do Option B before Option A.** A
   scoped ns-3 cross-check that reproduces M1 (a) yields a GPLv2 artifact
   immediately, (b) *doubles as the M6 validation the project already wants*, and
   (c) proves whether the per-node-clock + gPTP fidelity actually holds on ns-3
   **before** betting the whole project on a full rewrite. Promote B → A only
   after B demonstrates the numbers reproduce.
3. **Never do a big-bang cutover.** The two hardest, least-supported pieces (R1,
   R2) are exactly where a migration is most likely to stall; the project's
   existing decision-gate discipline ("if it stalls with disproportionate
   effort, document why and fall back") should govern every phase below.

---

## 5. Execution plan (phased, gated)

Mirrors the project's M-milestone + explicit-decision-gate style. Each phase is
**non-destructive** (OMNeT++/INET stays authoritative and green throughout) and
ends in a gate that can send the effort back to Option 0.

### Phase 0 — License decision *(hours)*
- Determine syncsim's actual/likely use case; check it against the APL text.
- Write the conclusion into README's "Why OMNeT++/INET" decision record.
- **Gate:** blocked or unrestricted-use required → proceed. Otherwise → stop.

### Phase 1 — ns-3 environment spike *(1–2 days)*
- Add a **new, separate** Docker target for ns-3 (pin a version, e.g. ns-3.44);
  do **not** touch the existing headless/gui/ide targets or the INET build.
- Smoke test: CSMA link + `BridgeNetDevice` + `DropTailQueue` + pcap, headless,
  green in CI. Proves R3/R4/R6 on ns-3 with near-zero risk.
- **Gate:** trivial data-plane run reproducible in CI.

### Phase 2 — Per-node clock spike *(the make-or-break, 1–3 weeks)*
- Evaluate and pick/port a node-local clock: *Clock Skew Models for ns-3* (2025)
  code, Lagwankar's `bounded-clock-skew-with-probability` branch, or LCA2-EPFL's
  local-time module. Prefer whichever is closest to the current ns-3 release.
- Prove: two nodes configured at e.g. +200 / −350 ppm **drift apart** at the
  configured rate, and node-local time is readable and distinct from global time.
- **Gate (hard):** if no clock model can be made to work cleanly on the pinned
  ns-3, the migration **stalls here** — document why and fall back to Option 0.
  R1 is a precondition for everything after it.

### Phase 3 — gPTP spike = M1 equivalent *(2–4 weeks)*
- Implement/port minimal 802.1AS: GM + 1 bridge + 2 clients, peer-delay + Sync,
  a servo that steers the Phase-2 clock.
- Reproduce M1's signature: independent drift → ~0 final offset, peak scaling
  with drift magnitude. Compare against INET's M1 table as a fidelity check
  (gate on *mechanism*, per the project's "correctness not magnitude" principle;
  document any transient divergence as a finding).
- **Gate:** M1 reproduces on ns-3, or the divergence is understood and acceptable.

### Phase 4 — Data plane + feedback = M2/M3/M4 *(3–6 weeks)*
- Multi-hop **time-aware bridges** with residence time (M2, the structurally
  riskiest gPTP piece); finite queues are already native (M3); background +
  clock-aligned burst traffic (M4, depends on R1/R5).
- Reproduce the core finding: congestion degrades sync **specifically for whoever
  shares the congested egress queue**, not globally.
- **Gate:** the sync↔congestion coupling arises mechanistically, as in INET.

### Phase 5 — Observability = M5 *(1–2 weeks)*
- Port `analyze.py`/`plot_results.py`/sweep tooling to ns-3 output
  (trace sinks / `FlowMonitor` in place of `opp_scavetool` `.vec`/`.sca`);
  time-windowed reporting; parameter sweep; GitHub Pages report.
- **Gate:** `--strict`-equivalent sanity gate green in CI on the ns-3 track.

### Phase 6 — Decision: dual-track vs. cut over *(days)*
- If fidelity held and the license demands it: retire INET, make ns-3
  authoritative, update the README decision record.
- Otherwise: keep **both** — INET authoritative, ns-3 as the standing GPL
  cross-check (this also closes out M6). This is the lower-risk, higher-value
  end state for a research sandbox.

---

## 6. What does *not* carry over (scope honesty)

A migration is a **model-layer rewrite**, not a config port — the opposite of the
OMNeT++ 6.0→6.4 bump, which was five ini-line fixes and **zero NED changes**.
Concretely thrown away and rebuilt:

- **All NED files** (`minimal.ned`, `nominal.ned`) → C++ ns-3 topology builders.
- **All `.ini` files** (`minimal/nominal/congestion/feedback/sweep`) →
  ns-3 `CommandLine`/config attributes.
- **`opp_run` / `opp_scavetool`** tooling → ns-3 run wrappers + trace parsing.
- **INET-specific analysis** (`simdata.py`'s `clock.timeChanged` / gPTP signal
  parsing) → ns-3 trace-sink parsing.
- **The pcap capture/replay recipe** → ns-3's own pcap (format-compatible, but
  re-plumbed).

Plausibly reusable, with rework:

- The **YAML topology model** (`configs/topology/*.yaml`) as the source of truth —
  `gen_topology.py` would emit ns-3 builder code instead of NED/ini.
- The **Python analysis skeleton** and the Mermaid/Pages report pipeline
  (`gen_mermaid.py`, `build_site.py`) — input format changes, structure survives.
- The **conceptual scenario definitions** (drift rates, hop layout, queue depths,
  sweep points) — the physics is portable even though every file expressing it
  is not.

---

## 7. Effort & risk summary

| Phase | Rough effort | Risk |
|---|---|---|
| 0 License decision | hours | none — may end the project cheaply |
| 1 ns-3 env + data plane | 1–2 days | low (native ns-3 strengths) |
| **2 Per-node clock** | **1–3 weeks** | **high — make-or-break; unmaintained forks** |
| **3 gPTP = M1** | **2–4 weeks** | **high — clean-room reimplementation + fidelity risk** |
| 4 M2/M3/M4 | 3–6 weeks | medium–high (time-aware bridges are the riskiest piece) |
| 5 M5 observability | 1–2 weeks | low–medium |
| 6 Decision | days | low |

**Bottom line:** the migration is *feasible* but is a research-grade
reimplementation of the two hardest components (per-node clocks + gPTP) on top of
unmaintained community forks — categorically different from the ini-only version
migration this repo just completed. The de-risking move is Option B (a scoped
ns-3 M1 cross-check that doubles as M6 validation), gated hard at Phase 2, with a
standing fallback to the already-green OMNeT++/INET stack — **and only after
Gate 0 confirms the license is a real blocker at all.**

---

## Sources

- OMNeT++ license (Academic Public License / OMNEST) — [omnetpp.org/intro/license.html](https://omnetpp.org/intro/license.html), [OMNeT++ — Wikipedia](https://en.wikipedia.org/wiki/OMNeT%2B%2B)
- INET is LGPL — [inet-framework/inet LICENSE.md](https://github.com/inet-framework/inet/blob/master/LICENSE.md)
- ns-3 GPLv2 / primary repo — [nsnam/ns-3-dev (GitLab)](https://gitlab.com/nsnam/ns-3-dev/)
- ns-3 lacks node-local clocks; *Clock Skew Models for ns-3* (2025 ns-3 conf) — [ACM DL 10.1145/3747204.3747208](https://dl.acm.org/doi/10.1145/3747204.3747208)
- Community clock-skew branch — [Lagwankar `bounded-clock-skew-with-probability` (GitLab)](https://gitlab.com/shaanzie/ns-3-dev/-/blob/bounded-clock-skew-with-probability/src/network/model/node.cc)
- ns-3 IEEE 1588 synchronization sim — [ResearchGate 308728520](https://www.researchgate.net/publication/308728520_NS-3_based_IEEE_1588_synchronization_simulator_for_multi-hop_network)
- ns-3 Time-Aware Shaper (802.1Qbv) implementation — [DenKrysos/Time-Aware-Shaper-TAS-in-ns-3](https://github.com/DenKrysos/Time-Aware-Shaper-TAS-in-ns-3), [TSN Simulation: TAS in ns-3 (ResearchGate)](https://www.researchgate.net/publication/348431501_TSN_Simulation_Time-Aware_Shaper_implemented_in_ns-3)
- ns-3 TSN ATS with imperfect/local clocks — [LCA2-EPFL/TSN-ATS-Clocks](https://github.com/LCA2-EPFL/TSN-ATS-Clocks)
- INET gPTP reference (802.1AS model syncsim uses today) — [Using gPTP — INET docs](https://inet.omnetpp.org/docs/showcases/tsn/timesynchronization/gptp/doc/index.html)
</content>
</invoke>
