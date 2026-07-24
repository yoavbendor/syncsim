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

1. **Peer delay** (per link, 2-step as of P2b): each slave port sends
   `Pdelay_Req` at t1; the responder returns a `Pdelay_Resp` carrying its rx (t2),
   then a `Pdelay_Resp_Follow_Up` carrying its tx (t3); the requester (holding
   pending state between the two) computes
   `meanLinkDelay = ((t4 − t1) − (t3 − t2)) / 2`. Timestamps are read from the
   node's local `syncsim::Clock`.
2. **Sync + residence-time correction** (2-step as of P2b): `gm` (drift-free, so
   its local time is global sim time) sends a bare `Sync` marker followed by a
   `Follow_Up` carrying `preciseOriginTimestamp` and `correctionField = 0`. `sw`
   records the bare `Sync`'s arrival, and on the `Follow_Up` reconstructs GM time
   as `origin + correction + peerDelay`, servos its own clock, and after a
   residence delay **regenerates** a bare `Sync` + `Follow_Up` on both master
   ports with `correctionField = upstreamCorrection + gm↔sw peerDelay +
   residenceTime`. Each client reconstructs `origin + correction + sw↔client
   peerDelay` and servos.
3. **Servo (hardened, P1a)**: on every `Sync`, `offset = localTime −
   reconstructedGmTime` drives a **PI control loop** — a damped **proportional
   phase** term (`Clock::AdjustOffset`, gain 0.7) plus a **bounded, low-pass
   integral frequency** term (`Clock::AdjustRate`) that accumulates a correction
   clamped to ±500 ppm, normalized by the *nominal* Sync interval (never the
   actual elapsed), and **skipped on any cycle whose gap signals a missed Sync**.
   The offset converges to ~0 instead of a bare sawtooth. This replaces the
   Phase-2 first-spike servo (deadbeat phase + a fresh, unbounded `offset/elapsed`
   rate estimate each cycle) that over-reacted to a ballooned gap after a dropped
   Sync — see the servo block in `gptp.h` and the P1a note below. A companion
   **peer-delay outlier filter** (running-minimum estimator, also P1a) protects
   the loop's *input*: it rejects a link-delay sample inflated by the shared-medium
   contention artifact (M3), which would otherwise inject a false offset the servo
   would faithfully chase.

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
- **S2 — 2-step framing — CLOSED (P2b).** Formerly 1-step. Now split into the
  real two-message form: `Pdelay_Resp` (t2 only) + `Pdelay_Resp_Follow_Up` (t3),
  and a bare `Sync` marker + `Follow_Up` (preciseOriginTimestamp + correction).
  Requester/slave hold pending state between each pair. Verified informationally
  identical to 1-step in lossless conditions (Gate 2/M2/M4 match to ≤ 1 ns); the
  one honest exception is M3's heavy-loss regime — see `congestion/README.md`.
- **S3 — neighborRateRatio — CLOSED (P2a).** Formerly assumed = 1. Now derived
  per link from two successive Pdelay exchanges (`neighborRateRatio =
  (t3_now − t3_prev) / (t4_now − t4_prev)` — neighbor-elapsed / local-elapsed)
  and folded into the peer-delay turnaround and the bridge residence-time
  correction. As S3 always predicted, no observed number moved to 3+ sig figs;
  post-servo-lock the measured ratio is ~1 (< 0.5 ppb residual, printed in the
  gate output) because the servo steers the local clock's *rate* to GM. See the
  P2a note below.
- **S4 — per-port termination, no `BridgeNetDevice`.** Real gPTP frames use a
  link-local reserved multicast that bridges do not forward; each port is a gPTP
  endpoint and the bridge regenerates Sync. So (unlike Phase 0) no transparent L2
  bridge is installed — we keep the exact CsmaHelper link construction and attach
  our own per-device receive callbacks.

Message wire format is the **byte-exact IEEE 802.1AS-2011 format as of P3c**
(Tier 3) — a 34-byte common PTP header + per-type bodies + the Follow_Up
Information TLV and Announce Path Trace TLV, on the real PTP EtherType 0x88F7 to
the reserved gPTP multicast `01-80-C2-00-00-0E`. It replaced the pragmatic
19-byte custom header used through P2c. Captures are now **genuinely dissectable
by tshark/Wireshark's own PTPv2 dissector** (verified zero-malformed);
`gptp-spike.cc` gained an opt-in `--pcapPrefix`. **Two frames per Pdelay exchange
and per Sync cycle as of P2b (2-step)**, plus a GM-only additive Announce as of
P3c. Full byte tables, the tshark transcript, and the one disclosed frame-size
number change are in **`WIRE_FORMAT.md`**.

