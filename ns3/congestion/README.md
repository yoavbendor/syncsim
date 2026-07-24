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
10`). Background data traffic (three ~50 Mbps flows) **originates at
`clientsA/B/C[0]` and is L2-forwarded hop-by-hop** (P3a — see S5 below) up to
`coreClient`, aggregating onto the single `swCore↔coreClient` 100 Mbps link — a
guaranteed ~150-into-100 Mbps oversubscription. gPTP Sync frames toward `coreClient`
share that exact congested egress queue, so `coreClient`'s sync degrades; no other
node's gPTP path touches that queue, so every other node is untouched.

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

gPTP frames and forwarded data frames share the **same per-device egress queue** at
the bottleneck: both are emitted via `NetDevice::Send()` on `swCore`'s
`coreClient`-facing `SimpleNetDevice`, which owns one finite `DropTailQueue`. Three
~50 Mbps flows, forwarded hop-by-hop from `clientsA/B/C[0]`, converge on that one
100 Mbps egress, so its queue runs full and drops ~1/3 of everything in it. The
gPTP Sync/Pdelay frames `swCore` regenerates toward `coreClient` must sit in — or be
dropped from — that same congested queue, so `coreClient`'s Sync arrives late (extra
unmodeled queueing delay folds into `recvLocal − reconstructedGmTime`) or not at all
(a missed servo cycle). Every other egress carries at most one un-oversubscribed
forwarded flow plus negligible gPTP, never fills, and those nodes are untouched —
**and, because the links are now full-duplex, the forward transit data on a zone
link no longer delays the reverse-direction gPTP Sync on that same link** (the CSMA
shared-medium artifact S5 used to dodge by injecting at the egress). **The asymmetry
emerges from the shared-queue mechanism over genuine hop-by-hop forwarding — it is
not hand-coded per node.**

## Simplifications (S1 carried forward; S2 closed by P2b, S3 by P2a, **S5 closed by P3a**; S4 is deliberate)

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

**S5 — CLOSED (Tier 3 / P3a real fix).** The three background flows now
**genuinely originate at `clientsA/B/C[0]` and are L2-forwarded hop-by-hop** through
`swA/B/C` → `swCore` → `coreClient`, exactly as INET's full-duplex Ethernet
forwards them — no longer injected directly at `swCore`'s convergence egress.

The blocker was that mainline ns-3.45's `CsmaChannel` is a single **shared
CSMA/CD medium**: on it, transit data on a zone link spuriously delays the
**reverse-direction** gPTP Sync on that same medium, which (confirmed empirically
by a first forwarding attempt) coupled **every** node's sync to the load and
destroyed the isolation the finding is about. The P3a spike found the fix: swap
the link transport to ns-3.45's **`SimpleNetDevice`/`SimpleChannel`** (mainline, no
external module, no core patch). Each `SimpleNetDevice` owns its own tx queue and
tx-busy state and `SimpleChannel` does no carrier sense/collision, so with exactly
two devices per channel each link is a **genuine full-duplex point-to-point link**:
forward transit data no longer contends with reverse-direction gPTP on the same
link. Data forwarding is a **static, hand-coded L2 table** in `CombinedRx` (the
topology is a fixed tree — each zone switch forwards data out its `swCore` uplink,
`swCore` forwards out its `coreClient` egress); gPTP frames keep their per-port
termination (**S4**, untouched) via the same dispatch, and we deliberately do **not**
use a `BridgeNetDevice` (which would transparently forward and clash with S4). The
oversubscription, the real drops, and the shared-queue coupling still land on the
`swCore→coreClient` egress — but now as the **emergent result of real hop-by-hop
forwarding**, visible in `analyze.py`'s data-plane summary as ~46.5 Mbps on each
zone-switch uplink (zero drops) aggregating to ~139.5 Mbps at `swCore.eth1`
(~29.6% drop). Zone links carry data one way and gPTP the other, independently, so
every non-`coreClient` node stays exactly at baseline — INET's result, now for the
right structural reason. `gptp.{h,cc}` needed **no** change (it is transport-agnostic,
talking to `NetDevice::Send`/`SetReceiveCallback`), verified byte-identical across
all four vendored copies. See `ns3/spikes/P3A_SPIKE_FINDINGS.md`.

**pcap restored:** `SimpleNetDeviceHelper` has no `EnablePcap`/`EnablePcapAll`
convenience wrapper (`SimpleNetDevice` exposes no `PromiscSniffer`/`Sniffer`
trace source for a helper to auto-hook), but ns-3's own low-level pcap
primitives — `PcapHelper::CreateFile` + `PcapFileWrapper::Write`, the same
machinery `CsmaHelper::EnablePcapInternal` calls internally — are public API
and work fine called by hand. `--pcapPrefix` capture is fully working again;
see the "pcap" section below.

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
  data offered by 3 sources : 1106326 pkts
  delivered to coreClient   : 779488 pkts (99.99 Mbps, 13212 pps)
  dropped at bottleneck     : 328674 pkts (29.66% of offered-into-queue)
  queue backlog             : mean 8.88/10, max 10/10
```

