# syncsim — gPTP + TSN congestion-feedback sandbox

A reproducible, CI-observable simulation of a time-sensitive LAN. Sensors, MCUs and
loggers synchronize to a gPTP grandmaster, then transmit frame-aligned bursts **on
their own synced clocks** — so good sync makes bursts collide, imperfect shaping
congests switch queues, PTP frames get delayed/dropped, sync degrades, bursts spread,
congestion relaxes, and the system recovers. That **sync ↔ congestion feedback loop is
the phenomenon under study.**

Built on **OMNeT++ 6.0.3 + INET 4.5.4**, run headless, entirely observable from CI.

## Why OMNeT++/INET (decision record)

Three tools each own a different corner of the fidelity triangle, and you cannot cheaply
have all three — a real kernel packet stack runs in wall-clock time, while independent
controllable clocks need virtual time, and those are incompatible:

| Tool | Real ptp4l | Independent drift | Real switches/queues | Emergent feedback | Deterministic/CI |
|---|:--:|:--:|:--:|:--:|:--:|
| clknetsim | ✅✅ | ✅✅ | ❌ (no queue model) | ❌ (open loop) | ✅✅ |
| netns + tc | ✅ | ❌ (shared host clock) | ✅✅ | ❌ (senders share clock) | ❌ |
| **OMNeT++/INET** | ❌ (reimpl. gPTP) | ✅✅ | ✅✅ | ✅✅ | ✅✅ |

INET is the **only** option where the closed feedback loop arises on its own, with
mechanistic time-aware bridges that model the hop-by-hop peer-delay + residence-time
propagation of IEEE 802.1AS.

**Accepted cost:** INET's `Gptp` is a clean-room reimplementation of 802.1AS. Its servo,
timestamp filtering, and config surface are **not** ptp4l's, so convergence transients and
lock-time will not match a production ptp4l build. Milestone 6 adds a `clknetsim` cross-check
(real ptp4l on a matched profile) to quantify that gap. Spread-spectrum clocking (SSC) is
deliberately out of scope — at gPTP's sampling rate it aliases to noise and teaches nothing
about the system.

## Design principles

**CI gates on model correctness, never on result magnitude.** A harsh scenario producing
a large but bounded, recovering offset is a real *result* to study -- the whole point of
this sandbox is to produce numbers like that -- not a test failure. `analyze.py --strict`
fails only on evidence the simulator isn't faithfully modeling the real protocol: missing
expected signals, NaN/Inf in a series, or offset diverging past a generous sanity ceiling
(default 50ms -- "gPTP has clearly stopped functioning," not a sync-quality target). It
never fails because a peak offset was merely large.

**Recording policy: data traffic gets descriptors, sync gets everything.** Background
data-plane UDP traffic is recorded only as aggregate scalars (packet/byte counts, drop
counts) -- never per-packet vectors, which balloon with offered load (an early congestion
run produced a ~2GB artifact before this policy existed). `analyze.py` derives Mbps/pps/
drop-ppm from those scalars for a compact congestion summary. gPTP, clock, and queue-
backlog signals are the actual subject of study and are **always** fully vector-recorded,
regardless of data-plane load -- see the recording-policy comment in `congestion.ini`.

## Layout

```
Dockerfile                  # headless OMNeT++ + INET image
.github/workflows/ci.yml    # build image (cached) + run Minimal + archive results
simulations/
  minimal.ned / minimal.ini # M1: GM + 1 bridge + 2 clients, independent drifting clocks
  nominal.ned / nominal.ini # M2: multi-hop -- GM + core switch + 3 zone switches + 13 clients
  congestion.ini            # M3: Nominal topology + finite queues + background UDP traffic
scripts/
  run.sh                    # opp_run wrapper (headless Cmdenv)
  analyze.py                # export vectors/scalars -> offset report, sanity checks, congestion summary
```

## Run locally (any Linux with Docker)

```bash
docker build -t syncsim .
docker run --rm -v "$PWD:/work" syncsim bash scripts/run.sh General simulations/minimal.ini results
docker run --rm -v "$PWD:/work" syncsim python3 scripts/analyze.py results
```

## Milestones

- **M0** — toolchain + CI + scaffolding *(this commit)*
- **M1** — minimal synced network (independent drifting clocks actually converge over gPTP)
- **M2** — multi-hop bridges; measure sync-error growth vs hop count
- **M3** — TSN switch realism (finite real-dropping queues, background congestion; priority
  shaping (Qbv/Qav) deferred -- see Status)
- **M4** — emergent feedback loop (clock-driven burst sources)
- **M5** — observability, scenario sweeps (the levers), phase-aware assertions
- **M6** — optional `clknetsim`/ptp4l cross-check

## Status

