# ns3/gptp/ — Phase 2 (R-GPTP): minimal 802.1AS = M1 equivalent

Clean-room, permissively-licensed (Apache-2.0) minimal IEEE 802.1AS (gPTP)
mechanism for ns-3, built on the Phase-1 `syncsim::Clock`. This is the second
make-or-break gate (`R-GPTP` in `NS3_MIGRATION_POC_PLAN.md`): the sync half of
syncsim's feedback loop — peer-delay measurement, Sync/Follow_Up propagation
with residence-time correction at a time-aware bridge, and a servo that steers
each node's local clock toward the grandmaster. Passing **Gate 1 and Gate 2** is
the real "migration is viable" decision point.

## Files

| File | Role | License |
|---|---|---|
| `gptp.h` / `gptp.cc` | `syncsim::GptpHeader` + `syncsim::GptpEntity` — the reusable gPTP model (peer-delay, Sync relay, servo) | Apache-2.0 (ours) |
| `gptp-spike.cc` | Gate 2 proof scenario (`main`) — the M1-equivalent 4-node run | Apache-2.0 (ours) |
| `clock.h` / `clock.cc` | **Vendored, byte-identical copy** of Phase-1 `ns3/clock/`'s clock (see note below) | Apache-2.0 (ours) |

The whole subdir is dropped into a pinned ns-3.45 checkout's `scratch/` at Docker
build time (`scratch/syncsim-gptp/`). ns-3's scratch build compiles all `.cc` in
the subdir together into **one** target named after the file containing `main` —
so this builds as target **`gptp-spike`** →
`build/scratch/syncsim-gptp/ns3.45-gptp-spike`.

### Why `clock.{h,cc}` is vendored here

ns-3's scratch build makes each `scratch/<subdir>` its own independent target and
**cannot share a `.cc` across sibling scratch subdirs** — `syncsim-gptp` cannot
link `syncsim-clock/clock.cc`. Since the Dockerfile is intentionally limited to a
single `COPY ns3/gptp` line (per the Phase 2 task), this subdir must be
self-contained, so the Phase-1 clock source is copied in verbatim (confirmed
byte-identical to `ns3/clock/clock.{h,cc}`). It is the same clean-room,
Apache-2.0 clock, not a fork. If this track graduates past the POC, the clock
would move to a proper ns-3 module/`contrib` library shared by both, removing the
copy.

## The mechanism (what actually closes the loop)

Topology — identical shape to `simulations/minimal.ned` and Phase 0's
`smoke/smoke-topology.cc` (same CsmaHelper per-link construction reused):

```
  gm(0ppm) --- sw(80ppm) --- client1(200ppm)
                 \--------- client2(-350ppm)

  sw.port0 <-> gm      : SLAVE  (faces the GM)
  sw.port1 <-> client1 : MASTER
  sw.port2 <-> client2 : MASTER      (sw is slave AND master, like a real bridge)
```

1. **Peer delay** (per link, 1-step): each slave port sends `Pdelay_Req` at t1;
   the responder returns a `Pdelay_Resp` carrying its rx (t2) and tx (t3)
   timestamps; the requester computes
   `meanLinkDelay = ((t4 − t1) − (t3 − t2)) / 2`. Timestamps are read from the
   node's local `syncsim::Clock`.
2. **Sync + residence-time correction**: `gm` (drift-free, so its local time is
   global sim time) sends `Sync` carrying `preciseOriginTimestamp` and
   `correctionField = 0`. `sw` receives it on its slave port, reconstructs GM
   time as `origin + correction + peerDelay`, servos its own clock, and after a
   residence delay **regenerates** `Sync` on both master ports with
   `correctionField = upstreamCorrection + gm↔sw peerDelay + residenceTime`. Each
   client reconstructs `origin + correction + sw↔client peerDelay` and servos.
3. **Servo**: on every `Sync`, `offset = localTime − reconstructedGmTime`; a
   deadbeat **phase step** (`Clock::AdjustOffset`) nulls it, and an integral
   **frequency correction** (`Clock::AdjustRate`, gain 0.6) cancels the residual
   drift so the offset converges to ~0 instead of a bare sawtooth.