**2-step framing note (P2b — S2 closed).** Splitting `Pdelay_Resp` into
`Pdelay_Resp` (t2) + `Pdelay_Resp_Follow_Up` (t3), and `Sync` into a bare marker
+ `Follow_Up` (origin + correction), left the Gate-2 numbers informationally
identical: peaks moved ≤ 1 ns (`client2 57.177 → 57.178 µs`), every final is
`0.000 µs`, and servo counts are unchanged at 159. (The downsampled trajectory
table now prints slightly later global sample instants — the offset trace fires
at `Follow_Up` receipt rather than `Sync` receipt — but the underlying
convergence is bit-identical, all `0.0000` from ~2 s on.) This empirically
confirms S2's "informationally identical" claim for the lossless case; the one
regime where 2-step genuinely differs is M3's heavy-loss queue
(`congestion/README.md`).

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
   0.2500 |        12.8500 |          32.2010 |         -57.1770   <- peak
   0.3750 |         8.7160 |          28.4110 |         -54.6520
   0.5000 |         3.9880 |          21.0230 |         -47.6470
   0.6250 |         0.9750 |          12.5580 |         -39.2930
   0.7500 |        -0.3190 |           4.9940 |         -30.5380
   2.1250 |        -0.0010 |           0.0030 |          -0.0460
   4.1250 |         0.0000 |           0.0000 |           0.0000
   ...    |         0.0000 |           0.0000 |           0.0000   (holds to t=20s)
```

Per-node summary vs the INET M1 baseline:

```
              node |    peak us |    final us |     peak/|ppm| | servos
------------------------------------------------------------------------
      sw   (80ppm) |     12.850 |       0.000 |          0.161 | 159
  client1 (200ppm) |     32.201 |       0.000 |          0.161 | 159
  client2(-350ppm) |     57.177 |       0.000 |          0.163 | 159
  INET M1 baseline : sw ~10.00,  client1 ~25.01,  client2 ~43.76   (all final ~0.00)
```

All five gate checks PASS:

- **final offset near 0** — every node converges to 0.000 µs (well under the
  2 µs tolerance) and holds there for the whole run.
- **peak order client2 > client1 > sw** — 57.18 > 32.20 > 12.85 µs.
- **peak roughly proportional to |drift|** — `peak/|ppm|` = 0.161 / 0.161 / 0.163
  (within 30 %; in fact within ~1 %).
- **peer delays measured, positive and small** — 6.62 µs each.
- **servo closed the loop** — 159 corrections per node.

**Servo-change note (P1a).** The transient peaks are now ~1.29× the ideal
deadbeat `|drift| × interval` (peak/|ppm| ≈ 0.161 vs the old 0.125), and the
loop takes ~2 clean Sync cycles rather than 1 to settle. That is the deliberate,
disclosed cost of the hardened servo's **damped** proportional phase (gain 0.7,
vs the old deadbeat 1.0): a fraction of each single Sync's jitter is now rejected
instead of stepped in whole. What the Gate-2 gate actually checks — **convergence
to ~0** and **drift-proportional peak ordering** — is *tighter* than before
(peak/|ppm| is now uniform to ~1 %, vs ~2 % before), so the mechanism is if
anything cleaner. The absolute peaks no longer sit on INET's M1 digits, and per
the POC plan they were never a match target: the gate is the mechanism, not the
digits (matching two independent servos' rounding would be a fool's errand). The
hardening exists to fix M3's congested-peak magnitude (see `congestion/README.md`,
46,281 µs → 510 µs) without regressing any gate; this clean-scenario transient
shift is that fix's only visible footprint here, and it is a strict wash on the
gated properties.

**neighborRateRatio note (P2a — S3 closed).** The gate output now prints the
per-link measured `neighborRateRatio` as a residual deviation from 1.0 in ppb.
Post-lock all three links read `1 + 0.000 ppb` (residual < 0.5 ppb): the servo
steers each local clock's *rate* to match GM, so once locked the neighbour and
local clocks genuinely tick at the same rate and the measured ratio is ~1. Folding
this into the peer-delay turnaround and residence-time math therefore leaves the
Gate-2 numbers unchanged to 3+ sig figs — one downsampled trajectory sample moved
by 1 ns (`client2 −54.652 → −54.653 µs` at t = 0.375 s, a pre-lock transient
point), and the peer delay `sw↔client2` reads `6.618` vs `6.617 µs`; nothing else
moved. This is exactly S3's long-standing prediction (sub-ps at these
drift/timescales), now empirically confirmed rather than asserted.

### What this does and does not establish

- **Does:** ns-3 *can* host a minimal but real, closed-loop 802.1AS mechanism —
  per-link peer delay, correction-field propagation through a dual slave/master
  bridge, and a servo steering the Phase-1 clock — reproducing M1's signature
  deterministically. R-GPTP is not a blocker. Gates 1 + 2 both hold → the POC's
  core "migration is viable" question is answered *yes* on the pinned ns-3.45.
- **Does not (deferred to later phases):** no multi-hop residence bridges (M2 /
  R-BRIDGE), no data-plane congestion coupling (M3/M4), no
  streaming-PHY-grade timestamps (S1), no IEEE TLV wire format / pcap.
  (**S3 `neighborRateRatio` is now closed — P2a**, see the note above.)
  The servo is a hardened PI loop (damped proportional phase + bounded low-pass
  integral frequency + missed-Sync skip + a peer-delay outlier filter on its
  input), not a full adaptive ptp4l PI2 with spike-rejection state machine.

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