**M1 is done and green in CI.** Independent drifting clocks (client1 at 200ppm, client2 at
-350ppm, the switch at 80ppm) genuinely converge over gPTP, measured via INET's own
`gptp.timeDifference` signal on each syncing node. From a real run:

```
client1.gptp   final=+0.00us  peak=25.01us   (200ppm drift)
client2.gptp   final=+0.00us  peak=43.76us   (-350ppm drift)
sw.gptp        final=+0.00us  peak=10.00us   (80ppm drift)
```

Peak error scales with configured drift magnitude and every node settles to ~0 offset --
the signature of real physics, not a placeholder. `analyze.py --strict` gates on model
correctness (signals present, no NaN/Inf, no unbounded divergence), not on this magnitude
being small -- see Design principles.

**M2 is done and green in CI.** Multi-hop topology (GM -> core switch -> {coreClient at 2
hops, 3 zone switches each with 4 clients at 3 hops}, 17 syncing nodes total, every node
including the switches independently drifting) confirms IEEE 802.1AS's hop-by-hop
peer-delay + residence-time propagation works through INET's transparent/boundary-clock
chain -- all 17 nodes converge to 0 final offset. From a real run:

```
hops=1  n=1   mean_peak= 7.36us  max_peak= 7.36us   (swCore)
hops=2  n=4   mean_peak=12.19us  max_peak=18.75us   (coreClient, swA/B/C)
hops=3  n=12  mean_peak= 8.40us  max_peak=17.90us   (zone clients)
```

**Honest finding, not the naive hypothesis:** peak error does *not* grow monotonically with
hop count here -- hops=3 is actually lower on average than hops=2. At this sync rate
(~16 corrections/sec) each hop's peer-delay/residence-time correction happens often enough
that per-node error is dominated by *that node's own local oscillator drift* between
corrections, not by compounding error inherited from upstream hops. The hop-by-hop
correction mechanism is doing its job -- it's actively preventing accumulation, which is
the point of the protocol. To isolate a pure hop-count effect (measurement/quantization
error accumulating independent of drift) would need a follow-up scenario holding drift
constant across all nodes and only varying hop depth.

**M3 is done and green in CI.** `congestion.ini` reuses the Nominal topology with every
switch port's egress queue replaced by INET's `DropTailQueue` (packetCapacity=10 -- the
confirmed real idiom; INET's default queue is unbounded and errors rather than drops if
given a capacity without a policy) plus background UDP traffic from one client per zone
converging on `coreClient`'s single 100Mbps uplink. A real run confirmed:

```
swCore.eth[1] (bottleneck)   148.77 Mbps   18,758.5 pps   drop=341,112 ppm (34.1%)
every other port             ~0 Mbps -- completely unaffected
backlog: swCore.eth[1] max=10 mean=9.23 (capacity) -- every other port max=1-2

coreClient.gptp   peak=1949.64us  n=446 samples   (vs ~18.75us / n=958 uncongested, M2)
swA/B/C, zone clients:  unaffected -- same range as M2 baseline
```

Congestion is isolated exactly to the shared link, and only `coreClient` (which shares the
overflowing queue with the data traffic) shows degraded sync -- ~100x peak offset and fewer
than half the normal sync updates. Every node on an unrelated path is untouched. This is the
core phenomenon the project set out to observe: **congestion doesn't degrade sync globally,
it degrades sync specifically for whoever shares the congested queue with the data traffic.**

**The recording policy took three attempts to get right** (documented honestly since it's a
real lesson, not just a footnote): mixing the `vector-recording` boolean with per-statistic
`result-recording-modes` on the same statistic first produced *zero* vectors (a real bug the
new sanity gate caught correctly -- "no .vec files" is exactly the kind of structural failure
it's meant to catch), then module-path wildcards like `eth[*].**.vector-recording` silently
did nothing, growing the artifact to 4.68GB. The fix, confirmed against OMNeT++'s actual
manual source rather than guessed: `result-recording-modes` is keyed **by statistic name**
regardless of module path, and a bare value *replaces* the mode set. `congestion.ini` now
sets `sum`/`count` for the length/count stats `analyze.py`'s Mbps/pps/drop-ppm needs, and `-`
for pure per-packet telemetry with no aggregate use -- gPTP/clock/queue-backlog signals are
never named, so they keep recording via the untouched default. Result: 2,180 vectors -> 143,
artifact size ~2GB -> 179MB (~11x smaller), with the underlying physics numbers unchanged
(bit-for-bit the same offset/drop findings as the original bloated run).

**Deferred from M3's original scope:** priority shaping (Qbv/Qav prioritizing gPTP over
data) so PTP survives congestion rather than sharing its fate. Shipping it requires
confirming how INET classifies locally-originated gPTP frames into egress traffic classes,
which isn't yet established -- a real stretch goal, not silently dropped.
