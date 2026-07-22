# ns3/clock/ — Phase 1 (R-CLOCK): per-node steerable drift clock

Clean-room, permissively-licensed (Apache-2.0) reimplementation of INET's
`ConstantDriftOscillator` + `ClockBase` semantics for ns-3 — a **node-local
time** that runs as a linear (constant-rate-error) function of ns-3's single
global `Simulator::Now()`. This is the make-or-break precondition (`R-CLOCK` in
`NS3_MIGRATION_POC_PLAN.md`) for the whole migration: ns-3 natively has **no**
per-node clock — every node reads one global clock — so without this, syncsim's
entire premise (independent drifting clocks whose good synchronization is what
makes their bursts collide) cannot exist, and gPTP (Phase 2) has nothing to
steer.

## Files

| File | Role | License |
|---|---|---|
| `clock.h` / `clock.cc` | `syncsim::Clock` — the reusable clock class | Apache-2.0 (ours) |
| `clock-spike.cc` | Gate 1 proof scenario (`main`) | Apache-2.0 (ours) |

The whole subdir is dropped into a pinned ns-3.45 checkout's `scratch/` at Docker
build time (`scratch/syncsim-clock/`). ns-3's scratch build compiles all `.cc`
in the subdir together into **one** target named after the file containing
`main` — so this builds as target **`clock-spike`** →
`build/scratch/syncsim-clock/ns3.45-clock-spike`.

## Model

```
local(t) = localBase + (1 + ppm/1e6) * (t - t0)          t = Simulator::Now()
```

- Drift `ppm` set at construction (positive or negative).
- **Steerable** (what a gPTP servo calls in Phase 2):
  - `AdjustRate(ppmDelta)` — frequency correction. Re-anchors `t0`/`localBase` to
    now first, so local time is **C0-continuous** across the change: only the
    future slope bends, no jump.
  - `AdjustOffset(offsetDelta)` — phase correction. A deliberate step of local
    time.
- **Observable**: `GetLocalTime()`, `GetDriftPpm()`, plus a `TracedCallback`
  sample trace source (`Clock::Sample()` fires it) so `Simulator::Schedule`-driven
  sampling logs the local-vs-global trajectory the way Phase 2 / analysis tooling
  will need. A second trace fires on every steering action.

Deliberately a **plain C++ class driven by `Simulator::Now()`**, not an
`ns3::Object`/`NetDevice`/`Application` — matching the Phase-0 finding
(`smoke/smoke-topology.cc` header) that staying close to ns-3 core and out of the
Application/Socket layers was the only thing that behaved cleanly on this
ns-3.45 build. A passive clock (local time is a *pure function* of global time)
has nothing to schedule for itself; the scenario samples it on a timer.

## Clean-room provenance

Written from first principles. The three community ns-3 clock efforts named in
the survey — the 2025 "Clock Skew Models for ns-3" conference code, Lagwankar's
`bounded-clock-skew-with-probability` branch, LCA2-EPFL's `TSN-ATS-Clocks`
local-time module — were **not read or copied**; only their existence and
general approach (per the survey) were known. That keeps the copyright entirely
ours, so `clock.h/.cc/clock-spike.cc` carry an Apache-2.0 SPDX header, distinct
from the GPL-2.0-only headers on the unmodified ns-3 files.

**Honest licensing caveat** (also in the POC plan): the Apache-2.0 header covers
*our files' copyright only*. Because the class links against ns-3 core
(GPL-2.0-only), the **combined, distributed binary is still GPLv2**. Clean-room
buys (a) copyright ownership we may reuse/dual-license elsewhere and (b) freedom
from any single fork's terms and ns-3-version lock — it does **not** make the
ns-3 build itself permissive. If a truly-permissive *combined* artifact is ever
the real requirement, that argues against ns-3 as the base at all (a Gate-2
finding, not something to gloss).

## Gate 1 result — **PASSED** (sandbox, ns-3.45, release build, asserts on)

Built and run per `ns3/README.md`'s recipe. `clock-spike` exits `0` and prints
the numeric evidence below (byte-identical across two consecutive runs → the
spike is **deterministic**; it touches no RNG at all, so determinism is
structural, and the pinned `RngSeedManager` seed/run are only hygiene/parity).

