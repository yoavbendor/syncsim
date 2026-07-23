# ns3/feedback/ — Phase 3 (M4): clock-aligned burst traffic

Clean-room, permissively-licensed (Apache-2.0) proof that syncsim's M4
("feedback") scenario reproduces on ns-3, built on the Phase-1 `syncsim::Clock`
and the Phase-2/M2 gPTP mechanism (hardened in **P1a** — bounded PI servo +
missed-Sync handling + peer-delay outlier filter). Reproduces
`simulations/feedback.ini`.

## What M4 is — and what its gate is (read this first)

M4 reuses M3's finite-queue mechanism but with `packetCapacity = 20` and replaces
M3's three independent-phase senders with periodic "frame" bursts from **all 12
zone clients, each scheduled on its OWN local (gPTP-steered) clock**. If gPTP has
synced the clients well, their clocks agree on "now", so their bursts land at the
same *simulated* instant — the collision **emerges from sync quality**, it is not
hand-scripted.

**M4's gate is NOT "prove coupling exists."** `feedback.ini`'s own header says so:
INET's real run found real congestion (41–45.8% drop, near-full queue) but gPTP
offsets came out **bit-for-bit identical to the no-traffic baseline** — no
measurable sync↔congestion coupling, because Sync fires ~every 0.125 s and tends
to land in the quiet gaps between 100 ms burst cycles. So the standard here is:
implement the mechanism faithfully and **report whatever the run actually shows,
honestly**. A faithful non-finding is as valid as a finding.

**Our result (updated by P3a — the S5 real-forwarding fix): a clean full
non-finding. With the data plane genuinely L2-forwarded hop-by-hop over
full-duplex links (S5 closed), NO node shows any measurable coupling —
`coreClient`'s delta is now exactly `0.000 µs` (it was `0.695 µs` under the old
CSMA egress-injection transport), and all 16 other nodes are 0.000 too. Despite
47.6% queue drop, every node's steady-window peak offset is bit-identical between
the no-traffic baseline and the aligned-burst pass — matching INET's original M4
non-finding (bit-for-bit-identical offsets) even more closely than before. The
faint 0.695 µs coreClient delta that P1a had surfaced was partly a residue of the
CSMA egress-injection transport; the cleaner mechanism removes it. Reported
honestly, not forced (below).**

## The genuinely new mechanism — clock-driven scheduling (S6)

ns-3's `Simulator::Schedule` takes a delta from *global* now; it has **no** native
"fire at this local-clock instant, re-anchor if the clock is later adjusted"
primitive — exactly what OMNeT++'s `clockModule` + `scheduleForAbsoluteTime` give
INET's `ActivePacketSource` for free. **S6 (stated, not claimed as perfect
parity):** after each burst, client *c* computes its next intended **local** send
time (`targetLocal = burstStart + k·100 ms`, an absolute local-clock instant),
reads its clock's **live** local time and **live** rate, and converts to a global
`Schedule` delta:

```
globalDelta = (targetLocal − currentLocal) / currentRate
```

