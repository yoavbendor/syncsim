# ns3/nominal/ — Phase 3 (M2 / R-BRIDGE): multi-hop time-aware bridges

Clean-room, permissively-licensed (Apache-2.0) proof that syncsim's M2
("Nominal") multi-hop gPTP scenario reproduces on ns-3, built on the Phase-1
`syncsim::Clock` and the Phase-2 gPTP mechanism (hardened in **P1a**, applied
identically across all four scenarios — no nominal-specific change). This is the
`R-BRIDGE` risk in `NS3_MIGRATION_POC_PLAN.md` — "multi-hop time-aware bridges
with residence-time correction, the riskiest gPTP piece" — chaining the exact
dual slave+master bridge role Phase 2 proved once, two levels deep.

## The question this phase answers

Phase 2's `GptpEntity` (1 slave port + N master ports, additive correction
field `upstreamCorrection + linkDelay + residence`) was written generically —
nothing in it assumes exactly one bridge hop. **Does it already generalize to a
chain of bridges (multi-hop) with zero changes to `gptp.h`/`gptp.cc`?**

**Finding: yes — zero multi-hop-specific changes.** `gptp.{h,cc}` here are a
**byte-identical vendored copy** of `ns3/gptp/`'s (confirmed by `md5sum`) — the
P1a-hardened servo, vendored identically into all four scenario dirs; nothing in
it was touched to make multi-hop work. The correction-field accumulation composes
hop-by-hop by construction:

- `gm` sources `Sync` with `correctionField = 0`.
- `swCore` (slave toward `gm`) reconstructs GM time as
  `origin + 0 + d(gm,swCore)`, servos, and after its residence time regenerates
  `Sync` on all four master ports with
  `correctionField = d(gm,swCore) + residence_swCore`.
- each zone switch `swA/B/C` (slave toward `swCore`) reconstructs
  `origin + [d(gm,swCore)+residence_swCore] + d(swCore,swZone)`, servos, and
  regenerates with the accumulated
  `correctionField = [d(gm,swCore)+residence_swCore] + d(swCore,swZone) + residence_swZone`.
- each leaf client reconstructs GM time from the **full accumulated
  correction** plus its own last-hop peer delay, and servos.

Nothing in `HandleSync` / `RelayDownstream` / `ApplyServo` counts hops or
assumes depth 1: a bridge is simply "a node with >1 port"
(`m_ports.size() == 1` is the *end-station* branch), and the additive
correction field means each hop only ever adds its own two locally-measured
durations (its slave-side peer delay + its residence time). `swCore` **and**
each zone switch play the identical dual role the single Phase-2 `sw` proved.

The only new code in this phase is the topology builder + scenario driver
(`nominal-topology.cc`) — 18 nodes across three hop depths, wired the same way
`gptp-spike.cc` wires one bridge.

### Why `clock.{h,cc}` and `gptp.{h,cc}` are vendored here

Same constraint Phase 2 hit and documented: ns-3's scratch build makes each
`scratch/<subdir>` an independent target and **cannot share a `.cc` across
sibling scratch subdirs**, and the Dockerfile is intentionally one `COPY
ns3/nominal` line. So this subdir is self-contained: `clock.{h,cc}` and
`gptp.{h,cc}` are copied in verbatim (confirmed byte-identical to
`ns3/clock/` and `ns3/gptp/`). They are the same clean-room Apache-2.0 sources,
not forks. **The vendored gptp being byte-identical is itself the headline
result** — the multi-hop generalization needed no model change (the P1a servo
hardening is a shared, scenario-agnostic change applied to that one source of
truth, not a nominal-specific edit).

## Files

| File | Role | License |
|---|---|---|
| `nominal-topology.cc` | M2 proof scenario (`main`) — the 18-node multi-hop run | Apache-2.0 (ours) |
| `gptp.h` / `gptp.cc` | **Vendored byte-identical** from `ns3/gptp/` (P1a-hardened servo) | Apache-2.0 (ours) |
| `clock.h` / `clock.cc` | **Vendored byte-identical** from `ns3/clock/` | Apache-2.0 (ours) |

Builds as target **`nominal-topology`** →
`build/scratch/syncsim-nominal/ns3.45-nominal-topology`.

## Topology (from `simulations/nominal.ned` / `nominal.ini`)

```
  gm(0ppm)
    |                                       hop 1: swCore
  swCore(50ppm)
    |-- coreClient(150ppm)                  hop 2: coreClient, swA, swB, swC
    |-- swA(80ppm)  --- clientsA[0..3]       hop 3: the 12 zone clients
    |-- swB(-60ppm) --- clientsB[0..3]
    |-- swC(100ppm) --- clientsC[0..3]

  swCore: eth0=gm (SLAVE), eth1..eth4 (MASTER, to coreClient/swA/swB/swC)
  swZone: eth0=swCore (SLAVE), eth1..eth4 (MASTER, to its 4 clients)
```

