# ns-3 track — parity plan: closing the gap with OMNeT++/INET

**Status: plan only. No code changed.** Companion to `NS3_MIGRATION_SURVEY.md`
and `NS3_MIGRATION_POC_PLAN.md`. Those two answered "should we migrate" and
"is the mechanism viable" — both closed as **yes**, with all six milestones
(Gates 0–2, M2–M5) green in real CI as of PR #3. This document answers a third
question: **given every known limitation the ns-3 track has accumulated along
the way, which are worth closing, in what order, and which aren't "bugs" at
all** — before relying on the ns-3 track for anything beyond "the migration is
viable."

## Why tiered, not a flat checklist

Not every item below is the same *kind* of problem. Treating them uniformly —
"fix all N before moving on" — either wastes effort on items that can't
meaningfully improve (S2's 1-step vs. 2-step framing is *informationally
identical* either way) or defers real accuracy problems behind items that
don't matter (the M3 servo anomaly should not wait behind wire-format purity).
Four kinds of item appear in the audit below, and they get different
treatment:

1. **Precision-only simplifications** — real, disclosed, but don't change the
   *shape* of any result (S1, S3, S6).
2. **A shape-level difference with a real root cause** — S5's background
   traffic can't be truly hop-by-hop-forwarded because mainline ns-3's CSMA is
   half-duplex, unlike INET's full-duplex Ethernet. This is not a bug in
   syncsim's code; it's an ns-3 architecture fact, confirmed empirically (a
   real forwarding attempt destroyed the M3 isolation finding it was supposed
   to preserve).
3. **A real, undamped anomaly** — M3's congested peak (46,281µs) is 24× larger
   than INET's (1,949µs). The isolation *shape* is exact; the *magnitude* of
   that one number is not yet trustworthy.
4. **Missing capabilities** — some cheap to add (pcap, a queue-length export),
   some genuinely large engineering investments comparable to what the
   OMNeT++ track accumulated over a much longer project lifetime (GUI
   tooling, a YAML topology DSL, IEEE-TLV wire format).

---

## Full audit