Because this recomputes fresh from the clock's current rate + offset after **every
burst**, it tracks ongoing servo corrections at burst granularity — the same
0.125 s cadence gPTP already updates the clock at. It does **not** retroactively
re-fire an in-flight event the instant a mid-interval servo step lands (unlike
OMNeT++'s literal `clockEvent` re-anchoring), so alignment is anchored to each
client's clock as of its last scheduling decision, not continuously. For 100 ms
cycles vs 0.125 s servo updates and few-ppm residual drift, the residual anchoring
error is sub-microsecond — far finer than the alignment spread we measure. This is
the honest analog of `scheduleForAbsoluteTime`, not a claim of bit-parity.

**It works: the 12 clients' bursts land within a mean 0.579 µs of each other**
(see below) — driven purely by how well gPTP synced their clocks, exactly the
emergent alignment M4 is about.

## Simplifications

- **S6** — clock-driven scheduling analog (above), new this phase.
- **S5** — **CLOSED by P3a** (same fix as M3, see `congestion/README.md`). Each
  client's burst now **genuinely originates at that client and is L2-forwarded
  hop-by-hop** (`clientsX[i]` → `swX` → `swCore` → `coreClient`) over full-duplex
  `SimpleNetDevice`/`SimpleChannel` links, using **that client's own clock for the
  timing**, so the emergent alignment still reflects pure gPTP sync quality. A
  structural consequence, reported honestly: all 4 clients in a zone now forward
  their aligned bursts through their **shared zone-switch uplink** (cap 20) *before*
  the bursts reach `swCore`'s bottleneck (cap 20), so the microburst overflows the
  zone-switch queue too, not only the final bottleneck — which changes the
  drop/delivery counts (below). The **coupling question** is unaffected: only
  `coreClient` shares an egress queue with data flowing in the *same* direction as
  its Sync (`swCore→coreClient`), so it is the only node that *can* couple; on every
  zone link burst data flows client→switch while Sync flows switch→client, and
  full-duplex keeps those independent.
- **Packetization** (stated per the brief): INET's `20000B − 54B` frame fragments
  into ~15 IP packets. We send each burst as **15 back-to-back frames of ~1330 B**,
  so 12 aligned clients present ~180 frames at one instant — the microburst regime
  `feedback.ini` describes. With the S5 real forwarding these now traverse two
  finite-queue stages: each zone switch's uplink (4×15 = 60 frames vs cap-20) then
  `swCore`'s bottleneck (survivors of all 3 zones vs cap-20).
- **S1** from Phase 2 carried forward unchanged; **S4** is deliberate. **S3
  (`neighborRateRatio`) closed by P2a**, **S2 (2-step framing) closed by P2b**,
  **S5 (hop-by-hop forwarding) closed by P3a**. With the P3a transport, M4's coupling
  is now a clean **0.000 µs** at every node (it was `0.695 µs` at `coreClient` under
  CSMA egress-injection) — so the driver prints **"NO MEASURABLE COUPLING"**.

## Why gptp/clock are vendored here

Same constraint every prior phase hit (ns-3 scratch can't share a `.cc` across
sibling subdirs; the Dockerfile is one `COPY ns3/feedback` line). `clock.{h,cc}`
and `gptp.{h,cc}` are **byte-identical** vendored copies (md5sum-confirmed against
`ns3/gptp/`/`ns3/clock/`). Only `feedback-topology.cc` is new.

## Files

| File | Role | License |
|---|---|---|
| `feedback-topology.cc` | M4 proof scenario (`main`) — baseline vs clock-aligned bursts | Apache-2.0 (ours) |
| `gptp.h` / `gptp.cc` | **Vendored byte-identical** from `ns3/gptp/` (P1a-hardened servo) | Apache-2.0 (ours) |
| `clock.h` / `clock.cc` | **Vendored byte-identical** from `ns3/clock/` | Apache-2.0 (ours) |

Builds as target **`feedback-topology`** →
`build/scratch/syncsim-feedback/ns3.45-feedback-topology`.

## Result — **GATE PASS** (sandbox, ns-3.45, release build, asserts on)

`feedback-topology` exits `0`. **Deterministic**: byte-identical stdout across two
runs (`md5sum` matched). The only RNG use is the 12 seeded client drift draws
(bursts are clock-driven, not random). 60 s run (P2d: was 30 s), bursts from local 1.0 s every
100 ms, 15 frags/burst, Sync 0.125 s. Runs **twice in one process** — baseline (no
bursts) vs bursts.

### Burst alignment (the emergent quantity — S6 working)

```
  full cycles measured : 590
  mean fire-time spread: 0.579 us   (12 clients agree on "now" to ~1 us => collide)
  max  fire-time spread: 335.256 us
```

The 12 clients, each scheduling on its own gPTP-steered clock, fire within ~1 µs
of each other on average — **the alignment is emergent from sync quality, not
scripted.** (The 335 µs max is an early cycle before full frequency-lock; it
settles to ~µs.)

### Bottleneck (swCore→coreClient egress, cap 20) under aligned bursts

```
  frames offered  : 106200 (15 frags x 12 clients x cycles)
  delivered       : 32452
  dropped         : 29502 (47.62% of offered-into-queue) -- at swCore.eth1; drops
                    also occur at each zone-switch uplink (two-stage forwarding)
  queue backlog   : mean 0.87/20, max 20/20
```

Congestion is **real**: aligned microbursts overflow the 20-slot queue (hits
20/20). **Before/after the P3a real fix:** delivered `12,393 → 32,452`, bottleneck
drop `88.37% → 47.62%`, mean backlog `0.33 → 0.87`. The drop rate at the final
bottleneck fell because the burst now traverses **two** finite-queue stages — each
zone-switch uplink already drops part of its 60-frame microburst before the
survivors reach `swCore`, so `swCore`'s bottleneck sees a thinner, time-spread
arrival (and `SimpleNetDevice`'s lack of CSMA inter-frame gap lets it drain
faster). This two-stage drop is the honest structural consequence of real
hop-by-hop forwarding; INET's ~45% single-stage drop is orientation only, not a
match target. What is unchanged: the queue genuinely, repeatedly overflows.

### The coupling question — per-node steady-window peak, baseline vs bursts

Compared over the **steady (burst) window** (`t ≥ 1.0 s`), so the large pre-burst
first-Sync convergence transient (~24 µs, identical in both passes, and it would
swamp any small burst effect in a global-max comparison) cannot mask coupling. This
is the honest coupling measure:

```
           node | hops |   ppm |  base peak |  burst peak |  delta us
  --------------------------------------------------------------------
         swCore |  1   |  50.0 |    0.266   |    0.266    |   0.000
     coreClient |  2   | 150.0 |    0.915   |    0.915    |   0.000   <-- now zero too
            swA |  2   |  80.0 |    0.427   |    0.427    |   0.000
            swB |  2   | -60.0 |    0.320   |    0.320    |   0.000
            swC |  2   | 100.0 |    0.543   |    0.543    |   0.000
    clientsA[0] |  3   | 126.6 |    0.756   |    0.756    |   0.000
    clientsA[1] |  3   |  42.7 |    0.228   |    0.228    |   0.000
    clientsA[2] |  3   |  -1.8 |    0.010   |    0.010    |   0.000
    clientsA[3] |  3   | -47.4 |    0.252   |    0.252    |   0.000
    clientsB[0] |  3   | -52.4 |    0.278   |    0.278    |   0.000
    clientsB[1] |  3   | 122.6 |    0.725   |    0.725    |   0.000
    clientsB[2] |  3   |-159.6 |    0.981   |    0.981    |   0.000
    clientsB[3] |  3   |  33.9 |    0.181   |    0.181    |   0.000
    clientsC[0] |  3   | 175.6 |    1.007   |    1.007    |   0.000
    clientsC[1] |  3   |  50.4 |    0.269   |    0.269    |   0.000
    clientsC[2] |  3   | 128.8 |    0.770   |    0.770    |   0.000
    clientsC[3] |  3   |  23.7 |    0.127   |    0.127    |   0.000
```

**Every node's steady-window burst peak is byte-identical to its no-traffic
baseline (delta exactly 0.000, all 17 nodes)** — a complete non-finding, now
including `coreClient` (0.695 → 0.000 vs the CSMA transport). Baseline peaks shifted
by ≤ 1 ns from the CSMA numbers (the `SimpleNetDevice` PHY convention); all sub-µs,
all converging to exactly 0.000 final.

## The finding — a clean full non-finding (updated by P3a)

**No measurable coupling at any node.** With the S5 real hop-by-hop forwarding over
full-duplex links, `coreClient` — the one node whose gPTP path shares the congested
egress queue — now shows a steady-window delta of **exactly 0.000 µs** (`0.915 →
0.915 µs`), and so do **all 16 other nodes**. The driver prints **"NO MEASURABLE
COUPLING"**. This reproduces INET's original M4 result — offsets bit-for-bit
identical between the no-traffic baseline and the aligned-burst pass — even more
closely than the CSMA transport did.

**Where did the P1a-surfaced 0.695 µs go?** Under the old CSMA egress-injection
transport, `coreClient` showed a `0.695 µs` steady-window delta (the only non-zero
node). That was partly a residue of injecting the aggregate burst directly into
`swCore`'s shared CSMA medium right where `coreClient`'s Sync competed. With the
cleaner full-duplex mechanism — data forwarded and arriving time-spread, tx/rx
independent — the effect drops below measurability (0.000). This is reported
honestly as data, not tuned: the parameters are unchanged from P1a/P2a/P2b; only the
transport changed (S5 fix).

**What it means:** the physical conclusion matches INET — aligned microbursts do
**not** meaningfully degrade sync, at *any* node. M3 and M4 remain the *same
mechanism* at two operating points: M3's tiny cap-10 queue + sustained
oversubscription drives `coreClient` to a ~551 µs congested peak; M4's cap-20 + brief
aligned microbursts leave every node, `coreClient` included, at exactly its baseline.
The gate (faithful mechanism + honest reporting) is unchanged and still PASSES.

### Gate checks (all PASS — faithful mechanism, honest reporting)

```
  [PASS] baseline (no bursts): every node converges (|final| < 2 us)
  [PASS] bursts genuinely aligned by gPTP sync (mean spread 0.579 us < 50 us)
  [PASS] congestion is real: aligned microbursts overflow the finite queue
```

The gate is **faithfulness** — real per-client-local-clock bursts (S6), real queue
congestion, baseline still converging — **not** "coupling must exist." The coupling
result is reported as data, per `feedback.ini`'s own standard.

## What this does and does not establish

- **Does:** ns-3 can schedule data traffic on each node's own gPTP-steered local
  clock (S6, the one genuinely new Phase-3 primitive), well enough that 12
  independent clients' bursts align to ~1 µs purely from sync; that traffic is now
  **genuinely L2-forwarded hop-by-hop** (S5 closed) over full-duplex links; and under
  that real, aligned, queue-overflowing microburst load, gPTP sync is **not**
  measurably degraded at any node — a faithful reproduction of INET's M4 non-finding
  (bit-identical offsets), now with zero coupling everywhere.
- **Does not (deferred / simplified):** perfect `scheduleForAbsoluteTime`
  re-anchoring (S6), **pcap capture** (regressed by the S5 transport swap —
  `SimpleNetDeviceHelper` has no `EnablePcap`; `--pcapPrefix` is now a warned no-op,
  a known follow-up gap, same as `congestion/README.md`), and **IEEE-TLV** wire
  format (Tier 3). Carries S1 forward unchanged and S4 is deliberate; **S2 closed by
  P2b**, **S3 closed by P2a**, **S5 (hop-by-hop forwarding) closed by P3a**.

### Honest licensing note

Same as every prior phase: the Apache-2.0 SPDX header covers our files' copyright
only. Because this links against ns-3 core (GPL-2.0-only), the **combined,
distributed binary is still GPLv2**.

### Not yet confirmed in real CI

Same Docker-daemon caveat as Gates 0/1/2 / M2 / M3. The `COPY ns3/feedback
"$NS3_ROOT/scratch/syncsim-feedback"` line (added with M3) picks these files up
automatically; verifying the containerized build is the standing step.

## Reproduce locally (no Docker)

```bash
git clone --branch ns-3.45 --depth 1 https://gitlab.com/nsnam/ns-3-dev.git /tmp/ns-3-dev
cp -r ns3/feedback /tmp/ns-3-dev/scratch/syncsim-feedback
cd /tmp/ns-3-dev
./ns3 configure --build-profile=release \
    --enable-modules=core,network,csma,bridge,point-to-point,applications,internet,flow-monitor \
    --disable-examples --disable-tests --disable-python \
    --enable-asserts --enable-logs
./ns3 build feedback-topology
./build/scratch/syncsim-feedback/ns3.45-feedback-topology   # exit 0 == GATE PASS
```