**Two clocks at syncsim's M1 baseline drift rates (client1 +200 ppm, client2
−350 ppm)** diverge from each other and from global time at exactly the
configured rate; then at **t = 4 s** a simulated servo steers clock B only:

```
global(s) | A off us(+200) | B off us(-350) |       A-B us
------------------------------------------------------------------------
     0.00 |          0.000 |          0.000 |        0.000
     1.00 |        200.000 |       -350.000 |      550.000
     2.00 |        400.000 |       -700.000 |     1100.000
     3.00 |        600.000 |      -1050.000 |     1650.000
     4.00 |        800.000 |      -1400.000 |     2200.000   <- servo steers B here
     4.50 |        900.000 |          0.000 |      900.000
     5.00 |       1000.000 |          0.000 |     1000.000
     6.00 |       1200.000 |          0.000 |     1200.000
     7.00 |       1400.000 |          0.000 |     1400.000
     8.00 |       1600.000 |          0.000 |     1600.000
```

(`off us` = local − global in µs; full 0.5 s-interval table in the program
output.) Slopes recovered from the sampled trajectory (`ppm =
d(local−global)/d(global)`, the same offset-from-GM trick `analyze.py` uses):

```
  clock A  [0,4]s : 200.000 ppm (configured +200)
  clock B  [0,4]s : -350.000 ppm (configured -350)
  clock A  [4,8]s : 200.000 ppm (still +200, untouched control)
  clock B  [4,8]s : 0.000 ppm (steered to ~0 by AdjustRate(+350))
  clock B final offset-from-global : 0.000 us (steered to ~0 by AdjustOffset)
```

All six gate checks PASS:

- clock A drifts at configured +200 ppm; clock B at −350 ppm.
- the two clocks diverge monotonically from each other and from global time.
- `AdjustRate(+350)` bent clock B's frequency error to ~0 ppm (slope flattens).
- `AdjustOffset(+1400 µs)` stepped clock B's accumulated phase offset to ~0.
- the untouched control clock A kept drifting at +200 ppm — proving the steering
  was **local to one clock**, not a global effect.

The recovered slopes match the configured ppm to within the 0.5 ppm gate
tolerance (in practice exactly, at the ns-resolution / µs-scale drifts here).
Steerability is demonstrated by the trajectory of clock B **visibly bending** at
t = 4 s — both the frequency correction (offset stops growing) and the phase
correction (offset jumps from −1400 µs to 0) show up directly in the sampled
data, not as a printed claim.

### What this does and does not establish

- **Does:** ns-3 *can* host an independent, per-node, steerable drift clock,
  cleanly, with a permissive-on-our-copyright implementation — Gate 1's hard
  question. R-CLOCK is not a blocker.
- **Does not (by design, deferred to Phase 2):** no gPTP, no peer-delay, no
  automatic servo loop, no routing of packet timestamps through local time yet.
  The steering here is a scripted stand-in for the servo Phase 2 must build.

### Not yet confirmed in real CI

Same caveat as Gate 0: this sandbox has no Docker daemon, so the numbers above
are from a local ns-3.45 build, not the Dockerfile `ns3` stage in a clean CI
container. The `COPY ns3/clock "$NS3_ROOT/scratch/syncsim-clock"` line already
exists in the Dockerfile (added in Phase 0) and picks these files up
automatically; verifying the containerized build is the standing "CI proves it
reproduces" step.

## Reproduce locally (no Docker)

```bash
git clone --branch ns-3.45 --depth 1 https://gitlab.com/nsnam/ns-3-dev.git /tmp/ns-3-dev
cp -r ns3/clock /tmp/ns-3-dev/scratch/syncsim-clock
cd /tmp/ns-3-dev
./ns3 configure --build-profile=release \
    --enable-modules=core,network,csma,bridge,point-to-point,applications,internet,flow-monitor \
    --disable-examples --disable-tests --disable-python \
    --enable-asserts --enable-logs
./ns3 build clock-spike
./build/scratch/syncsim-clock/ns3.45-clock-spike     # exit 0 == Gate 1 PASS
```
