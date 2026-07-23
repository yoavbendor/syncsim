# P3a feasibility spike — full-duplex L2 transport for gPTP's 0x88b6 (closes S5?)

**Scope: a time-boxed feasibility spike, per `NS3_PARITY_PLAN.md` Tier 3 / P3a.**
Nothing here is the S5 fix itself. This answers exactly one go/no-go question
with a built-and-run standalone program (`p3a-fullduplex-spike.cc`), touching
**none** of the committed scenario files (`congestion-topology.cc`,
`gptp.{h,cc}`, `clock.{h,cc}`). It does not decide whether to proceed — that is
the calling session's call after it re-verifies these numbers independently.

## The question

> Can gPTP's custom ethertype (`0x88b6`) be carried over *some* full-duplex-capable
> ns-3 device — WITHOUT (a) patching ns-3's pinned `CsmaChannel`/`CsmaNetDevice`
> core or (b) writing a new `NetDevice` subclass from scratch — i.e. is there an
> existing device/configuration that gives real point-to-point full-duplex
> semantics (each direction independent, no cross-direction contention) while
> still passing arbitrary ethertypes including custom ones?

## Answer

**YES.** `ns3::SimpleNetDevice` + `ns3::SimpleChannel` (both mainline ns-3.45,
`src/network/utils/`, no external module) carry `0x88b6` full-duplex with each
direction fully independent, and plug into a stock `BridgeNetDevice` for a
multi-hop forwarding chain — with zero patches to ns-3 core and no new
`NetDevice` subclass.

## Why it works (source-level, before the empirical proof)

Read from the pinned ns-3.45 tree:

- **Arbitrary ethertype, no filter.** `SimpleNetDevice::SendFrom`
  (`simple-net-device.cc`) stashes the caller's `protocolNumber` in a `SimpleTag`
  and hands it verbatim to the receiver's callback
  (`m_rxCallback(this, packet, protocol, from)`). There is **no** ethertype
  validation anywhere — unlike `PointToPointNetDevice`, whose PPP framer maps the
  ethertype through `PppToEther`/`EtherToPpp` and rejects anything it doesn't know
  (the documented reason P2P was rejected for gPTP). `0x88b6` rides through
  untouched.
