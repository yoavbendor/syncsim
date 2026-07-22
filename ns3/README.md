# ns3/ — syncsim's ns-3 migration POC track

Companion source tree to `NS3_MIGRATION_SURVEY.md` and
`NS3_MIGRATION_POC_PLAN.md`. **Parallel and non-destructive**: the
`simulations/`, `scripts/*.py`, and existing Dockerfile stages
(`headless`/`gui`/`ide`) are untouched by anything here. OMNeT++/INET remains
syncsim's authoritative simulator; this tree exists to prove or kill the two
make-or-break risks the POC plan identifies before any larger port is
considered.

## Layout

```
ns3/
  smoke/            Phase 0 (Gate 0): CSMA + BridgeNetDevice + DropTailQueue +
                     pcap data-plane smoke test. No clock/gPTP model.
  clock/             Phase 1 (Gate 1): the clean-room, permissively-licensed
                     steerable per-node drift clock spike. See clock/README.md.
  gptp/              Phase 2 (Gate 2): clean-room minimal 802.1AS servo spike =
                     M1 equivalent (GM + 1 bridge + 2 clients). See gptp/README.md.
  nominal/           Phase 3 (M2 / R-BRIDGE): multi-hop time-aware bridges with
                     residence-time correction (18 nodes, 3 hop depths). See
                     nominal/README.md.
  congestion/        Phase 3 (M3): finite real-dropping queues + background
                     congestion; sync degrades only for the node sharing the
                     congested egress queue. See congestion/README.md.
  feedback/          Phase 3 (M4): finite queues + per-local-clock-aligned
                     bursts from all 12 zone clients; honest coupling-or-not
                     finding. See feedback/README.md.
```

Each `.cc` file here is dropped into a pinned ns-3 checkout's `scratch/`
directory at Docker build time (see the `ns3` Dockerfile stage) and built as
an `ns3 scratch` executable — the target name ns-3's build system assigns is
the `.cc` file's stem (e.g. `smoke/smoke-topology.cc` builds as
`smoke-topology`, regardless of which subdirectory it lives in), not a
directory-derived name.

## Pinned version

**ns-3.45** (GPLv2). Picked as a recent, stable point release — one behind
the bleeding edge at the time this track was started — over the latest
release, to reduce the odds of hitting not-yet-shaken-out regressions during
a spike that is already stressing corner cases (see `smoke/smoke-topology.cc`'s
header comment for two such corner cases hit and worked around during Phase 0).

## Building/running locally (no Docker)

```bash
git clone --branch ns-3.45 --depth 1 https://gitlab.com/nsnam/ns-3-dev.git /tmp/ns-3-dev
cp -r ns3/smoke /tmp/ns-3-dev/scratch/syncsim-smoke
cd /tmp/ns-3-dev
./ns3 configure --build-profile=release \
    --enable-modules=core,network,csma,bridge,point-to-point,applications,internet,flow-monitor \
    --disable-examples --disable-tests --disable-python \
    --enable-asserts --enable-logs
./ns3 build smoke-topology
./build/scratch/syncsim-smoke/ns3.45-smoke-topology
```

Runtime asserts are deliberately kept **on** (`--enable-asserts`), unlike a
typical performance-tuned ns-3 build: this is a research sandbox where
catching a simulation-model bug matters more than raw throughput, matching
the project's existing quality bar for OMNeT++/INET.

## Status

- **Gate 0 (Phase 0, data-plane smoke test): PASSED in the sandbox.**
  `smoke/smoke-topology.cc` proves R3 (finite, really-dropping queues — 2
  converging floods force real drops on the switch's shared egress queue,
  not merely "packets arrived"), R4 (`BridgeNetDevice` switch forwarding),
  and R6 (headless, deterministic — bit-for-bit identical pcap output
  across two independent runs; pcap capture on every link). **Not yet
  confirmed in real CI** — this sandbox has no Docker daemon, so the
  Dockerfile `ns3` stage below is written but unbuilt; verifying it in a
  clean CI container is the next step, per the project's own
  "sandbox proves it can work, CI proves it reproduces" discipline
  (`MIGRATION_HANDOFF.md`).
- **Gate 1 (Phase 1, steerable clock spike): PASSED in the sandbox.**
  `clock/clock.{h,cc}` is a clean-room, Apache-2.0 reimplementation of INET's
  `ConstantDriftOscillator` — a per-node local clock
  (`local = localBase + (1 + ppm/1e6)*(Now − t0)`) proving R1, the single
  biggest ns-3 gap (ns-3 natively has only one global clock). `clock-spike.cc`
  shows two clocks at syncsim's M1 baseline rates (+200 / −350 ppm) diverging
  from each other and from `Simulator::Now()` at exactly the configured rate
  (measured slopes 200.000 / −350.000 ppm, recovered from the sampled
  trajectory), then a mid-run servo action — `AdjustOffset(+1400 µs)` (phase)
  + `AdjustRate(+350)` (frequency) — steers one clock's offset and drift to ~0
  while the untouched control clock keeps drifting, proving the clock is
  programmatically steerable (the precondition for the Phase-2 gPTP servo).
  Deterministic (byte-identical across two runs; touches no RNG). Exposes a
  `TracedCallback` sample trace source for observability. **Not yet confirmed
  in real CI** — same Docker-daemon caveat as Gate 0. Full numeric evidence and
  the honest licensing caveat (combined ns-3 binary is still GPLv2) in
  `clock/README.md`.
