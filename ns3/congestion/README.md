# ns3/congestion/ — Phase 3 (M3): finite queues + background congestion

Clean-room, permissively-licensed (Apache-2.0) proof that syncsim's M3
("congestion") scenario reproduces on ns-3, built on the Phase-1 `syncsim::Clock`
and the Phase-2/M2 gPTP mechanism (hardened in **P1a** — bounded PI servo +
missed-Sync handling + peer-delay outlier filter; see below). Reproduces
`simulations/congestion.ini`.

## The finding this phase reproduces

> **Congestion doesn't degrade sync globally — it degrades sync specifically for
> whoever shares the congested egress queue with the data traffic.**

Every switch port gets a finite, real-dropping `DropTailQueue` (`packetCapacity =
10`). Background data traffic (three ~50 Mbps flows) is offered onto the single
`swCore↔coreClient` 100 Mbps link — a guaranteed ~150-into-100 Mbps
oversubscription. gPTP Sync frames toward `coreClient` share that exact congested
egress queue, so `coreClient`'s sync degrades; no other node's gPTP path touches
that queue, so every other node is untouched.

## Why gptp/clock are vendored here

Same constraint every prior phase hit: ns-3's scratch build makes each
`scratch/<subdir>` an independent target and **cannot share a `.cc` across
sibling scratch subdirs**, and the Dockerfile is one `COPY ns3/congestion` line.
So `clock.{h,cc}` and `gptp.{h,cc}` are copied in **byte-identical** (confirmed by
`md5sum` against `ns3/nominal/` = `ns3/gptp/` = `ns3/feedback/`). The M3 data-plane
+ congestion machinery is entirely in the topology driver; the gPTP model is the
P1a-hardened `gptp.{h,cc}`, vendored identically across all four scenario dirs.

## Files

| File | Role | License |
|---|---|---|
| `congestion-topology.cc` | M3 proof scenario (`main`) — 18-node run, baseline vs congested | Apache-2.0 (ours) |
| `gptp.h` / `gptp.cc` | **Vendored byte-identical** from `ns3/gptp/` (P1a-hardened servo) | Apache-2.0 (ours) |
| `clock.h` / `clock.cc` | **Vendored byte-identical** from `ns3/clock/` | Apache-2.0 (ours) |

Builds as target **`congestion-topology`** →
`build/scratch/syncsim-congestion/ns3.45-congestion-topology`.

## How the coupling arises (the mechanism)

gPTP frames and data frames share the **same per-device CSMA egress queue**: both
are emitted via `NetDevice::Send()` on the same `CsmaNetDevice`, which owns one
finite `DropTailQueue` (Phase 0's proven drop mechanism). On the
`swCore→coreClient` device, three ~50 Mbps flows are offered onto a 100 Mbps link,
so that queue runs full and drops ~1/3 of everything in it. The gPTP Sync/Pdelay
frames `swCore` regenerates toward `coreClient` must sit in — or be dropped from —
that same congested queue, so `coreClient`'s Sync arrives late (extra unmodeled
queueing delay folds into `recvLocal − reconstructedGmTime`) or not at all (a
missed servo cycle). Every other switch egress carries only negligible gPTP, never
fills, and those nodes are untouched. **The asymmetry emerges from the shared-queue
mechanism — it is not hand-coded per node.**

## Simplifications (Phase-2's S1/S4 carried forward; S2 closed by P2b, S3 by P2a)

**S3 (`neighborRateRatio`) is closed as of P2a** — derived per link from
consecutive Pdelay exchanges and folded into the peer-delay/residence math. Its
only effect was a **≤ 2 ns** shift on peaks (last printed digit; measured in
isolation on the 1-step build it moved the congested peak `510.471 → 510.473 µs`);
the isolation shape, drop rate, backlog, and every gate are unchanged. The
peer-delay outlier filter (P1a) already guards `neighborRateRatio` too — the same
congestion-inflated `t4` the running-minimum rejects is rejected by P2a's
`>1%`-off-unity ratio guard.