**Before/after the P3a real fix** (both at the 60 s default, 2-step framing):

| metric | CSMA, injected at egress | **SimpleNetDevice, real forwarding** |
|---|---|---|
| delivered | 745,749 (95.66 Mbps) | **779,488 (99.99 Mbps)** |
| dropped | 362,847 (32.73%) | **328,674 (29.66%)** |
| mean backlog | 8.81/10 | **8.88/10** |
| `coreClient` cong peak | 429.207 µs (17.8×) | **550.854 µs (22.6×)** |
| `coreClient` servos (cong) | 153 | **125** |

The drop fraction eased slightly (32.73% → 29.66%) and goodput rose to a full
99.99 Mbps because `SimpleNetDevice` has no CSMA inter-frame gap / framing
overhead, so the 100 Mbps egress carries slightly more payload before saturating.
The `coreClient` congested peak **moved up** (429 → 551 µs) — reported honestly, not
forced back: the peak is the queueing delay a `coreClient`-bound Sync sees behind a
near-full 10-packet bottleneck (≈10 × ~76 µs ≈ 760 µs ceiling, minus dropped
Syncs), and the cleaner full-duplex mechanism plus real forwarded arrival timing
lands it at 551 µs. It is still well below INET's 1,949.64 µs, same order of
magnitude, still clearly the one degraded node. Compare INET's own M3 run
(orientation only, **not** a match target): ~149 Mbps / ~18,759 pps, ~34% drop,
mean backlog 9.23/10 — our 29.66% drop and 8.88/10 backlog reproduce the same
regime.

### Per-node peak offset-from-GM: baseline vs congested

```
           node | hops |   ppm |  base peak |  cong peak |  ratio
  ----------------------------------------------------------------
         swCore |  1   |  50.0 |     8.125  |     8.125  |   1.0x
     coreClient |  2   | 150.0 |    24.375  |   550.854  |  22.6x   <-- degrades
            swA |  2   |  80.0 |    13.000  |    13.000  |   1.0x
            swB |  2   | -60.0 |     9.750  |     9.750  |   1.0x
            swC |  2   | 100.0 |    16.250  |    16.250  |   1.0x
    clientsA[0] |  3   | 126.6 |    20.576  |    20.576  |   1.0x
    clientsA[1] |  3   |  42.7 |     6.945  |     6.945  |   1.0x
    clientsA[2] |  3   |  -1.8 |     0.289  |     0.289  |   1.0x
    clientsA[3] |  3   | -47.4 |     7.696  |     7.696  |   1.0x
    clientsB[0] |  3   | -52.4 |     8.510  |     8.510  |   1.0x
    clientsB[1] |  3   | 122.6 |    19.931  |    19.931  |   1.0x
    clientsB[2] |  3   |-159.6 |    25.943  |    25.943  |   1.0x
    clientsB[3] |  3   |  33.9 |     5.512  |     5.512  |   1.0x
    clientsC[0] |  3   | 175.6 |    28.540  |    28.540  |   1.0x
    clientsC[1] |  3   |  50.4 |     8.197  |     8.197  |   1.0x
    clientsC[2] |  3   | 128.8 |    20.928  |    20.928  |   1.0x
    clientsC[3] |  3   |  23.7 |     3.852  |     3.852  |   1.0x
```