| ID | Item | Kind | Root cause | Changes result *shape*, or just precision? | Tier |
|---|---|---|---|---|---|
| S1 | Coarser timestamps (send-scheduled/receive-callback vs. INET's streaming-PHY SFD signals) | Precision-only | ns-3 has no native SFD hook; `PhyTxBegin`/`PhyRxBegin` traces exist but aren't used yet | Precision only — delay is real, positive, stable | 2 |
| S2 | 1-step Pdelay/Sync — ✅ **DONE (P2b)**: real 2-step (`Pdelay_Resp`+`Pdelay_Resp_Follow_Up`, `Sync`+`Follow_Up`) | Precision-only* | ~~Simplicity~~ implemented P2b | *Informationally identical where loss is negligible (Gate2/M2/M4 ≤1 ns); under M3's heavy loss the extra frame/cycle really does move the peak (510→429 µs) + servo count (170→90), a faithful 2-step loss property, isolation shape intact | 2 |
| S3 | `neighborRateRatio = 1` — ✅ **DONE (P2a)**: now derived per link, folded into peer-delay + residence math | Precision-only | ~~Not implemented~~ implemented P2a | Precision only — confirmed empirically (≤ 2 ns transient effect, steady state unchanged) | 2 |
| S4 | Per-port gPTP termination, no transparent bridging | *Not a gap* | Deliberate — real 802.1AS gPTP uses a non-forwarded reserved multicast | N/A — arguably more correct than a transparent bridge | — |
| ~~**S5**~~ | ~~Background congestion traffic injected at the bottleneck egress, not truly forwarded hop-by-hop~~ — ✅ **DONE (P3a real fix)**: data now L2-forwarded hop-by-hop `clientsX[0]`→`swX`→`swCore`→`coreClient` over full-duplex `SimpleNetDevice`/`SimpleChannel` | Shape-level, real root cause | ~~Mainline ns-3's `CsmaChannel` is half-duplex shared-medium~~ — closed by swapping transport to `SimpleNetDevice` (full-duplex, mainline, no core patch) + a static hand-coded L2 forwarding table | Shape — real forwarding now matches INET; isolation shape held exactly, M3 peak moved 429→551 µs, M4 coupling 0.695→0.000 µs (both reported honestly) | **3 — DONE** |
| S6 | Approximate clock-driven burst scheduling (recomputed per-burst, not true mid-flight re-anchoring) | Precision-only | ns-3 has no `scheduleForAbsoluteTime` primitive | Precision only — still genuinely clock-driven | 2 |
| — | **M3 servo lock-loss transient**: congested peak 24× larger than INET's — ✅ **FIXED (P1a): 46,281 µs → 510 µs** | **Real, undamped anomaly** | Phase 2's deadbeat-phase + naive-integral-frequency servo overreacting to a wild rate estimate after a sporadically missed Sync — **plus** (found during the fix) a peer-delay `d` corrupted to tens of ms by Pdelay contention on the saturated shared CSMA medium, the dominant driver | **Magnitude — the one number this undermined trust in** | **1** |
| — | `queueLength:vector` not exported — ✅ **DONE (P1b)** | Missing capability, cheap | Never wired up in Phase 4; now sampled per switch-egress queue + exported to `vectors.csv` | Was blocking Pages backlog/coupling plots for ns-3 (already-scaffolded code, missing data) — now render non-empty | **1** |
| — | No pcap capture/replay past Phase 0's smoke test — ✅ **DONE (P2c)**: `--pcapPrefix` on all three scenarios + `check_pcap_gptp.py` verifier | Missing capability, moderate | ~~Not built~~ built P2c (own-format, off by default) | Debuggability gap closed; IEEE-dissectable capture still Tier 3 | 2 |
| — | No YAML-topology-DSL equivalent (Phase B) | Missing capability, large | Not built | Every ns-3 topology is hand-coded C++, not data-driven | 3 |
| — | No GUI/interactive tooling (Qtenv/IDE equivalent) | Missing capability, large | Not built; ns-3's own tools (NetAnim/PyViz) are a different paradigm, not a clean port target | Developer-experience gap, not a fidelity gap | 3 |
| — | Real IEEE TLV wire format for gPTP messages | Missing capability, large | Pragmatic 19-byte custom header used throughout | Needed for genuinely Wireshark-dissectable captures — pcap alone (above) doesn't get you this | 3 |
| — | Single topology fully proven (Nominal/Minimal shape only) | Missing capability | No generator/DSL to vary topology cheaply | Same root cause as the YAML-DSL gap | 3 |
| — | Shorter sim-time (20–30s ns-3 runs vs. OMNeT++'s 60s) — ✅ **DONE (P2d)**: default now 60s | Minor | ~~Convenience~~ normalized P2d | Low severity; peaks unchanged, time-scaled counts ~2× | 2 (folded into P2c commit) |
| — | M6 (real `ptp4l` cross-check) | Shared gap | Never built on *either* track | Not ns-3 falling behind — a future capability for the whole project | 4 (out of scope here) |
| — | Priority shaping (Qbv/Qav) | Shared gap | Deferred on the OMNeT++ side too (M3's original scope) | Same — not ns-3-specific | 4 (out of scope here) |

---

## Tier 1 — Trust restoration *(do first; ~1–2 weeks)*

These don't chase new fidelity — they make the numbers the ns-3 track
**already produced** trustworthy at face value.

### P1a — Harden the servo — ✅ **DONE**

**Status (done):** M3's congested `coreClient` peak went **46,280.929 µs →
510.471 µs** (24× outlier → below INET's 1,949.64 µs, same order of magnitude).
The isolation shape is unchanged — all 16 other nodes still ratio exactly 1.0x
(base peak == congested peak). Regression: Gate 2, M2, M4 all still PASS,
deterministic (byte-identical stdout across two runs each). The fix was two
clean-room changes in the vendored `gptp.{h,cc}` (re-vendored byte-identical
across `gptp/`, `nominal/`, `congestion/`, `feedback/`):

1. **Hardened PI servo** (public linuxptp control-loop idea, no ptp4l source):
   damped proportional phase + a **bounded, low-pass integral** frequency term
   (clamped ±500 ppm, per-cycle step ≤50 ppm, normalized by the *nominal* Sync
   interval), with the frequency update **skipped on any missed-Sync-length
   cycle** (nominal interval inferred as the running-minimum inter-Sync gap).
   This replaced the deadbeat-phase + fresh unbounded `offset/elapsed` estimate.
   It cut the peak 46,281 → ~20,500 µs (~2.3×).
2. **Peer-delay outlier filter** (running-minimum estimator) — **the dominant
   fix**, discovered during P1a verification. With the servo loop stabilized, the
   residual ~20 ms peak traced to the measured link delay `d` inflating to
   15–22 ms (≈1000× its true ~2–3 µs) because the Pdelay handshake frames contend
   on the *saturated shared CSMA medium* (the S5 artifact reaching the peer-delay
   path). A corrupted `d` injects a false offset any servo faithfully chases. The
   true delay is a physical floor, so a running-minimum estimator (established by
   the ~20 clean pre-congestion Pdelay exchanges) rejects the inflated samples.
   This took the peak ~20,500 → 510 µs. **510 µs is the genuine congested-queue
   Sync-delay signal** (a Sync waiting behind a near-full 10-packet queue), the
   real M3 mechanism measured cleanly.

Side effect (disclosed, all gates still green): the damped phase makes clean
scenarios' startup transient ~1.29× larger (e.g. gptp-spike client1 25.00 →
32.20 µs; nominal coreClient 18.75 → 24.08 µs). The gated properties (convergence
to ~0, drift-proportional ordering) are unchanged/tighter. M4's finding flipped
label — from a ~77 ns non-finding to a genuine but sub-µs 0.695 µs coupling
localized entirely to `coreClient` (all 16 others exactly 0.000) — reported
honestly rather than forced back; the physical conclusion (aligned microbursts do
not meaningfully degrade sync) holds.

---

Root cause, precisely: the current servo (`gptp.cc`'s `ApplyServo`) nulls the
*entire* measured offset every Sync (deadbeat phase correction) and estimates
residual drift fresh each cycle as `offset / elapsed`. When a Sync is
sporadically missed or arrives very late under congestion, `elapsed` balloons,
the derived rate estimate goes wild, and the deadbeat correction applies that
wild estimate as if it were ground truth — producing a large excursion that
takes several subsequent cycles to re-damp. This is exactly the gap flagged
as a "nice-to-have, time-boxed" item earlier in this project's own design
discussion about porting `ptp4l`'s real PI/PI2 control-loop *algorithm*
(not source — same clean-room discipline every prior phase followed). That
nice-to-have is now load-bearing.

- Port the real linuxptp PI/PI2 servo control-loop *algorithm* (bounded gain,
  integral windup protection) into `Clock::AdjustRate`/`AdjustOffset` calls,
  replacing the naive deadbeat+fresh-estimate approach.
- Add outlier handling: when `elapsed` for a Sync cycle exceeds some multiple
  of the configured Sync interval (a missed-Sync signature), either skip that
  cycle's frequency correction or clamp its magnitude, rather than trusting a
  derived rate computed over an anomalously long window.
- **This touches `gptp.cc`, which is vendored byte-identical across
  `ns3/gptp/`, `ns3/nominal/`, `ns3/congestion/`, `ns3/feedback/`** — a change
  here is a change everywhere, and **every** existing gate must be re-run as a
  regression check, not just M3.

**Gate:** M3's congested peak lands in a defensible range relative to INET's
1,949µs (not required to bit-match — this project's own standard is gating on
mechanism, not magnitude — but a 24× outlier is not "a different mechanism,"
it's an under-damped implementation). The M3 isolation shape (16/17 nodes at
ratio 1.0x) must still hold exactly. Gates 0–2 and M2/M4/M5 must all still
pass unchanged (regression).

### P1b — Export `queueLength:vector` — ✅ **DONE**

**Status (done):** `nominal`/`congestion`/`feedback-topology.cc` now sample every
traced switch-egress queue's backlog (5 ms cadence, whole run) and write one
`queueLength:vector` row per queue into `vectors.csv` (module
`Nominal.<node>.eth<port>.macLayer.queue`, the bracket-free ns-3 form
`plot_results.py` already matches; 20 queues per scenario). Sampling is read-only
and scheduled only when `--resultDir` is set, so stdout and every gate stay
**byte-identical** when unset (verified; all three still deterministic). Verified
end-to-end: `plot_results.py` against fresh `congestion`/`feedback` output now
renders **non-empty** backlog + offset-vs-backlog-coupling PNGs (54–92 KB;
congestion backlog reaches its 10-packet cap, mean ≈ 8.5; feedback spikes to its
20-packet cap under the aligned microbursts). Bonus: `analyze.py`'s pre-existing
`[analyze] egress queue backlog (packets):` section — previously always empty for
ns-3 — now lights up too. `ns3/OBSERVABILITY.md`'s "known gap" note is updated to
closed.

---

Cheap and already scaffolded: `plot_results.py`'s `plot_backlog`/
`plot_coupling` already look for this signal and degrade gracefully to
"render nothing" in its absence — they just need the data. Add a periodic (or
Enqueue/Dequeue-triggered) sample of each traced port's queue depth in
`nominal-topology.cc`/`congestion-topology.cc`/`feedback-topology.cc`,
exported as `queueLength:vector` rows with the module-name convention
`analyze.py`/`plot_results.py` already expect
(`Nominal.<node>.eth<port>.macLayer.queue`).

**Gate:** re-run `plot_results.py` against fresh congestion/feedback output;
the backlog and offset-vs-backlog-coupling charts render real content instead
of nothing, on the same published Pages report.

---

## Tier 2 — Fidelity closing, bounded effort *(~2–3 weeks)*

Real improvements toward matching real 802.1AS behavior, each individually
cheap enough to time-box, none expected to change any *observed number* (S1,
S3, S6 are all already proven precision-only, not shape-level).

### P2a — `neighborRateRatio` computation (closes S3) — ✅ **DONE**

**Status (done):** `neighborRateRatio` is now derived per link in
`GptpEntity`'s peer-delay handling from two successive Pdelay exchanges
(`neighborRateRatio = (t3_now − t3_prev) / (t4_now − t4_prev)` = neighbor-elapsed
/ local-elapsed) and folded into **both** the peer-delay turnaround (`(t3−t2)`,
measured on the neighbor's clock, divided by the ratio to reach local time) and
the bridge residence-time correction (residence scaled by the slave port's
ratio). A `>1%`-off-unity guard rejects a `t4` inflated by the M3 shared-medium
artifact — the same outlier class P1a's running-minimum peer-delay filter
rejects. Re-vendored byte-identical across all four scenario dirs; `gptp-spike`
now prints the measured ratio (as a ppb residual).

**Result — matches S3's long-standing prediction, essentially unchanged:** the
term is real and live, but post-servo-lock the measured ratio converges to ~1
(residual **< 0.5 ppb**) because the servo steers each local clock's *rate* to
GM — so folding it in only bites during the brief pre-lock transient. Net
observed effect, honestly reported (not forced to zero):

- **Gate 2 (gptp-spike):** all PASS; one downsampled trajectory sample moved 1 ns
  (`client2 −54.652 → −54.653 µs`), peer delay `sw↔client2 6.617 → 6.618 µs`;
  peaks/finals/servos otherwise identical.
- **M2 (nominal):** all PASS; a few transient peaks shifted ≤ 1 ns (last digit,
  e.g. `coreClient 24.075 → 24.076 µs`); every final `0.000 µs`.
- **M3 (congestion):** all PASS, **isolation shape exact** (16/17 nodes ratio
  1.0x); `coreClient` congested peak `510.471 → 510.473 µs` (+2 ns, still 21.2x),
  drop rate / backlog unchanged.
- **M4 (feedback):** all PASS; `coreClient` steady-window delta `0.695 → 0.694 µs`
  (still the sole non-zero node, still > 0.5 µs tol → "COUPLING OBSERVED"
  unchanged); all 16 others exactly 0.000.

All four scenarios deterministic (run-to-run byte-identical, twice each). The
≤ 2 ns shifts are the residence-time rateRatio fold acting during the pre-lock
transient; steady state is untouched, confirming S3 was precision-only.

**Original plan (for reference):** Real 802.1AS derives the rate ratio from
consecutive Pdelay measurement intervals at both link ends and folds it into the
peer-delay/residence-time correction instead of assuming both ends tick at the
same rate. S3's own documentation already established the omitted term is
sub-picosecond at this project's drift magnitudes — protocol completeness, not an
expected change to any result. Confirmed empirically above.

### P2b — Real 2-step Pdelay/Sync framing (closes S2) — ✅ **DONE**

**Status (done):** `Pdelay_Resp`/`Sync` are split into their real two-message
form — `Pdelay_Resp` (t2 only) + `Pdelay_Resp_Follow_Up` (t3), and a bare `Sync`
marker + `Follow_Up` (preciseOriginTimestamp + correctionField). New
`GptpMsgType` entries (`PdelayRespFollowUp=3`, `FollowUp=4`); the requester holds
per-port pending state (t2, t4) between Resp and its Follow_Up, and the slave
holds pending state (syncRxLocal, seq) between the bare Sync and its Follow_Up.
Re-vendored byte-identical across all four scenario dirs; `gptp.h`'s S2 header
block updated to closed.

**Result — verified empirically, NOT just asserted (this is exactly what the
gate asked for):** informationally identical in the lossless / light-loss
scenarios, with one honest, well-understood exception under heavy loss:

- **Gate 2 (gptp-spike):** all PASS. Peaks ≤ 1 ns (`client2 57.177 → 57.178 µs`),
  finals `0.000`, servo counts identical (159). Trajectory-table *row selection*
  shifts (the offset trace fires at `Follow_Up` receipt, not `Sync` receipt) but
  the underlying convergence is bit-identical.
- **M2 (nominal):** all PASS. Only hop-3 leaf peaks shifted ≤ 1 ns (the extra
  frame/cycle); finals `0.000`, servo counts 239, gate unchanged.
- **M4 (feedback):** all PASS. `coreClient` delta `0.695 µs` (= the P1a value to
  3 sig figs), still the sole non-zero node, "COUPLING OBSERVED" unchanged.
- **M3 (congestion) — the honest exception:** all PASS and **isolation shape
  EXACT** (16/17 nodes ratio 1.0x), but the headline numbers *do* move:
  `coreClient` congested peak **510.473 → 429.207 µs** (21.2x → 17.8x) and servo
  count **170 → 90**. This is **not** informational identity — and it is **not a
  bug**. It is a real property of 2-step: each cycle now needs BOTH the bare Sync
  and its Follow_Up to survive the ~33%-drop bottleneck queue, so surviving
  corrections roughly halve, and because the worst-delayed Syncs are the ones
  most likely to lose their partner Follow_Up, the *surviving* peak is lower. Real
  `ptp4l`/hardware 2-step behaves exactly this way under loss; the 1-step form was
  simply more loss-robust by carrying everything in one frame. The M3 *finding*
  (only the shared-queue node degrades), the drop/backlog regime, and the gate are
  all unchanged. Reported as data, per the project standard, not forced back.

All four scenarios deterministic (run-to-run byte-identical, twice each); the
CSV-export/`analyze.py --strict` path still passes on the P2b build. So S2's
"informationally identical" claim holds wherever loss is negligible, and P2b
additionally surfaced *where* it does not — a result only visible by verifying
empirically, exactly as the plan required.

### P2c — pcap capture on the gPTP + data path — ✅ **DONE**

**Status (done):** `nominal`/`congestion`/`feedback-topology.cc` each take a new
`--pcapPrefix=<prefix>` arg that enables `CsmaHelper::EnablePcapAll` on every CSMA
device (Phase 0's proven mechanism), writing one `<prefix>-<node>-<dev>.pcap` per
device — gPTP frames plus, on congestion/feedback's bottleneck, the background
data (congested/bursts pass only). **Opt-in and off by default**: with no
`--pcapPrefix` no files are written and stdout / every gate is byte-for-byte
unchanged (verified — capture only attaches trace sinks; the printed numbers are
bit-identical with and without it). `ns3/scripts/check_pcap_gptp.py` (new,
dependency-free, no scapy) is the content verifier — it parses each classic-pcap
frame, tallies gPTP frames by `GptpMsgType` off the `0x88b6` ethertype, and
asserts real parseable gPTP traffic **with the 2-step message types present** (a
1-step capture could not contain `Pdelay_Resp_Follow_Up`/`Follow_Up`). Mirrors
`scripts/check_pcap_replay.py`'s role. **Scope, as promised:** round-trip-
verifiable in this project's own 19-byte `GptpHeader` format, **not**
Wireshark-dissectable 802.1AS (IEEE TLV = Tier 3).

**Verification result:** nominal's gm↔swCore capture reads all five message types
with matched pairs (Pdelay_Req 20 / Pdelay_Resp 20 / …Follow_Up 20; Sync 7 /
Follow_Up 7); the congestion bottleneck link reads 12 758 total frames, 118 gPTP,
and — a bonus, honest confirmation of P2b — shows **more `Pdelay_Resp`/`Sync` than
their paired `…Follow_Up`s** (31 vs 24, 13 vs 10): the partner frames the
congested queue dropped, exactly the loss statistic that halves `coreClient`'s
servo cycles. Both captures PASS the verifier (exit 0).

### P2d — Normalize sim-time to 60s — ✅ **DONE**

**Status (done):** the default `simTime` in `nominal`/`congestion`/`feedback-`
`topology.cc` is now **60 s** (was 30 s), matching OMNeT++'s `sim-time-limit =
60s`, still overridable via the existing `--simTime` arg. Bundled into the P2c
commit per the plan. Verified: all four gates still PASS, all runs deterministic
(twice each), `analyze.py --strict --sim-time 60` PASS on all three. **Peaks are
unchanged** (they are early transients — coreClient nominal 24.076 µs, congested
429.207 µs, feedback delta 0.695 µs all identical to the 30 s run); only
time-scaled quantities grew ~2× (servo counts 239→479 baseline, congested 90→153;
congestion drop total 178 k→363 k at the same 32.7 %; feedback burst cycles
291→591, mean alignment spread even tighter at 0.579 µs). READMEs/OBSERVABILITY
updated to the 60 s figures.

---

## Tier 3 — Exploratory, each its own go/no-go *(none committed by this plan)*

These are not scoped or estimated the way Tier 1/2 are, because their actual
cost is genuinely unknown until a time-boxed spike answers the open question
each one carries. None of these should be treated as "planned work" — each
needs its own explicit decision after its spike, the same discipline that
governed R-CLOCK/R-GPTP in the original POC plan.

### P3a — Hop-by-hop L2 forwarding (closes S5) — ✅ **DONE (spike + real fix)**

**Spike (feasibility, DONE):** the open question was whether gPTP's custom
ethertype (`0x88b6`) can ride *some* full-duplex ns-3 device without patching
`CsmaChannel` core or writing a new `NetDevice` subclass. Answer: **YES** —
`ns3::SimpleNetDevice`/`SimpleChannel` (mainline ns-3.45) carry `0x88b6`
full-duplex, proven in `ns3/spikes/p3a-fullduplex-spike.cc` (reverse-gPTP latency
1.000× under forward saturation vs CSMA's 1488×). See
`ns3/spikes/P3A_SPIKE_FINDINGS.md`.

**Real fix (DONE, this phase):** built the actual S5 fix on the 18-node M3/M4
topologies. Swapped the link transport in `congestion-topology.cc` and
`feedback-topology.cc` from CSMA to `SimpleNetDeviceHelper` (exactly two devices per
channel = genuine full-duplex point-to-point), preserved the finite real-dropping
egress queue (`SimpleNetDevice::GetQueue()` + the same Enqueue/Dequeue/Drop traces),
and built **genuine hop-by-hop data forwarding** via a static hand-coded L2 table in
each switch's `CombinedRx` receive callback (client → zone switch → `swCore` →
`coreClient`) — **not** a `BridgeNetDevice`, per the spike's recommendation, so
gPTP's per-port termination (S4) stays untouched. `gptp.{h,cc}` needed **no** change
(transport-agnostic; verified byte-identical across all four vendored copies).

**Outcome (all gates re-run, honest numbers):**
- **M3 (congestion): GATE PASS.** Isolation shape **exact** (all 16 non-`coreClient`
  nodes ratio 1.0×, base peak == cong peak); `coreClient` congested peak **moved
  429.207 → 550.854 µs** (17.8× → 22.6×), drop `32.73% → 29.66%`, still well below
  INET's 1,949.64 µs. The peak moved up because the cleaner full-duplex mechanism +
  real forwarded arrival timing changed the queueing delay a `coreClient`-bound Sync
  sees — reported as data, not forced back.
- **M4 (feedback): GATE PASS.** The finding flipped to a **clean full non-finding**:
  `coreClient`'s steady-window delta **0.695 → 0.000 µs**, and all 17 nodes now
  exactly 0.000 — matching INET's bit-identical M4 result even more closely. Two-stage
  forwarding (zone-switch uplink cap-20 + bottleneck cap-20) changed drop `88.37% →
  47.62%` — the honest structural consequence of real forwarding.
- **Regression:** Gate 2 (`gptp-spike`), M2 (`nominal`) untouched and still PASS
  (they keep CSMA; only the two congestion/feedback files changed). All four
  scenarios deterministic (byte-identical stdout across two runs each);
  `analyze.py --strict` PASS on fresh congestion + feedback CSVs (and now visibly
  shows the per-hop forwarding: ~46.5 Mbps per zone uplink aggregating to ~139.5 Mbps
  at the bottleneck).
- **P2c pcap — briefly regressed, then restored (same session):** `SimpleNetDeviceHelper`
  has no `EnablePcap`/`EnablePcapAll` convenience wrapper (`SimpleNetDevice` exposes
  no `PromiscSniffer`/`Sniffer` trace source for a helper to auto-hook, confirmed
  against the pinned tree). But ns-3's own low-level pcap primitives —
  `PcapHelper::CreateFile()` + `PcapFileWrapper::Write()`, the exact calls
  `CsmaHelper::EnablePcapInternal` makes internally — are public API and work fine
  called by hand. `link()`'s new `PcapCapture` hook does exactly that, once per
  device, from inside each scenario's existing `CombinedRx` receive path (RX-only,
  content-equivalent to CSMA's old TX+RX capture on a point-to-point topology; a
  synthetic 14-byte Ethernet header is written since `SimpleNetDevice` carries none
  on the wire). No `gptp.{h,cc}` change needed. Verified: fresh captures on both
  scenarios are non-empty and `check_pcap_gptp.py` PASSes with all five message
  types present; the unset-prefix gate path stays byte-identical. `--pcapPrefix`
  works on all four scenarios again.

### P3b — YAML-topology equivalent for ns-3

A smarter implementation path than mirroring Phase B's code-generation
approach: rather than generating new `.cc` per topology, write one generic
"build an ns-3 topology from this YAML file" C++ function, read at program
startup — no codegen, no recompilation per topology. Real value (arbitrary
topologies without hand-coding each one), real effort (a general-purpose
topology interpreter, plus wiring `GptpEntity` role assignment generically).
Depends on nothing else in this plan; can be spiked independently.

### P3c — Real IEEE TLV wire format

Needed only if full Wireshark-dissectable capture (not just this project's
own round-trip format from P2c) is an actual goal. Substantial,
byte-exact-to-spec protocol implementation work. Pairs naturally with P2b
(2-step framing) if pursued — a 1-step, custom-header capture would not look
like a real gPTP capture regardless of TLV work done in isolation.

### P3d — GUI/interactive tooling

Lowest priority in this tier. ns-3's own visualization tools (NetAnim, PyViz)
are a different paradigm (packet animation) from Qtenv's step-through
message inspection — not a clean 1:1 port target. **Recommend deferring
indefinitely** unless a specific need surfaces; this looks like the least
justified use of effort in the entire audit.

---

## Explicitly out of scope for *this* plan (Tier 4)

**M6 (real `ptp4l` cross-check)** and **priority shaping (Qbv/Qav)** are not
ns-3 falling behind OMNeT++ — neither track has ever built them
(`congestion.ini`'s own scope explicitly deferred Qbv/Qav; M6 has been an
idea since the original README's decision record, never executed on either
simulator). Closing them here would be *new capability for the whole
project*, not ns-3 catching up to parity — track them against
`NS3_MIGRATION_SURVEY.md`'s own M6 mention if/when they're picked up, not as
part of this plan.

---

## Effort & risk summary

| Phase | Effort | Risk | Expected to change any *observed number*? |
|---|---|---|---|
| P1a servo hardening | 1 wk | Medium — touches vendored `gptp.cc` everywhere, full regression required | **Yes** — the point of this phase |
| P1b `queueLength:vector` | 2–3 days | Low | No — unblocks existing, already-scaffolded plots |
| P2a rate-ratio | 2–3 days | Low | No — already proven negligible |
| P2b 2-step framing | 1 wk | Low–medium (new state machine surface) | **Mostly no, but proven empirically NOT to be free**: identical to ≤1 ns in Gate2/M2/M4; under M3's heavy loss it genuinely shifts the peak (510→429 µs) + servo count (170→90) — a faithful 2-step loss property, isolation intact |
| P2c pcap capture | 3–5 days | Low — reuses Phase 0's proven mechanism | No — **DONE**, opt-in, gates byte-identical when off |
| P2d sim-time normalization | trivial | None | No — **DONE**, peaks unchanged (transient), counts ~2× |
| P3a S5 fix (spike + real fix) | ✅ **DONE** | Was unknown; spike said bounded, real fix confirmed it | **Yes** — M3 peak 429→551 µs, M4 coupling 0.695→0.000 µs, isolation shape exact; P2c pcap briefly regressed then restored (manual `PcapHelper`/`PcapFileWrapper` hook) |
| P3b YAML topology | 2–4 wk if pursued | Medium | No |
| P3c IEEE TLV wire format | 2–4 wk if pursued | Medium | No |
| P3d GUI tooling | Not recommended | — | — |

**Bottom line:** Tier 1 (≈1–2 weeks) is the only phase that changes a
*trusted* number — do it first, unconditionally. Tier 2 (≈2–3 weeks) is
real, bounded, worth doing, but every item in it is already known to be
precision-only — treat it as fidelity polish, not risk reduction. Tier 3 is
explicitly **not** committed by writing this document; each item gets its own
spike-first, gate-before-build treatment, mirroring exactly how R-CLOCK and
R-GPTP were handled in the original POC.

## Verification discipline (unchanged from every prior phase)

Same standard the whole ns-3 track has held itself to: build from a clean
copy of committed files, run twice to confirm determinism, run the *actual*
gate tooling (`analyze.py --strict`, the servo/isolation checks already built
into each scenario) and report real printed numbers — not a claim. Because
P1a touches vendored code shared across four scenario directories, its
regression pass must re-verify **all** of Gates 0–2 and M2–M5, not just the
phase that motivated the change. Every phase here should land in real CI
(now wired and proven green as of PR #3) before being considered done, not
just the local sandbox.

## Next step

This document is the plan only. Per this project's own established rhythm,
each tier — arguably each phase within Tier 1/2 — should be approved for
execution explicitly, the same way Phases 0–4 of the original POC plan were:
approving this document does not start P1a. Tier 3 additionally requires a
second decision *after* each item's own spike, before any of it is treated
as committed work.
</content>