**S2 (2-step framing) is closed as of P2b** — and, unlike S3, it *does* change
this scenario's headline numbers (congested peak `510.473 → 429.207 µs`, servo
count `170 → 90`), for a real, protocol-faithful loss-statistics reason detailed
in "P2b genuinely changes this one scenario" below. S1/S4 carry forward unchanged.

**S5 — background flows injected at their convergence egress.** The three flows
are offered on `swCore`'s `coreClient`-facing device directly, rather than
L2-forwarded hop-by-hop from `clientsA/B/C[0]` through `swA/B/C`. Reason: ns-3's
`CsmaChannel` is a single **shared CSMA/CD medium** (mainline ns-3.45 has no
full-duplex CSMA; a full-duplex `PointToPointNetDevice` was tried but its PPP
framer rejects the vendored gPTP ethertype `0x88b6`, and `gptp.cc` must stay
byte-identical). On a shared medium, 50 Mbps of transit data on a zone link
spuriously delays the **reverse-direction** gPTP Sync on that same medium — which,
confirmed empirically (a first hop-by-hop-forwarding attempt), coupled **every**
node's sync to the load and destroyed the isolation the finding is about (every
zone switch degraded to ~5 ms). INET's Ethernet links are **full-duplex**
(independent tx/rx), which is exactly why INET gets clean isolation. Injecting the
aggregate at the one genuinely oversubscribed egress reproduces the real
phenomenon — the shared bottleneck queue `coreClient`'s Sync competes in — without
the shared-medium artifact on transit links. The convergence, the oversubscription,
the real drops, and the shared-queue coupling all land exactly where
`congestion.ini` puts them: the `swCore→coreClient` egress. Zone links carry only
gPTP and are therefore isolated — INET's result.

## Result — **GATE PASS** (sandbox, ns-3.45, release build, asserts on)

`congestion-topology` exits `0`. **Deterministic**: byte-identical stdout across
two consecutive runs (`md5sum` matched). RNG use is the 12 seeded client drift
draws (drawn once, reused by both passes) + the seeded exponential inter-packet
timing.

The scenario runs **twice inside one process** — background OFF (baseline == M2
with finite queues but no data) then background ON — printing per-node peak
offsets side by side, so the isolation is a direct within-binary comparison.

