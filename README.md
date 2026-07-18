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
- **M4** — emergent feedback loop (clock-driven burst sources; see Status)
- **M5** — observability: time-windowed reporting + a real parameter sweep (see Status)
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

**M4 is done, with a genuinely surprising honest finding.** `feedback.ini` reuses the
Nominal topology with all 12 zone clients sending clock-aligned "frame" bursts (10fps,
timed via `source.clockModule = "^.^.clock"` + `scheduleForAbsoluteTime = true` --
confirmed directly from `inet/src/inet/queueing/source/ActivePacketSource.ned`, no custom
C++ needed, contrary to the original plan's assumption) converging on `coreClient`.

First attempt (1400B single-packet frames) validated the mechanism but showed **zero
effect**: offsets identical to the no-traffic baseline, zero drops -- a 12-packet aligned
burst can't overflow a 20-packet queue regardless of alignment. Fixed by recognizing the
relevant variable is burst *width* (simultaneous packet count), not sustained bandwidth:
20000B frames fragment into ~15 IP packets each, so 12 aligned clients present ~180
simultaneous packets against the queue. That produced real, substantial, sustained
congestion:

```
swA/B/C.eth[0] and swCore.eth[1]   drop rate 41.0-45.8%   queue mean 14-15.5/20 (near-full)
```

**But gPTP offsets are still bit-for-bit identical to the no-traffic baseline** -- every
peak value matches M2 exactly, including `coreClient` (18.75us, unchanged). This is not a
broken mechanism (the queue-backlog numbers clearly changed between runs, confirming the
simulation genuinely re-ran with different traffic); it's a real, non-obvious research
result: **M3's sustained flood (~18,750 continuous pps, queue permanently full) degraded
`coreClient`'s gPTP by ~100x, but M4's periodic bursts (100ms cycles, ~1,008 pps average at
the same bottleneck) show no measurable coupling at all**, despite a higher peak drop rate.
The likely explanation: gPTP messages fire roughly every 62ms and may simply land in the
quieter gaps between 100ms burst cycles often enough to avoid the worst queueing, even
though the burst instants themselves drop half the data traffic. Sustained congestion
couples to sync in this model; intermittent burst congestion that drains between cycles
apparently doesn't -- at least at this burst/period ratio. A natural follow-up (not yet
attempted) would be narrowing the gap between bursts and PTP's own cadence, or shrinking
the queue further, to find where -- or whether -- the coupling actually appears.

**M5 (in progress).** Two additions, both scoped to avoid gambling on unconfirmed
mechanisms:

1. **Time-windowed reporting** (`analyze.py --time-windows N`, default 4): peak |offset|
   per node broken into equal windows across the run, not just whole-run peak/final.
   Surfaces transient onset or oscillation within a constant-load run without needing any
   new scenario mechanics. (Phased scenarios via `ScenarioManager` runtime `set-param` on
   `productionInterval` were considered and set aside -- whether `ActivePacketSource`'s
   `volatile` parameter actually re-reads after a runtime change wasn't confirmed, and
   this gives most of the same visibility on data already collected.)
2. **A real parameter sweep** (`sweep.ini` + `scripts/run_sweep.sh` +
   `scripts/summarize_sweep.py`): queue capacity swept across `{5, 20, 80}` in one ini
   using OMNeT++'s native `${var=...}` iteration syntax -- one CI step, three runs, one
   drop-rate-vs-capacity comparison table. This is the concrete "lever" M5 is about:
   instead of hand-editing a number and re-running, the sweep *is* the scenario.

**Scope boundary, stated honestly:** the sweep's per-iteration `.sca`/`.vec` files aren't
run through `analyze.py --strict` -- its sanity check assumes one module path maps to one
series, which a multi-iteration result directory violates (the same module path repeats
once per iteration). `summarize_sweep.py` is the sweep's own comparison report; per-
iteration model-correctness isn't independently re-verified here.

**M5 is done and green in CI. Both mechanisms worked on the first attempt**, and each
revealed something whole-run peak/final numbers couldn't show.

**Time-windowed report** confirms and sharpens the M2/M3 findings with temporal
resolution:

```
M2/M4 (uncongested): all peak activity in window 0 (0-15s, initial convergence
  transient) -- exactly 0.00us in windows 1-3. Confirms gPTP locks fast and stays
  essentially perfectly synced for the rest of the run.

M3 (congestion): coreClient.gptp peak per window = 1609.70 / 1949.64 / 1212.79 / 1302.22us
  -- sustained across all 4 windows, not a brief spike. The ~100x degradation is a
  steady-state condition tied to continuous congestion, not a one-time transient.
```

**Parameter sweep** (queue capacity x {5, 20, 80} on the M3 congestion traffic) produced a
real, textbook-instructive result -- not the naive expectation:

```
cap= 5   drop=342,635.3 ppm (34.26%)
cap=20   drop=340,960.9 ppm (34.10%)
cap=80   drop=341,802.7 ppm (34.18%)
```

**Drop rate is essentially flat regardless of a 16x change in queue capacity.** Under
*sustained* oversubscription (150Mbps offered vs 100Mbps link, continuously), loss rate
converges to `(offered - capacity) / offered` regardless of buffer depth -- bigger buffers
only add latency, they don't rescue you from persistent overload. This is the exact flip
side of M4's finding (buffer *size* mattered there, because M4's load was a transient
burst, not a sustained flood): together, M3/M4/M5 now tell a coherent, correct story about
when queue depth matters and when it doesn't.