- **Gate 2 (Phase 2, gPTP servo spike): PASSED in the sandbox.** `gptp/gptp.{h,cc}`
  is a clean-room, Apache-2.0 minimal IEEE 802.1AS mechanism on the Phase-1
  `Clock`: per-link peer-delay measurement (1-step Pdelay_Req/Resp),
  Sync/correction-field propagation through `sw` acting as both slave (toward
  `gm`) and master (toward `client1`/`client2`) with real residence-time
  accounting, and a phase-step + integral-frequency servo. `gptp-spike.cc`
  reproduces M1's exact topology and drift rates (sw 80ppm, client1 200ppm,
  client2 -350ppm) and its signature: every node's offset-from-GM converges to
  0.000us and holds (159 servo corrections/node over a 20s run), with peak
  offset correctly ordered and roughly proportional to |drift| (sw 10.00,
  client1 25.00, client2 44.76us — within ~2% of INET's own M1 baseline
  peaks, though per the POC plan the gate is the *mechanism*, not matching
  those digits). Measured peer delays are real, positive, and stable
  (6.62us/link). Deterministic (byte-identical across two independent runs).
  **Passing Gates 1 and 2 together is the POC plan's actual "migration is
  viable" decision point** — both hold on this pinned ns-3.45. Four explicit,
  stated simplifications (coarser send/receive-callback timestamping vs
  INET's streaming-PHY signals, 1-step Pdelay/Sync variants, unity
  neighborRateRatio, per-port gPTP termination instead of transparent bridging
  since real gPTP uses a non-forwarded reserved multicast) — none hidden, all
  in `gptp/README.md`. Does not yet cover multi-hop residence bridges (M2),
  data-plane congestion coupling (M3/M4), or wire-format/pcap fidelity —
  deferred to Phase 3. **Not yet confirmed in real CI** — same Docker-daemon
  caveat as Gates 0/1. Full numeric evidence and the honest licensing caveat
  in `gptp/README.md`.
- **M2 / R-BRIDGE (Phase 3, multi-hop time-aware bridges): PASSED in the
  sandbox.** `nominal/nominal-topology.cc` reproduces syncsim's M2 ("Nominal")
  scenario: 18 nodes across **three hop depths** from the GM — `gm` → `swCore`
  (hop 1) → {`coreClient`, `swA`, `swB`, `swC`} (hop 2) → 12 zone clients
  (hop 3) — each an independent Phase-1 `Clock` at M2's drift rates (`swCore`
  50, `swA` 80, `swB` −60, `swC` 100, `coreClient` 150 ppm; the 12 zone clients
  draw `uniform(−200,200) ppm` from ns-3's seeded `UniformRandomVariable`). The
  headline finding: the **Phase-2 `gptp.{h,cc}` needed ZERO changes** to
  generalize from one bridge hop to a chain — the M2 subdir vendors it
  **byte-identical** (confirmed by `md5sum`). The additive correction field
  (`upstreamCorrection + linkDelay + residence`) composes hop-by-hop by
  construction: `swCore` and each zone switch play the exact dual slave+master
  role the single Phase-2 `sw` proved, and a bridge is simply "a node with >1
  port" — nothing counts hops. **Every one of the 18 nodes' offset-from-GM
  converges to 0.000 µs and holds** (239 servo corrections/node over a 30 s
  run, 0.125 s Sync interval), across all three depths — the gate. Peak by hop:
  hops=1 mean/max 6.25; hops=2 mean 12.44 max 18.75; hops=3 mean 10.56 max
  21.95 µs — reproducing INET's non-obvious finding that peak does **not** grow
  monotonically with hop count (hops=3 mean < hops=2 mean), because each node's
  own local drift between corrections dominates its error more than compounding
  upstream error does; per-node peak tracks `|drift| × interval`, not depth.
  Deterministic (byte-identical stdout across two runs; only RNG use is the 12
  seeded drift draws). Carries forward all four Phase-2 simplifications (S1–S4)
  unchanged; does not yet cover data-plane congestion coupling (M3/M4).
  **Not yet confirmed in real CI** — same Docker-daemon caveat. Full numeric
  evidence in `nominal/README.md`.
