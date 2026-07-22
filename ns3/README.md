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
</content>