**Every non-coreClient node's congested peak is byte-identical to its baseline
(ratio exactly 1.0x)** — total isolation, now demonstrated over **genuine
hop-by-hop forwarding** (the S5 fix) rather than egress injection, and unchanged
by P1a's servo hardening, P2a's rate-ratio fold, and P2b's 2-step framing.
`coreClient` alone degrades, now by **22.6x** (550.854 µs). The baseline peaks
shifted by ≤ 0.4 µs from the CSMA numbers (e.g. `coreClient` 24.076 → 24.375 µs,
`clientsA[2]` 1.722 → 0.289 µs — the lowest-drift node is the most sensitive to the
transport's slightly different serialization/timing), which is expected from the
`SimpleNetDevice` PHY convention; the isolation **shape** is exact. `coreClient`'s
servo corrections under load are **125** vs its **479** baseline — real Sync frames
dropped/starved from the congested queue.

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
   garbage. Under P2b's 2-step framing on the old CSMA egress-injection transport
   the surviving peak was **429.21 µs** (17.8x; 510.47 µs / 21.2x under 1-step);
   under the **P3a real hop-by-hop forwarding over full-duplex `SimpleNetDevice`**
   it is **550.85 µs** (22.6x) — reported as data, not forced back. Clearly the one
   degraded node in every case, in the same ~hundreds-of-µs regime as INET's
   1,949.64 µs.

The isolation finding is untouched (all 16 other nodes still exactly 1.0x). Both
P1a changes live in the vendored `gptp.{h,cc}` and are documented in `gptp.h`'s
SERVO header block and the peer-delay filter comment; both are clean-room (public
linuxptp PI idea + a first-principles floor estimator, no ptp4l/INET source read).

## Gate checks (all PASS)

```
  [PASS] baseline (no traffic): every node still converges (|final| < 2 us)
  [PASS] congestion is real: bottleneck queue actually drops packets
  [PASS] coreClient sync degrades under load (22.6x its baseline, 550.854 us)
  [PASS] every OTHER node stays within 5 us of its baseline (isolation)
```

## pcap capture (P2c) — **restored under the S5 transport**

`CsmaHelper` is a `PcapHelperForDevice` (it has `EnablePcapAll`); ns-3.45's
`SimpleNetDeviceHelper` **is not** — it has no `EnablePcap`/`EnablePcapAll`
convenience wrapper, and `SimpleNetDevice` itself exposes no
`PromiscSniffer`/`Sniffer` trace source for a helper to auto-hook (both
verified directly against the pinned tree). But the convenience wrapper isn't
the only way in: ns-3's own low-level pcap primitives —
`PcapHelper::CreateFile()` + `PcapFileWrapper::Write()` — are public API, the
exact same calls `CsmaHelper::EnablePcapInternal` makes internally. `link()`'s
`PcapCapture` hook calls them by hand, once per device, from inside
`CombinedRx` — the single place every frame this topology carries (gPTP *and*
data) already passes through, so no `gptp.{h,cc}` change is needed. Capturing
on RX only (rather than TX+RX like CSMA's trace) is content-equivalent on this
point-to-point topology: every frame sent by one device is received by exactly
one other, so it's captured exactly once, just filed under the receiving
device's `.pcap` instead of duplicated under both link ends. Since
`SimpleNetDevice` doesn't carry a real Ethernet header on the wire (addressing
is out-of-band via a `SimpleTag`), the hook synthesizes one (14-byte dst=gPTP
multicast for gPTP / src / ethertype) so the capture is genuinely classic-Ethernet,
DLT_EN10MB — dissectable by tshark's PTPv2 dissector and by
`ns3/scripts/check_pcap_gptp.py`.

Verified (P3c): a 3s `--pcapPrefix` run wrote 34 non-empty per-device `.pcap`
files; **tshark's own unmodified PTPv2 dissector parses every frame with zero
malformed** across all 34, and `check_pcap_gptp.py` reports genuine, parseable
gPTP traffic with all six 802.1AS message types present (Sync, Pdelay_Req,
Pdelay_Resp, Follow_Up, Pdelay_Resp_Follow_Up, Announce), exit 0 PASS. With
`--pcapPrefix` unset, stdout stays byte-identical and zero files are written
(verified, gate unaffected).

## What this does and does not establish

- **Does:** finite real-dropping queues (native ns-3, Phase 0's mechanism)
  reproduce the M3 congestion regime (~33% drop, near-full backlog) on the
  Nominal topology; and gPTP↔congestion coupling is **localized to the node
  sharing the congested egress queue** (`coreClient`), leaving all 16 other nodes
  at their exact M2 baseline. The core M3 finding reproduces.
- **Also does (new — P3a):** the data plane is now **genuinely L2-forwarded
  hop-by-hop** (`clientsX[0]` → `swX` → `swCore` → `coreClient`) over full-duplex
  `SimpleNetDevice`/`SimpleChannel` links — **S5 closed** — and the isolation
  finding survives that (all 16 other nodes still exactly 1.0x), demonstrated over
  real forwarding rather than egress injection.
- **Also does (new — P3c):** the captured wire format is now **byte-exact IEEE
  802.1AS** on the real PTP EtherType 0x88F7 to the reserved gPTP multicast —
  **genuinely Wireshark/tshark-dissectable**, verified zero-malformed (**S6-adjacent
  wire-format gap closed**). Honest consequence: the real frames are larger than
  the old 19-byte header, so per S1 the measured peer delay grows and the congested
  peak shifted `550.854 → 513.430 µs` (22.6× → 21.5×) with congested servo count
  `125 → 114` — a frame-size effect on the loss-sensitive path (isolation shape
  still exact, drop 29.66% unchanged, gate PASS). See `ns3/gptp/WIRE_FORMAT.md`.
- **Does not (deferred / simplified):** full adaptive ptp4l-grade servo behavior
  (P1a hardens the loop to a bounded PI with missed-Sync skip + a peer-delay
  outlier filter, but not a full spike-rejection state machine).
  Carries S1 forward unchanged and S4 is deliberate; **S2 (2-step framing) closed
  by P2b**, **S3 (`neighborRateRatio`) closed by P2a**, **S5 (hop-by-hop
  forwarding) closed by P3a**, **the real 802.1AS wire format closed by P3c**;
  the P1a servo/peer-delay hardening stands.

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