- **M3 (Phase 3, finite queues + background congestion): PASSED in the
  sandbox.** `congestion/congestion-topology.cc` reuses the exact 18-node
  Nominal topology and the **byte-identical** vendored Phase-2 gPTP + Phase-1
  clock, adds a finite real-dropping `DropTailQueue` (`packetCapacity = 10`) on
  every switch egress port, and offers three ~50 Mbps background data flows
  (distinct ethertype) onto the single `swCore↔coreClient` 100 Mbps link — a
  ~150-into-100 Mbps oversubscription. gPTP and data share the **same
  per-device CSMA egress queue**, so the finding falls straight out: at the
  bottleneck the queue runs full (mean backlog 8.82/10, max 10/10) and drops
  **32.64%** (reproducing INET's ~34% / 9.23 backlog regime), and the gPTP Sync
  frames toward `coreClient` are dropped/delayed from that same queue. The
  scenario runs **twice in one process** (baseline no-traffic vs congested) and
  prints per-node peak offset side by side: **`coreClient` alone degrades — by
  ~2468x (18.75 → 46,281 µs) — while all 16 other nodes hold their baseline
  peak byte-identically (ratio exactly 1.0x)**. `coreClient`'s servo corrections
  drop 239 → 170 (real Syncs starved from the queue). That is the M3 finding:
  **congestion degrades sync only for whoever shares the congested egress
  queue, not globally.** The congested peak (~46 ms) is far larger than INET's
  ~1.95 ms — an honest, documented consequence of the Phase-2 servo (deadbeat +
  integral, gptp.h's stated servo simplification) losing lock and ringing under
  *sporadic* Sync loss (a servo transient, not free-run drift: 46 ms at 150 ppm
  would need ~300 s); the gate is the *shape/isolation*, not the digits, and the
  isolation is perfect. One new simplification (**S5**): the background flows are
  injected at their convergence egress rather than L2-forwarded hop-by-hop,
  because ns-3's shared-medium `CsmaChannel` (no full-duplex mode; full-duplex
  P2P rejects the vendored gPTP ethertype) would otherwise couple gPTP to
  reverse-direction transit data on every link — an artifact absent from INET's
  full-duplex Ethernet, and the reason INET gets clean isolation. Carries S1–S4
  forward unchanged. Deterministic (byte-identical stdout across two runs).
  **Not yet confirmed in real CI** — same Docker-daemon caveat. Full numeric
  evidence in `congestion/README.md`.
- **M4 (Phase 3, clock-aligned burst traffic): PASSED in the sandbox —
  faithful NON-finding, matching INET.** `feedback/feedback-topology.cc` reuses
  M3's finite-queue mechanism at `packetCapacity = 20` and replaces M3's three
  independent senders with periodic "frame" bursts from **all 12 zone clients,
  each scheduled on its OWN gPTP-steered local clock** — so the bursts align in
  *simulated* time only if gPTP has synced the clients. **M4's gate is not
  "prove coupling exists"** (`feedback.ini`'s own header says so): INET found
  real congestion yet gPTP offsets bit-for-bit identical to the no-traffic
  baseline. The genuinely new primitive is **clock-driven scheduling (S6)**:
  ns-3's `Simulator::Schedule` takes a *global* delta with no
  `scheduleForAbsoluteTime` analog, so after each burst a client recomputes its
  next **absolute local-clock** send instant from its clock's *live* rate+offset
  (`globalDelta = (targetLocal − currentLocal)/currentRate`) and schedules that —
  re-anchoring every burst at gPTP's own 0.125 s update granularity (stated as an
  honest analog, not bit-parity). It works: the 12 clients' bursts land within a
  **mean 1.153 µs** of each other, emergent purely from sync quality. The
  microbursts genuinely congest (15 frags × 12 clients ≈ 180 frames/instant vs a
  20-slot queue → **88.4% drop**, queue hits 20/20). **The finding, reported
  honestly: NO measurable coupling** — every node's steady-window (`t ≥ 1 s`, so
  the identical pre-burst transient can't mask small effects) peak offset under
  bursts is within 0.5 µs of its baseline, reproducing INET's result (Sync lands
  in the quiet gaps between 100 ms bursts). One honest nuance reconciling M3 and
  M4: `coreClient` — the sole node sharing the congested queue — is the *only*
  node with any non-zero delta at all (77 ns; all 16 others exactly 0.000), so
  M3's localization mechanism is faintly present but negligible at M4's larger
  queue. Same mechanism, two operating points. Carries S1–S5 forward unchanged.
  Deterministic (byte-identical across two runs; RNG use is only the 12 seeded
  drift draws — bursts are clock-driven). **Not yet confirmed in real CI** —
  same Docker-daemon caveat. Full numeric evidence in `feedback/README.md`.
</content>
