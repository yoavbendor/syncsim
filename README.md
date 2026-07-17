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

## Layout

```
Dockerfile                  # headless OMNeT++ + INET image
.github/workflows/ci.yml    # build image (cached) + run Minimal + archive results
simulations/
  minimal.ned / minimal.ini # M1: GM + 1 bridge + 2 clients, independent drifting clocks
scripts/
  run.sh                    # opp_run wrapper (headless Cmdenv)
  analyze.py                # export vectors -> derive offset-from-GM -> assertions
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
- **M3** — TSN switch realism (Qbv/Qav shapers, finite queues, real drops)
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
the signature of real physics, not a placeholder. `analyze.py` asserts `--max-offset-us 200`
in CI (`--strict`), well above the observed peaks but tight enough to catch regressions.

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