18 nodes, 17 links, 100 Mbps uniform. Drift rates match `nominal.ini`
(`gm`=0, `swCore`=50, `swA`=80, `swB`=−60, `swC`=100, `coreClient`=150 ppm);
the 12 zone clients draw `uniform(−200,200) ppm` from ns-3's own seeded
`UniformRandomVariable` (seed=1, run=1) — a deterministic per-client spread
across INET's range, **not** an attempt to match INET's specific RNG draws
(impossible across two RNGs, and not the point per the task).

## Result — **GATE PASS** (sandbox, ns-3.45, release build, asserts on)

`nominal-topology` exits `0`. **Deterministic**: byte-identical stdout across
two consecutive runs (`md5sum` matched); the only RNG use is the 12 seeded
client drift draws, so determinism is structural under the pinned seed/run.

60 s run (P2d: was 30 s), Sync interval 0.125 s (INET default; 479 servo corrections/node),
peer-delay interval 0.05 s.

### Seeded client drift draws (deterministic)

```
  clientsA[0]=+126.61  A[1]=+42.74  A[2]=−1.77   A[3]=−47.36  ppm
  clientsB[0]=−52.37   B[1]=+122.64 B[2]=−159.65 B[3]=+33.92  ppm
  clientsC[0]=+175.62  C[1]=+50.44  C[2]=+128.78 C[3]=+23.71  ppm
```

### Per-node offset-from-GM (every one of the 18 nodes)

```
           node | hops |   ppm |  peak us | final us | servos
  --------------------------------------------------------------
         swCore |  1   |  50.0 |    7.975 |    0.000 | 479
     coreClient |  2   | 150.0 |   24.076 |    0.000 | 479
            swA |  2   |  80.0 |   12.701 |    0.000 | 479
            swB |  2   | −60.0 |   10.050 |    0.000 | 479
            swC |  2   | 100.0 |   15.951 |    0.000 | 479
    clientsA[0] |  3   | 126.6 |   20.127 |    0.000 | 479
    clientsA[1] |  3   |  42.7 |    6.495 |    0.000 | 479
    clientsA[2] |  3   |  −1.8 |    1.722 |    0.000 | 479
    clientsA[3] |  3   | −47.4 |    8.147 |    0.000 | 479
    clientsB[0] |  3   | −52.4 |    8.961 |    0.000 | 479
    clientsB[1] |  3   | 122.6 |   19.482 |    0.000 | 479
    clientsB[2] |  3   |−159.6 |   26.395 |    0.000 | 479
    clientsB[3] |  3   |  33.9 |    5.062 |    0.000 | 479
    clientsC[0] |  3   | 175.6 |   28.090 |    0.000 | 479
    clientsC[1] |  3   |  50.4 |    7.747 |    0.000 | 479
    clientsC[2] |  3   | 128.8 |   20.479 |    0.000 | 479
    clientsC[3] |  3   |  23.7 |    3.402 |    0.000 | 479
```

**Every one of the 18 nodes converges to 0.000 µs final and holds** (well under
the 2 µs tolerance), across all three hop depths. That is the gate. (Peaks are
~1.29× the pre-P1a values — `coreClient` 24.08 vs the old 18.75, etc. — because
the hardened servo's **damped** proportional phase, gain 0.7, takes ~2 Sync
cycles rather than 1 to settle the startup transient; `final` and the gate are
unchanged. See `gptp/README.md`'s servo-change note. `clientsA[2]` at −1.8 ppm is
unchanged at 1.722 µs — its peak is set by first-Sync latency, not the servo gain,
so the damping does not touch it.)

### Peak grouped by hop depth (INET's own reporting shape)

```
  hops | nodes | mean peak us | max peak us
  -------------------------------------------
     1 |   1   |     7.975    |    7.975
     2 |   4   |    15.694    |   24.076
     3 |  12   |    13.009    |   28.090
  INET M2 reference (orientation only): hops=1 ~7.36; hops=2 mean 12.19 max 18.75;
                                        hops=3 mean 8.40 max 17.90 us
```

Representative measured peer delays (mechanism sanity) — small, positive,
stable: `gm↔swCore`, `swCore↔swA`, `swA↔clientsA[0]` all **6.620 µs** (1 µs
channel delay + one 100 Mbps frame serialization, per S1).

### The honest, non-obvious finding — reproduced

