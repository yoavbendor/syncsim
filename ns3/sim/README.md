# ns3/sim/ — the generic YAML-driven scenario engine (Tier 3 / P3b)

**One ns-3 program, many YAML configs.** This is the ns-3-native analog of how
the OMNeT++ side already works (one simulation binary + different `.ini` files):
a new topology or traffic pattern needs a **new YAML file, not new C++**. It is
the point at which the ns-3 track moves from "one hand-coded `.cc` per scenario"
to a data-driven engine — the stage where ns-3 stops being a POC and becomes the
project's primary way of defining scenarios.

`sim.cc` is a single generic interpreter that builds any of the four existing
scenarios (and anything else expressible in the schema) from a YAML config read
at startup. `gptp.{h,cc}` and `clock.{h,cc}` here are **byte-identical vendored
copies** of `ns3/gptp/`'s (md5-confirmed — the same discipline every prior phase
followed); `GptpEntity`/`Clock` are **not modified**. The engine is a new
*consumer* of those already-verified, tshark-validated libraries, not a rewrite.

This directory is **purely additive**: the four hand-coded scenario `.cc` files
and their directories are untouched. Whether to later retire them and rewire CI
to this engine is a separate future decision.

## Building / running

The engine links the external `yaml-cpp` library (MIT, mature, standard —
`libyaml-cpp-dev`). ns-3.45's `scratch/CMakeLists.txt` already supports linking
external libraries when a scratch subdirectory carries its own `CMakeLists.txt`
(it calls `build_exec()` with a `LIBRARIES_TO_LINK` argument); `ns3/sim/CMakeLists.txt`
does exactly that. The Docker `ns3` stage installs `libyaml-cpp-dev` and copies
this dir to `scratch/syncsim-sim`.

```bash
# local (mirrors ns3/README.md's build recipe, + libyaml-cpp-dev):
sudo apt-get install -y --no-install-recommends libyaml-cpp-dev
cp -r ns3/sim /path/to/ns-3-dev/scratch/syncsim-sim
cd /path/to/ns-3-dev && ./ns3 build      # builds scratch_syncsim-sim_sim

# run a scenario:
./build/scratch/syncsim-sim/ns3.45-sim --config=scratch/syncsim-sim/scenarios/congestion.yaml
# with CSV export for scripts/analyze.py and pcap capture:
./build/scratch/syncsim-sim/ns3.45-sim \
    --config=scratch/syncsim-sim/scenarios/congestion.yaml \
    --resultDir=/tmp/res-cong --pcapPrefix=/tmp/pcap-cong/c
```

CLI flags: `--config` (required), `--resultDir` (CSV export dir, default off),
`--pcapPrefix` (pcap capture prefix, default off — **the directory must exist**),
`--simTime` (override `sim.time_s`), `--seed`/`--run` (RNG, default 1/1).

## YAML schema

```yaml
sim:
  time_s: 60.0                # total simulated duration
  sync_interval_ms: 125.0
  pdelay_interval_ms: 50.0

nodes:
  - {name: gm,     role: gm,     drift_ppm: 0.0}    # role: gm | switch | client
  - {name: swCore, role: switch, drift_ppm: 50.0}   # gm is forced to 0 ppm
  - {name: coreClient, role: client, drift_ppm: 150.0}
  # hop depth is DERIVED (BFS from gm), never listed by hand.

links:
  - a: gm
    b: swCore
    master: a           # which endpoint holds the gPTP master port (a | b)
    queue_cap: 10       # 0 = default/unbounded; >0 = finite real-dropping cap
    data_rate: 100Mbps
    delay_us: 1.0

traffic:
  mode: none            # none | background_flows | aligned_bursts
  # background_flows:  mean_gap_us, payload_bytes, start_s, sources[], sink
  # aligned_bursts:    burst_interval_ms, fragments_per_burst, fragment_bytes,
  #                    start_s, sources[], sink

report:
  compare_baseline: true   # true => run TWICE (traffic off, then on) + isolation table
  isolation_tol_us: 5.0
  final_tol_us: 2.0        # single-run convergence gate tolerance
  degrade_factor: 5.0      # sink "degraded" if cong peak > this x baseline
  module_root: Nominal     # CSV module prefix (analyze.py HOP_MAPS key)
```

**Design decisions:**

- **Transport is full-duplex `SimpleNetDevice`/`SimpleChannel` for every link**
  (the S5-fix transport proven in `congestion`/`feedback`), for all scenarios —
  including the reproductions of `gptp-spike` and `nominal`, which were
  originally CSMA. The gPTP mechanism is transport-agnostic, so convergence /
  hop-composition / isolation all hold; the only visible consequence for the two
  formerly-CSMA scenarios is a small, disclosed peer-delay / transient-peak shift
  from the different serialization path (see the old-vs-new tables below).
- **`gm` sources Sync/Announce** on its master ports, has no slave port, never
  servos. A **`switch`** node gets one slave port (the link where it is *not* the
  master) + N master ports and relays Sync downstream. A **`client`** gets a
  single slave port. These roles fall out of the `role` + per-link `master`
  fields via `GptpEntity::AddPort(dev, mc, isMaster)` — nothing hop-specific.