All timestamps route through node-local time — the invasive plumbing the survey
flagged as R-GPTP's core difficulty.

### The bridge's dual role is *not* an ordering problem

The correction-field mechanism forwards only **durations** (measured peer delay
+ residence time) plus the GM's origin timestamp, so a client's GM-time
reconstruction never depends on how well `sw` itself is synced. `sw` may servo
its own clock at any point relative to the relay without corrupting the clients.
(We still apply `sw`'s servo strictly *after* the downstream relay, so its phase
step cannot corrupt the residence duration read off its local clock.) What looked
like an unresolvable slave/master ordering problem dissolves.

## Simplifications (stated, not hidden — full detail in `gptp.h`)

- **S1 — timestamping.** "Hardware" timestamps are the scheduled-send-fires and
  receive-callback-fires instants, not INET's streaming-PHY SFD signals. The
  receive callback fires at end-of-reception, so a measured link delay folds in
  one frame serialization time on top of channel propagation — real, positive,
  stable, which is all Gate 2 asks.
- **S2 — 1-step variants.** `Pdelay_Resp` carries (t2, t3) directly (vs 2-step
  `Pdelay_Resp` + `Pdelay_Resp_Follow_Up`); `Sync` carries origin + correction
  directly (vs `Sync` + `Follow_Up`). Same information, fewer frames.
- **S3 — neighborRateRatio = 1.** Peer delay and residence are treated as
  equal-rate durations; over a few microseconds the residual rate error is
  sub-picosecond. Real 802.1AS carries the rate ratio for ptp4l-grade precision;
  a first spike does not need it.
- **S4 — per-port termination, no `BridgeNetDevice`.** Real gPTP frames use a
  link-local reserved multicast that bridges do not forward; each port is a gPTP
  endpoint and the bridge regenerates Sync. So (unlike Phase 0) no transparent L2
  bridge is installed — we keep the exact CsmaHelper link construction and attach
  our own per-device receive callbacks.

Message wire format is a pragmatic 19-byte custom `ns3::Header` (type + seq + two
int64 femtosecond fields); no IEEE TLV fidelity or pcap for this scenario (Gate 2
gates on convergence, not wire format — unlike Phase 0/real syncsim, which do
care about pcap).

## Gate 2 result — **PASSED** (sandbox, ns-3.45, release build, asserts on)

Built and run per the recipe below. `gptp-spike` exits `0` and prints the numeric
evidence below. **Deterministic**: byte-identical stdout across two consecutive
runs (`md5sum` matched); the spike touches no RNG, so determinism is structural.

