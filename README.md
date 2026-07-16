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
docker run --rm -v "$PWD:/work" syncsim bash scripts/run.sh Minimal simulations/minimal.ini results
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

## Status / known verification points

The first CI build is the toolchain's proving ground (a full INET build takes ~20–30 min,
cached thereafter). Two things get confirmed from that first green run and are handled
defensively until then:
1. exact INET 4.5.4 parameter paths in `minimal.ini` (roles, oscillator, reference clock);
2. the exact recordable clock-time signal name used by `analyze.py`.