- **Data forwarding is REAL hop-by-hop L2 forwarding** (the S5 mechanism): a
  traffic frame originates at each `sources` node and is forwarded switch-by-
  switch toward `sink` along the shortest path. The path is **DERIVED** from the
  link graph (BFS parent pointers rooted at the sink), generalizing
  congestion/feedback's hand-coded static L2 table to any tree. gPTP frames stay
  per-port-terminated (S4) the whole way — never bridged.
- **`queue_cap: 0` still tracks/samples** the switch-egress queue for CSV
  consistency (so `nominal`, with no finite cap, still exports its queue vectors);
  it only skips setting a finite `MaxSize`. `>0` installs a finite real-dropping
  cap on the switch-side device of that link.

## Reproducing the four scenarios

`scenarios/{gptp-spike,nominal,congestion,feedback}.yaml` make the generic engine
reproduce each existing scenario. The client drift rates (originally drawn from a
seeded `UniformRandomVariable`) are pinned **explicitly** in the YAML (the values
that driver's seed=1/run=1 draw prints), so each config is self-contained and
deterministic without depending on RNG draw order.

## Verification evidence (this sandbox, ns-3.45)

Every config was built from a clean copy of the committed files, run **twice**
(stdout byte-identical modulo the `--resultDir`/`--pcapPrefix` path lines →
deterministic), and compared against the **freshly re-run** hand-coded binaries.
The MECHANISM matches in every case; honest numeric differences are noted.

**Gate 2 — `gptp-spike` (single run, M1 signature):** GATE PASS.

| node | old (CSMA) peak µs | new (SimpleNetDevice) peak µs | final | servos |
|---|---|---|---|---|
| sw (80ppm) | 12.658 | 12.760 | 0.000 | 159 = 159 |
| client1 (200ppm) | 31.817 | 32.021 | 0.000 | 159 = 159 |
| client2 (-350ppm) | 57.561 | 57.358 | 0.000 | 159 = 159 |

Ordering (client2 > client1 > sw) and |drift|-proportionality hold; every final
0.000. Peer delay `7.260 → 5.320 µs` (the CSMA→SimpleNetDevice serialization
difference, disclosed).

**M2 — `nominal` (single run, hop-depth convergence):** GATE PASS. Every one of
the 18 nodes converges (final 0.000) across all three hop depths. Peak-by-hop
reproduces INET's non-monotonic finding (hops=3 mean < hops=2 mean):

| hops | old (CSMA) mean / max µs | new (SND) mean / max µs |
|---|---|---|
| 1 | 7.783 / 7.783 | 7.885 / 7.885 |
| 2 | 15.502 / 23.692 | 15.604 / 23.896 |
| 3 | 13.016 / 27.514 | 12.971 / 27.819 |

**M3 — `congestion` (compare-baseline, isolation shape):** GATE PASS.

| metric | old hand-coded | new engine |
|---|---|---|
| bottleneck drop | 29.66% | 29.59% |
| mean backlog | 8.88/10 | 8.88/10 |
| coreClient base → cong peak | 23.896 → 513.430 µs (21.5×) | 23.896 → 659.841 µs (27.6×) |
| isolation (other 16 nodes) | ratio exactly 1.0× | ratio exactly 1.0× |
| coreClient servos base/cong | 479 / 114 | 479 / — |

Isolation shape **exact** (all 16 non-sink nodes at their own baseline, ratio
1.0×); the sink alone degrades; drop rate and backlog essentially identical. The
congested **peak differs (513 → 660 µs)** — same order of magnitude, still far
below INET's ~1,950 µs, an honest difference from (a) the generic engine drawing
its exponential inter-packet gaps from a different RNG substream position (client
drifts are explicit here, not RNG-drawn, so the background-traffic RNG stream
starts from a different state) and (b) the generic forwarding scheduling. The M3
*mechanism* — congestion degrades sync **only** for the node sharing the
congested egress queue — is reproduced exactly.

**M4 — `feedback` (compare-baseline, coupling finding):** GATE PASS. Essentially
byte-identical to the hand-coded run (bursts are clock-driven, so no RNG stream
divergence): mean fire-time spread **0.578 µs** (= old), bottleneck drop
**47.62%** (= old), **every node's steady-window delta 0.000** (= old). Reproduces
INET's M4 non-finding: aligned microbursts do not measurably degrade sync.

**`analyze.py --strict`, pcap:** see the "Verification discipline" section of
`NS3_PARITY_PLAN.md`'s P3b entry for the `analyze.py --strict` and
`check_pcap_gptp.py` transcripts against fresh engine output.

## Determinism

The only RNG use is `background_flows`' seeded exponential inter-packet gap
(`aligned_bursts` is clock-driven, `none` touches no RNG); drifts are explicit.
All four configs are byte-identical run-to-run (verified twice each).