20 s run, Sync interval 0.125 s (INET's default; 159 servo corrections/node),
peer-delay interval 0.05 s.

Measured peer delays (per link) — small, positive, stable:

```
  gm <-> sw      : 6.620 us
  sw <-> client1 : 6.620 us
  sw <-> client2 : 6.620 us     (1us channel Delay + one 100Mbps frame serialization)
```

Offset-from-GM trajectory (local − reconstructed GM, µs) — the transient peak,
then convergence to ~0:

```
global(s) |   sw   (80ppm) | client1 (200ppm) | client2(-350ppm)
------------------------------------------------------------------------
   0.1250 |         9.5000 |          24.0020 |         -44.7560
   0.2500 |        10.0000 |          25.0000 |         -43.7500   <- peak (~|drift|*interval)
   0.3750 |         4.0010 |          10.0020 |         -17.5010
   0.5000 |         1.5990 |           3.9990 |          -6.9990
   0.6250 |         0.6400 |           1.5990 |          -2.8010
   0.7500 |         0.2560 |           0.6400 |          -1.1190
   2.1250 |         0.0000 |           0.0000 |           0.0000
   ...    |         0.0000 |           0.0000 |           0.0000   (holds to t=20s)
```

Per-node summary vs the INET M1 baseline:

```
              node |    peak us |    final us |     peak/|ppm| | servos
------------------------------------------------------------------------
      sw   (80ppm) |     10.000 |       0.000 |          0.125 | 159
  client1 (200ppm) |     25.000 |       0.000 |          0.125 | 159
  client2(-350ppm) |     44.756 |       0.000 |          0.128 | 159
  INET M1 baseline : sw ~10.00,  client1 ~25.01,  client2 ~43.76   (all final ~0.00)
```

All five gate checks PASS:

- **final offset near 0** — every node converges to 0.000 µs (well under the
  2 µs tolerance) and holds there for the whole run.
- **peak order client2 > client1 > sw** — 44.76 > 25.00 > 10.00 µs.
- **peak roughly proportional to |drift|** — `peak/|ppm|` = 0.125 / 0.125 / 0.128
  (within 30 %; in fact within ~2 %).
- **peer delays measured, positive and small** — 6.62 µs each.
- **servo closed the loop** — 159 corrections per node.

The peaks land almost exactly on INET's M1 numbers (sw 10.00 vs ~10.00,
client1 25.00 vs ~25.01, client2 44.76 vs ~43.76 µs) — but per the POC plan the
gate is the **mechanism** (convergence + drift-proportional peak ordering), not
matching those digits, which would be a fool's errand across two independent
servo/rounding implementations. The near-match is because both this spike and
INET's default gPTP use a 0.125 s Sync interval, so the transient peak is
`|drift| × interval` in both. client2's 44.76 µs (vs the pure-sawtooth 43.75)
reflects its first Sync arriving slightly after t = 0.125 s (relay + two link
delays), so a touch more drift accumulated — physically correct, not a rounding
artifact.

### What this does and does not establish

- **Does:** ns-3 *can* host a minimal but real, closed-loop 802.1AS mechanism —
  per-link peer delay, correction-field propagation through a dual slave/master
  bridge, and a servo steering the Phase-1 clock — reproducing M1's signature
  deterministically. R-GPTP is not a blocker. Gates 1 + 2 both hold → the POC's
  core "migration is viable" question is answered *yes* on the pinned ns-3.45.
- **Does not (deferred to later phases):** no multi-hop residence bridges (M2 /
  R-BRIDGE), no data-plane congestion coupling (M3/M4), no neighborRateRatio
  (S3), no streaming-PHY-grade timestamps (S1), no IEEE TLV wire format / pcap.
  The servo is a phase+integral-frequency loop, not a tuned ptp4l PI2.

### Honest licensing note

Same as `clock/README.md`: the Apache-2.0 SPDX header covers *our files'
copyright only*. Because this links against ns-3 core (GPL-2.0-only), the
**combined, distributed binary is still GPLv2**. Clean-room buys copyright
ownership (reusable/dual-licensable elsewhere) and freedom from any one fork's
terms — it does not make the ns-3 build permissive.

### Not yet confirmed in real CI

Same caveat as Gates 0/1: this sandbox has no Docker daemon, so the numbers above
are from a local ns-3.45 build, not the Dockerfile `ns3` stage in a clean CI
container. The `COPY ns3/gptp "$NS3_ROOT/scratch/syncsim-gptp"` line (added in
this phase) picks these files up automatically; verifying the containerized build
is the standing "CI proves it reproduces" step.

## Reproduce locally (no Docker)

```bash
git clone --branch ns-3.45 --depth 1 https://gitlab.com/nsnam/ns-3-dev.git /tmp/ns-3-dev
cp -r ns3/gptp /tmp/ns-3-dev/scratch/syncsim-gptp
cd /tmp/ns-3-dev
./ns3 configure --build-profile=release \
    --enable-modules=core,network,csma,bridge,point-to-point,applications,internet,flow-monitor \
    --disable-examples --disable-tests --disable-python \
    --enable-asserts --enable-logs
./ns3 build gptp-spike
./build/scratch/syncsim-gptp/ns3.45-gptp-spike     # exit 0 == Gate 2 PASS
```