Peak error does **not** grow monotonically with hop count. **hops=3 mean
(13.01 µs) is *lower* than hops=2 mean (15.69 µs)** — the same qualitative
finding INET's own M2 run reported. The mechanism is visible in the per-node
table: **each node's peak tracks its own `|drift|`, not its depth** — it lands
almost exactly on `|ppm| × 0.16 s` (the hardened servo's damped-phase transient
constant; swCore 50 ppm → 7.98; coreClient 150 ppm → 24.08; swA 80 ppm → 12.70;
clientsB[2] −159.6 ppm → 26.39 ≈ 25.5 + multi-hop first-Sync latency). hops=2's
mean is pulled up only because
`coreClient` (150 ppm) and `swC` (100 ppm) happen to have larger drift than the
average of the 12 seeded zone-client draws. So the hop-by-hop
peer-delay + residence-time correction actively **prevents** upstream error from
compounding: each node's local drift between corrections dominates its error far
more than accumulated upstream offset does — exactly the effect the correction
field is designed to produce. (Low-drift nodes like `clientsA[2]` at −1.8 ppm
still show a ~1.7 µs peak, not ~0.2 µs, because the very first Sync reaches a
depth-3 leaf ~40 µs after t = 0.125 s — three link delays + two residence times
later — so a little extra drift accumulates before the first correction. Real,
not an artifact.)

Our absolute numbers differ from INET's in detail (different seeded drift draws,
coarser S1 timestamping) — but per the POC plan the gate is the **mechanism**
(every node converges near 0 across all hop depths, peak tracks local drift, no
unbounded compounding with depth), not the digits. That mechanism reproduces
faithfully.

**S3 note (closed by P2a) + S2 note (closed by P2b).** `neighborRateRatio` is now
derived per link and folded into the peer-delay/residence math (P2a), and the
Pdelay/Sync messages are now real 2-step (P2b). On M2 both are negligible: a
handful of **transient-peak** values shifted by **≤ 1 ns** (last printed digit —
e.g. `coreClient` peak `24.075 → 24.076 µs`; the hop-3 leaf peaks moved by ≤ 1 ns
under P2b's extra frame/cycle), every node's **final** offset unchanged at
`0.000 µs`, and all 479 servo counts unchanged. The reasons are physical: the
servo steers each clock's *rate* to GM (so post-lock `neighborRateRatio` ≈ 1,
< 0.5 ppb), and the 2-step bare Sync occupies the same wire slot as the old
combined Sync with the residence correction self-compensating for the extra
Sync→Follow_Up gap — so downstream offsets are preserved. (M3's heavy-loss queue
is the one place 2-step genuinely differs; see `congestion/README.md`.) The M2
gate is unchanged (all 18 nodes converge, all PASS).

## What this does and does not establish

- **Does:** the Phase-2 `GptpEntity` — additive correction field, dual
  slave/master bridge role — generalizes to a **chain** of time-aware bridges
  two hops deep with **zero model changes**. Hop-by-hop peer-delay +
  residence-time propagation works through a multi-bridge tree; every node
  across three hop depths converges near 0 and holds; peak error tracks local
  drift and does not compound monotonically with depth. R-BRIDGE is not a
  blocker.
- **Does not (deferred):** data-plane congestion coupling (M3/M4 — this run has
  no background/burst traffic and no finite-queue contention),
  streaming-PHY-grade timestamps (S1), **IEEE-TLV-dissectable** pcap (own-format
  pcap capture IS available now — P2c, `--pcapPrefix`, off by default, verify with
  `ns3/scripts/check_pcap_gptp.py`; the IEEE TLV wire format is Tier 3). Carries
  forward S1/S4 unchanged; **S2 (2-step framing) is closed by P2b** and **S3
  (`neighborRateRatio`) is closed by P2a** (see the S2/S3 note above — both
  ≤ 1 ns on M2).

### Honest licensing note

Same as `clock/README.md` and `gptp/README.md`: the Apache-2.0 SPDX header
covers *our files' copyright only*. Because this links against ns-3 core
(GPL-2.0-only), the **combined, distributed binary is still GPLv2**. Clean-room
buys copyright ownership and freedom from any one fork's terms — it does not make
the ns-3 build permissive.

### Not yet confirmed in real CI

Same caveat as Gates 0/1/2: this sandbox has no Docker daemon, so the numbers
above are from a local ns-3.45 build, not the Dockerfile `ns3` stage in a clean
CI container. The `COPY ns3/nominal "$NS3_ROOT/scratch/syncsim-nominal"` line
(added this phase) picks these files up automatically; verifying the
containerized build is the standing "CI proves it reproduces" step.

## Reproduce locally (no Docker)

```bash
git clone --branch ns-3.45 --depth 1 https://gitlab.com/nsnam/ns-3-dev.git /tmp/ns-3-dev
cp -r ns3/nominal /tmp/ns-3-dev/scratch/syncsim-nominal
cd /tmp/ns-3-dev
./ns3 configure --build-profile=release \
    --enable-modules=core,network,csma,bridge,point-to-point,applications,internet,flow-monitor \
    --disable-examples --disable-tests --disable-python \
    --enable-asserts --enable-logs
./ns3 build nominal-topology
./build/scratch/syncsim-nominal/ns3.45-nominal-topology   # exit 0 == GATE PASS
```