- **Genuine full-duplex, no shared medium.** `SimpleChannel::Send`
  (`simple-channel.cc`) simply `Simulator::ScheduleWithContext`s a `Receive` on
  every *other* device after a fixed `Delay`. There is **no** carrier sense, no
  medium-busy state, no collision/backoff — the source comment says so outright
  ("SimpleChannel doesn't have a locking mechanism, and doesn't check for
  collisions"). Each `SimpleNetDevice` owns its **own** tx queue and its own
  `FinishTransmissionEvent` tx-busy flag, so a device's transmit path being busy
  never blocks the *other* device's transmit path, and never blocks either
  device's receive path. With exactly two devices on one channel this is a clean
  full-duplex point-to-point link. (Contrast `CsmaChannel`, which has a single
  `IDLE/TRANSMITTING/PROPAGATING` state shared by all attached devices — one
  transmitter at a time — which is the S5 root cause.)
- **DataRate + finite queue still modelled.** `SimpleNetDevice` has a `DataRate`
  attribute (serialization time via `FinishTransmission`) and a real
  `DropTailQueue` (`TxQueue` attribute) whose `MaxSize` can be capped — so the M3
  bottleneck-queue mechanism (finite, really-dropping egress) is preserved, just
  per-direction instead of shared.
- **Bridge-compatible.** `BridgeNetDevice::AddBridgePort` requires only a
  Mac48-addressed port that returns `SupportsSendFrom() == true`.
  `SimpleNetDevice` is Mac48-addressed and `SupportsSendFrom()` returns `true`,
  and it implements `SetPromiscReceiveCallback` (which the bridge registers). So
  it is a legal bridge port with no adaptation.

## Method (the empirical proof — not just "it compiled")

`p3a-fullduplex-spike.cc`, built in `scratch/` against the same pinned ns-3.45 as
the rest of the track (`--build-profile=release`, `--enable-asserts`, same module
set as every scenario's README). Two experiments:

- **EXP1 — full-duplex vs shared-medium, head to head.** Two nodes, one
  point-to-point link. The forward direction (A→B) is saturated with back-to-back
  1500 B data frames (`0x88b5`) at 100 Mbps. The reverse direction (B→A)
  simultaneously carries small 64 B `0x88b6` "gPTP" frames every 137 µs. We
  measure every reverse frame's one-way latency (send time keyed by packet UID,
  matched at receive). The whole thing runs on **both** device models — the
  `SimpleNetDevice` full-duplex pair and the **exact** `CsmaNetDevice`/`CsmaChannel`
  pair M3 uses today — plus an idle control (no forward load) for each.
- **EXP2 — bridge forwarding.** `end0 -- [BridgeNetDevice sw] -- end1`, where `sw`
  bridges two `SimpleNetDevice` ports (each on its own 2-device `SimpleChannel`).
  10 `0x88b6` frames are fired each way, overlapping in time, and counted at each
  end.

## Evidence (verbatim, deterministic)

Built clean (`./ns3 build p3a-fullduplex-spike`, exit 0) and run **twice** —
**byte-identical** stdout across both runs (`md5sum` = `af27bba35590bc09982c537cfab34b61`
for each), exit 0 (PASS):

```
  -- control: no forward saturation (both directions idle-ish) --
  SimpleNetDevice (full-duplex)      reverse gPTP (B->A): n=365 lat us[min/mean/max]=6.00/6.00/6.00   (fwd data delivered=0)
  CsmaNetDevice   (shared medium)    reverse gPTP (B->A): n=365 lat us[min/mean/max]=7.00/7.00/7.00   (fwd data delivered=0)

  -- forward A->B SATURATED with 1500B data at 100Mbps --
  SimpleNetDevice (full-duplex)      reverse gPTP (B->A): n=365 lat us[min/mean/max]=6.00/6.00/6.00   (fwd data delivered=444)
  CsmaNetDevice   (shared medium)    reverse gPTP (B->A): n=190 lat us[min/mean/max]=7.00/10418.36/19577.00   (fwd data delivered=406)

  [EXP1 verdict]
    SimpleNetDevice: reverse-gPTP mean latency idle=6.000us  saturated=6.000us  (max sat=6.000us) -> inflation 1.000x
    CsmaNetDevice  : reverse-gPTP mean latency idle=7.000us  saturated=10418.363us  (max sat=19577.000us) -> inflation 1488.338x

    end1 received 10/10 forwarded gPTP frames (last ethertype 0x88b6)
    end0 received 10/10 forwarded gPTP frames (reverse direction)
```

Reading the numbers:

- **SimpleNetDevice is genuinely full-duplex.** The reverse `0x88b6` frame's
  latency is **6.00 µs flat — identical whether the forward direction is idle or
  saturated** (min = mean = max = 6.00 µs, all 365 frames delivered). Forward
  saturation delivered 444 data frames over the same window and had **zero** effect
  on the reverse gPTP path. (6.00 µs = 64 B / 100 Mbps ≈ 5.12 µs serialization +
  1 µs channel delay, receive firing at end-of-reception — the S1 timestamping
  convention the track already documents.)
- **CsmaNetDevice reproduces S5 exactly.** On the same offered load the reverse
  frame's latency explodes from 7.00 µs (idle) to **mean 10,418 µs, max 19,577 µs**
  (≈1488× inflation), and **175 of 365 reverse frames never arrive** in the window
  (n = 190) — they are stuck behind / colliding with the forward data on the shared
  medium. This is precisely the mechanism `congestion/README.md`'s S5 note and
  P1a's peer-delay-corruption finding describe: transit data on a shared CSMA link
  spuriously delays the reverse-direction gPTP frame.
- **The bridge forwards the custom ethertype.** `end1` received 10/10 frames with
  the ethertype still `0x88b6`, `end0` received 10/10 in the reverse direction, and
  the two overlapping directions did not interfere — a working 3-node hop-by-hop
  chain over full-duplex `SimpleNetDevice` links.

## What this means for closing S5 for real — and the caveats

**Closing S5 is a bounded project, not an open-ended one.** The blocker the plan
worried about (no full-duplex device that passes `0x88b6` without a core patch or
a from-scratch `NetDevice`) is not real: `SimpleNetDevice` is that device. But
"bounded" is not "trivial," and the honest scope is **medium, not small** — it is
*not* a one-line channel-wiring swap. Concretely, a real S5 fix would need:

1. **Swap the transport in all four scenario topologies.** Replace each
   `CsmaHelper::Install` per link with a manually-wired 2-device `SimpleChannel` +
   two `SimpleNetDevice`s (DataRate, finite `TxQueue` cap, Mac48 address, receive
   callback). The gPTP send/receive path itself is transport-agnostic
   (`dev->Send(pkt, peerMac, 0x88b6)` + `SetReceiveCallback`), so `gptp.{h,cc}`
   should need **no** change — but that must be *verified*, not assumed, and
   `gptp.{h,cc}` is vendored byte-identical across four dirs, so any change there
   is a change everywhere with a full Gates 0–2 + M2–M5 regression.
2. **Actually build the hop-by-hop data forwarding** S5 is about — inject the
   background flows at `clientsA/B/C[0]` and forward them through `swA/B/C` → `swCore`
   → `coreClient`. Over full-duplex links this no longer couples reverse-direction
   gPTP (that is the whole point), but it does mean adding real L2 forwarding for
   the data plane. Two routes, each with a cost:
   - a `BridgeNetDevice` on each switch (proven here to forward the ethertype) —
     but note the current design deliberately does **per-port gPTP termination**
     (S4), so gPTP must stay off the bridged path (e.g. different device set, or
     rely on gPTP's per-port callbacks intercepting before the bridge forwards);
     reconciling "bridge forwards data" with "gPTP is terminated per-port" is the
     main design question a real fix has to answer, and it is **not** answered by
     this spike.
   - or manual per-switch forwarding of the data ethertype in the receive callback
     (no bridge), which keeps gPTP termination clean but hand-rolls the data path.
3. **Re-establish every M3/M4 number under the new transport.** The bottleneck is
   now one direction of a full-duplex link rather than a shared medium, so the
   drop rate, backlog, `coreClient` peak, and the isolation shape must be
   re-measured and re-gated. The isolation *should* get cleaner (that is the
   hypothesis), but P1a's servo hardening and peer-delay filter were tuned against
   the CSMA artifact; their behavior on the cleaner transport must be re-verified,
   not assumed to carry over.

**New limitations the approach itself introduces (disclosed):**

- **`SimpleNetDevice` is a deliberately simplified PHY/MAC.** No inter-frame gap,
  no real Ethernet framing/preamble, no CSMA/CD (by design — that is the feature
  here). Its pcap output is not the same as `CsmaHelper::EnablePcapAll`'s
  Ethernet-framed capture, so **P2c's pcap path would need rework** (the P2c
  verifier parses classic-pcap Ethernet frames). Anything downstream that assumes
  the CSMA capture format is affected.
- **Manual wiring, not a one-liner.** There is a `SimpleNetDeviceHelper`, but to
  guarantee exactly two devices per channel (true point-to-point, not a shared
  segment) the topology builder is more verbose than `csma.Install(NodeContainer)`.
- **This spike did *not* test the full 18-node M3 topology or whether the
  spurious-coupling problem is actually cured end-to-end.** It proves the two
  necessary primitives (full-duplex 0x88b6 transport + bridge forwarding of the
  ethertype). It does **not** prove the sufficient result (M3 isolation holds with
  real hop-by-hop forwarding). That end-to-end check is the first task of the real
  fix, and remains genuinely unproven here — this is a feasibility spike, and the
  claim is bounded to feasibility.

## Bottom line

Feasibility: **YES**, with hard evidence — `SimpleNetDevice`/`SimpleChannel` is an
existing mainline ns-3.45 device that carries `0x88b6` full-duplex (reverse
latency provably unaffected by forward saturation: 1.000× vs CSMA's 1488×) and
bridges it (10/10 both ways). No core patch, no new `NetDevice` subclass. The
real S5 fix is a **bounded, medium-effort** job (swap transport in the four
topologies + build genuine hop-by-hop data forwarding + full regression), whose
one genuinely open design question — reconciling per-port gPTP termination (S4)
with bridged hop-by-hop data forwarding — this spike surfaces but does not
resolve. Whether to spend that effort is the calling session's decision.

## Reproduce

```bash
git clone --branch ns-3.45 --depth 1 https://gitlab.com/nsnam/ns-3-dev.git /tmp/ns-3-dev
cp -r ns3/spikes /tmp/ns-3-dev/scratch/syncsim-spike
cd /tmp/ns-3-dev
./ns3 configure --build-profile=release \
    --enable-modules=core,network,csma,bridge,point-to-point,applications,internet,flow-monitor \
    --disable-examples --disable-tests --disable-python --enable-asserts --enable-logs
./ns3 build p3a-fullduplex-spike
./build/scratch/syncsim-spike/ns3.45-p3a-fullduplex-spike   # exit 0 == spike PASS
```

Same sandbox caveat as every other gate on this track: built and run in the local
ns-3.45 sandbox (no Docker daemon here), deterministic across two runs.
```