60 s run (P2d: normalized to OMNeT++'s 60 s, was 30 s), background window
[1 s, 60 s], Sync 0.125 s, `meanGap` = exponential(160 µs).

### Bottleneck (swCore→coreClient egress, cap 10) under load

```
  data offered by 3 sources : 1107019 pkts
  delivered to coreClient   : 745749 pkts (95.66 Mbps, 12640 pps)
  dropped at bottleneck     : 362847 pkts (32.73% of offered-into-queue)
  queue backlog             : mean 8.81/10, max 10/10
```

(Counts are ~2× the former 30 s run — P2d normalized the default to 60 s; the
rates, drop fraction, and backlog are unchanged. P2b's 2-step framing added one
extra gPTP frame per Sync cycle and per Pdelay exchange; against the ~150 Mbps
data flood this barely moves the drop rate, 32.64% → 32.73%.) Compare INET's own
M3 run (orientation only, **not** a match target): ~149 Mbps / ~18,759 pps at the
bottleneck, ~34% drop, mean backlog 9.23/10. Our drop rate (32.73%) and near-full
backlog (8.81/10) reproduce the same regime. (Goodput
differs because our single 946 B frame size and injection point differ from INET's
full multi-app IP path; the **drop fraction and backlog** — the congestion
descriptors that matter — line up.)

### Per-node peak offset-from-GM: baseline vs congested

```
           node | hops |   ppm |  base peak |  cong peak |  ratio
  ----------------------------------------------------------------
         swCore |  1   |  50.0 |     7.975  |     7.975  |   1.0x
     coreClient |  2   | 150.0 |    24.076  |   429.207  |  17.8x   <-- degrades
            swA |  2   |  80.0 |    12.701  |    12.701  |   1.0x
            swB |  2   | -60.0 |    10.050  |    10.050  |   1.0x
            swC |  2   | 100.0 |    15.951  |    15.951  |   1.0x
    clientsA[0] |  3   | 126.6 |    20.127  |    20.127  |   1.0x
    clientsA[1] |  3   |  42.7 |     6.495  |     6.495  |   1.0x
    clientsA[2] |  3   |  -1.8 |     1.722  |     1.722  |   1.0x
    clientsA[3] |  3   | -47.4 |     8.147  |     8.147  |   1.0x
    clientsB[0] |  3   | -52.4 |     8.961  |     8.961  |   1.0x
    clientsB[1] |  3   | 122.6 |    19.482  |    19.482  |   1.0x
    clientsB[2] |  3   |-159.6 |    26.395  |    26.395  |   1.0x
    clientsB[3] |  3   |  33.9 |     5.062  |     5.062  |   1.0x
    clientsC[0] |  3   | 175.6 |    28.090  |    28.090  |   1.0x
    clientsC[1] |  3   |  50.4 |     7.747  |     7.747  |   1.0x
    clientsC[2] |  3   | 128.8 |    20.479  |    20.479  |   1.0x
    clientsC[3] |  3   |  23.7 |     3.402  |     3.402  |   1.0x
```

**Every non-coreClient node's congested peak is byte-identical to its baseline
(ratio exactly 1.0x)** — total isolation, unchanged by P1a's servo hardening, by
P2a's rate-ratio fold, and by P2b's 2-step framing. `coreClient` alone degrades,
now by **17.8x** (429.207 µs; was 21.2x / 510.473 µs under 1-step, 2468x
pre-P1a). `coreClient`'s servo corrections under load are **153** vs its **479**
baseline (60 s run) — real Sync frames dropped/starved from the congested queue.

### P2b (2-step framing) genuinely changes this one scenario — an honest, verified finding

The Tier-2 plan predicted 2-step framing would be *informationally identical* to
1-step. **Verifying that empirically (not assuming it) showed it holds for every
scenario except this one.** Gate 2, M2, and M4 came out identical to ≤ 1 ns; but
M3's heavy-loss regime is the exception, and the reason is a real property of
2-step, not a bug (the controlled 1-step-vs-2-step A/B below was measured at the
then-default **30 s**; peaks are duration-independent so they carry over to the
current 60 s default verbatim, and the servo *ratio* does too):

- **1-step:** one Sync frame per cycle carries everything. A cycle survives if
  that single frame survives the lossy bottleneck queue. Result: 170 surviving
  corrections (30 s), congested peak **510.473 µs**.
- **2-step:** each cycle is a bare `Sync` **plus** a `Follow_Up`, and the slave
  can only use the cycle if **both** survive the queue. Under a ~33%-drop
  bottleneck the paired-survival probability is roughly the square, so surviving
  corrections roughly halve (**170 → 90** at 30 s; **153** at the 60 s default vs a
  ~1-step-equivalent ~305), and — because the *worst*-delayed Syncs are exactly
  the ones whose partner `Follow_Up` is most likely to be dropped — the surviving
  peak is **lower** (**510.473 → 429.207 µs**, unchanged at 60 s). This is
  directly visible in a P2c pcap of the bottleneck link (see below): the capture
  shows more `Pdelay_Resp`/`Sync` frames than their paired `…Follow_Up`s, the
  dropped partners in the raw trace.

This is faithful IEEE 802.1AS 2-step behavior: real hardware/`ptp4l` captures are
always 2-step, and under loss they genuinely deliver fewer usable Sync cycles than
a (non-standard) all-in-one-frame Sync would. The 1-step form was simply more
loss-robust by construction. **What is unchanged:** the M3 *finding* — the
isolation shape (all 16 other nodes still exactly 1.0x), the "only the
shared-queue node degrades" conclusion, the drop rate/backlog regime, and the
gate (coreClient clearly the one degraded node, 17.8x, still hundreds of µs, same
order as INET's 1,949.64 µs). Only the one magnitude and the servo count moved,
and both moved for a well-understood, protocol-faithful reason. Reported as data,
per this project's standard, rather than forced back to the 1-step number.

### On the magnitude (24.08 µs → 429.21 µs vs INET's 1949.64 µs) — **fixed by P1a; refined by P2b**

Pre-P1a this congested peak was **46,280.929 µs** — a 24x outlier over INET's
1,949.64 µs, the sole number the ns-3 track produced that was not trustworthy at
face value (`NS3_PARITY_PLAN.md` Tier 1). **P1a fixed it** (to **510.47 µs**
under the then-current 1-step framing), and **P2b's 2-step framing refined it to
429.21 µs** (for the loss-statistics reason in the section just above). Either way
the peak is below INET's 1,949.64 µs, the same order of magnitude, and it is the
genuine congested-queue Sync-delay signal rather than a servo/measurement
artifact. The P1a root cause and fix, precisely (numbers quoted for the 1-step
state in which P1a was developed):

1. **The old servo amplified missed Syncs (partial cause).** The Phase-2 servo
   (deadbeat phase + a fresh, unbounded `offset/elapsed` rate estimate each cycle)
   over-reacted when a Sync was sporadically dropped: the ballooned `elapsed` made
   its rate estimate briefly wild and the deadbeat phase rang on it. Hardening the
   servo to a **bounded, low-pass PI loop with missed-Sync skip** (P1a) cut the
   peak from 46,281 µs to ~20,500 µs — real, but only a ~2.3x improvement.

2. **The dominant cause was a corrupted peer delay, discovered during P1a
   verification.** With the servo loop stabilized, the residual ~20 ms peak traced
   to the measured link delay `d` inflating to **15–22 ms** — ~1000x its true
   ~2–3 µs value. Cause: the peer-delay handshake (`Pdelay_Req`/`Resp`) frames
   contend on the **saturated shared CSMA medium** (the S5 shared-medium artifact
   reaching the peer-delay path), so `t4 − t1` absorbs tens of ms of CSMA-backoff
   delay and `d = ((t4−t1) − (t3−t2))/2` reads garbage. A corrupted `d` feeds
   straight into `recvLocal − (originTs + upstreamCorr + d)` and injects a false
   offset that *any* servo, however well-tuned, faithfully chases. A **peer-delay
   outlier filter** (a running-minimum estimator — the true link delay is a
   physical floor, so contention can only ever *add*, and the ~20 clean
   pre-congestion Pdelay exchanges establish the floor) rejects the inflated
   samples. That takes the peak from ~20,500 µs to **510.47 µs**.

3. **What the peak *is*.** It is the real extra queueing delay a
   `coreClient`-bound Sync experiences waiting behind a near-full 10-packet
   bottleneck queue (10 × ~76 µs ≈ 760 µs, minus the fraction of Syncs dropped
   outright). That is exactly the M3 mechanism — the shared-queue coupling — now
   measured cleanly instead of being buried under servo ringing and peer-delay
   garbage. Under P2b's 2-step framing the surviving peak is **429.21 µs** (17.8x
   its own baseline; it was 510.47 µs / 21.2x under 1-step — the paired-frame
   survival effect above); clearly the one degraded node either way, in the same
   ~hundreds-of-µs-to-low-ms regime as INET's 1,949.64 µs.

The isolation finding is untouched (all 16 other nodes still exactly 1.0x). Both
P1a changes live in the vendored `gptp.{h,cc}` and are documented in `gptp.h`'s
SERVO header block and the peer-delay filter comment; both are clean-room (public
linuxptp PI idea + a first-principles floor estimator, no ptp4l/INET source read).

## Gate checks (all PASS)

```
  [PASS] baseline (no traffic): every node still converges (|final| < 2 us)
  [PASS] congestion is real: bottleneck queue actually drops packets
  [PASS] coreClient sync degrades under load (17.8x its baseline, 429.207 us)
  [PASS] every OTHER node stays within 5 us of its baseline (isolation)
```

## pcap capture (P2c) — opt-in, off by default

`--pcapPrefix=<prefix>` enables `CsmaHelper::EnablePcapAll` on every CSMA device
(the congested pass only), reusing Phase 0's proven capture mechanism — one
`<prefix>-<node>-<dev>.pcap` per device, capturing **both** the gPTP frames and
the background data flood on the shared bottleneck link. It is **off by default
and additive**: with no `--pcapPrefix`, no files are written and stdout / every
gate is byte-identical (verified — enabling capture leaves the printed numbers
bit-for-bit unchanged, since it only attaches trace sinks).

Verify a capture is genuine (not just "a file exists") with
`ns3/scripts/check_pcap_gptp.py`, a dependency-free parser for this project's own
19-byte `GptpHeader` format (**not** Wireshark-dissectable 802.1AS — that needs
the IEEE TLV wire format, Tier 3). On the bottleneck link `…-1-1.pcap` it reads,
e.g., 12 758 total frames, 118 gPTP, with the honest P2b fingerprint visible in
the raw trace — **more `Pdelay_Resp`/`Sync` than their paired `…Follow_Up`s** (31
vs 24, 13 vs 10), the partner frames the congested queue dropped:

```
$ python3 ns3/scripts/check_pcap_gptp.py <prefix>-1-1.pcap
  gPTP frames (ethertype 0x88b6): 118
    type 0 PdelayReq            : 40
    type 1 PdelayResp           : 31
    type 2 Sync                 : 13
    type 3 PdelayRespFollowUp   : 24
    type 4 FollowUp             : 10
  PASS: genuine, parseable gPTP capture with 2-step framing
```

## What this does and does not establish

- **Does:** finite real-dropping queues (native ns-3, Phase 0's mechanism)
  reproduce the M3 congestion regime (~33% drop, near-full backlog) on the
  Nominal topology; and gPTP↔congestion coupling is **localized to the node
  sharing the congested egress queue** (`coreClient`), leaving all 16 other nodes
  at their exact M2 baseline. The core M3 finding reproduces.
- **Does not (deferred / simplified):** hop-by-hop L2 forwarding of the data
  (S5), full adaptive ptp4l-grade servo behavior (P1a hardens the loop to a
  bounded PI with missed-Sync skip + a peer-delay outlier filter, but not a full
  spike-rejection state machine), **IEEE-TLV-dissectable** pcap (own-format pcap
  capture IS available now — P2c, above; the IEEE TLV wire format is Tier 3).
  Carries S1/S4 forward unchanged; **S2 (2-step framing) closed by P2b** (the one
  Tier-2 item that moved M3's numbers — peak 510.473 → 429.207 µs, servo 170 → 90
  at 30 s / 153 at 60 s, see the Simplifications section) and
  **S3 (`neighborRateRatio`) closed by P2a** (≤ 2 ns
  effect here); the P1a servo/peer-delay hardening stands.

### Honest licensing note

Same as every prior phase: the Apache-2.0 SPDX header covers our files' copyright
only. Because this links against ns-3 core (GPL-2.0-only), the **combined,
distributed binary is still GPLv2**.

### Not yet confirmed in real CI

Same caveat as Gates 0/1/2 / M2: no Docker daemon in this sandbox, so these
numbers are from a local ns-3.45 build. The `COPY ns3/congestion
"$NS3_ROOT/scratch/syncsim-congestion"` line (added this phase) picks these files
up automatically; verifying the containerized build is the standing step.

## Reproduce locally (no Docker)

```bash
git clone --branch ns-3.45 --depth 1 https://gitlab.com/nsnam/ns-3-dev.git /tmp/ns-3-dev
cp -r ns3/congestion /tmp/ns-3-dev/scratch/syncsim-congestion
cd /tmp/ns-3-dev
./ns3 configure --build-profile=release \
    --enable-modules=core,network,csma,bridge,point-to-point,applications,internet,flow-monitor \
    --disable-examples --disable-tests --disable-python \
    --enable-asserts --enable-logs
./ns3 build congestion-topology
./build/scratch/syncsim-congestion/ns3.45-congestion-topology   # exit 0 == GATE PASS
```
