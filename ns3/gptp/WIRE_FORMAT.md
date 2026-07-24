# gPTP wire format (ns-3 track) — as implemented and tshark-verified

**Status: P3c (Tier 3) — CLOSED.** `ns3/gptp/gptp.{h,cc}` now emit the **byte-exact
IEEE 802.1AS-2011 / IEEE 1588-2008 wire format**, replacing the pragmatic 19-byte
custom header (`type:1 + seq:2 + ta:8 + tb:8`) used through P2c. Captures written
with `--pcapPrefix` are now dissected by **unmodified tools** (Wireshark / tshark's
own PTPv2 dissector), not just this project's `check_pcap_gptp.py`.

This file documents the **actual, verified** layout the code emits (cross-checked
against real tshark dissection, which is stronger ground truth than any spec
draft). It is meant to be a genuinely useful reference for someone debugging a
real 802.1AS capture.

## Provenance / clean-room

The byte layout was derived from the **public** IEEE 1588-2008 / 802.1AS-2011
specification, cross-checked against multiple independent public descriptions —
**not** copied from any GPL source (INET's `Gptp.cc`, linuxptp, or Wireshark's
dissector). Same clean-room discipline as every other file in this subtree.
tshark is used only as an **external oracle** to confirm the bytes are correct.

## Scope (strict 802.1AS)

Peer-delay (P2P) mechanism only. Supported message types: **Announce, Sync,
Follow_Up** (with the 802.1 Follow_Up Information TLV), **Pdelay_Req,
Pdelay_Resp, Pdelay_Resp_Follow_Up**. No E2E `Delay_Req`/`Delay_Resp` (real gPTP
never sends those), no PTPv1, no `Signaling`/`Management` (this simulator does not
use them).

- **EtherType**: `0x88F7` (the real IEEE-assigned PTP EtherType; was `0x88b6`).
- **Destination MAC**: `01-80-C2-00-00-0E` (the reserved, non-forwarded gPTP
  multicast; was the peer's unicast address). Empirically confirmed to deliver
  over this project's 2-device point-to-point `SimpleNetDevice`/`CsmaChannel`
  links on both transports before adoption.
- **ClockIdentity**: standard EUI-64 from each port's device MAC — insert
  `0xFF 0xFE` in the middle (`AA:BB:CC:DD:EE:FF` → `AA:BB:CC:FF:FE:DD:EE:FF`),
  implemented once in `MacToClockIdentity()` and used for every
  `sourcePortIdentity` / `requestingPortIdentity` / `grandmasterIdentity`.

## Common PTP header — 34 bytes (identical for every message type)

| Off | Size | Field | Value this project emits |
|----:|-----:|-------|--------------------------|
| 0 | 1 | `transportSpecific`\|`messageType` | high nibble `0x1` (gPTP), low nibble = messageType |
| 1 | 1 | `reserved`\|`versionPTP` | `0x02` |
| 2 | 2 | `messageLength` (U16 BE) | total message bytes (44/54/76 by type) |
| 4 | 1 | `domainNumber` | `0` |
| 5 | 1 | `reserved` | `0` |
| 6 | 1 | `flagField[0]` | `0x02` (twoStepFlag) for Sync & Pdelay_Resp, else `0` |
| 7 | 1 | `flagField[1]` | `0` |
| 8 | 8 | `correctionField` (Int64 BE) | nanoseconds × 2¹⁶ (sub-ns fraction always 0) |
| 16 | 4 | `reserved` | `0` |
| 20 | 8 | `sourcePortIdentity.clockIdentity` | EUI-64 from the sending port's MAC |
| 28 | 2 | `sourcePortIdentity.portNumber` (U16 BE) | port index + 1 |
| 30 | 2 | `sequenceId` (U16 BE) | per-type sequence counter |
| 32 | 1 | `controlField` | Sync=0, Follow_Up=2, everything else=5 |
| 33 | 1 | `logMessageInterval` (Int8) | round(log₂ interval s); `0x7F` for Pdelay_Resp/…FollowUp |

`messageType` nibbles: **Sync=0x0, Pdelay_Req=0x2, Pdelay_Resp=0x3,
Follow_Up=0x8, Pdelay_Resp_Follow_Up=0xA, Announce=0xB.**

## Per-message-type body (after the 34-byte common header)

**Sync** — 10-byte body, 44 bytes total. `originTimestamp` (secondsField 6 B BE +
nanosecondsField 4 B BE), all-zero (always 2-step; precise time is in Follow_Up).

**Follow_Up** — 42-byte body, 76 bytes total.
- `preciseOriginTimestamp` (6 B sec + 4 B ns) — the reconstructed GM-relative
  origin time (the value the code calls `ta` for Follow_Up).
- Follow_Up Information TLV (32 B): `tlvType`=0x0003 (ORGANIZATION_EXTENSION),
  `lengthField`=28, `organizationId`=00-80-C2 (IEEE 802.1 OUI),
  `organizationSubType`=00-00-01, `cumulativeScaledRateOffset`=0 (Int32),
  `gmTimeBaseIndicator`=0 (U16), `lastGmPhaseChange`=0 (ScaledNs, 12 B),
  `scaledLastGmFreqChange`=0 (Int32). *The correctionField (accumulated peer
  delay + residence, the code's `tb`) lives in the common header at offset 8,
  not in the body.*

**Pdelay_Req** — 20-byte body, 54 bytes total. `originTimestamp` all-zero (t1 is
tracked locally by the requester, as in real 2-step) + 10 B reserved (zero).

**Pdelay_Resp** — 20-byte body, 54 bytes total. `requestReceiptTimestamp` (t2,
6 B sec + 4 B ns) + `requestingPortIdentity` = the **Pdelay_Req sender's**
clockIdentity (8 B) + portNumber (2 B), echoed from the request being answered.

**Pdelay_Resp_Follow_Up** — 20-byte body, 54 bytes total.
`responseOriginTimestamp` (t3) + the same echoed `requestingPortIdentity`.

**Announce** — 42-byte body, 76 bytes total (NEW in P3c; GM-only, additive,
not consumed by any receiver).
- `originTimestamp` all-zero (10 B), `currentUtcOffset`=37 (Int16), reserved (1 B),
  `grandmasterPriority1`=128, `grandmasterClockQuality`={clockClass=248,
  clockAccuracy=0xFE, offsetScaledLogVariance=0xFFFF}, `grandmasterPriority2`=128,
  `grandmasterIdentity`=GM's own ClockIdentity (8 B), `stepsRemoved`=0 (U16),
  `timeSource`=0xA0 (internal oscillator).
- Path Trace TLV (12 B): `tlvType`=0x0008 (PATH_TRACE), `lengthField`=8,
  `pathSequence[0]`=GM's own ClockIdentity (only the GM emits, so one entry).

Announce carries static/hardcoded values: this project has no dynamic BMCA (the
GM is fixed by construction), and Announce is emitted GM-only on every master
port on the same cadence as Sync, never relayed and never fed to any servo.

## tshark verification (the authoritative correctness check)

`tshark 4.2.2`, capturing on `gptp-spike` with `--pcapPrefix` (gm↔sw link,
`--simTime=1.0`). **Every frame is recognised as valid PTP; zero malformed.**

```
## Message-type tally on the gm<->sw link (7 sync cycles):
      7  Sync                 (0x00)
     20  Pdelay_Req           (0x02)
     20  Pdelay_Resp          (0x03)
      7  Follow_Up            (0x08)
     20  Pdelay_Resp_Follow_Up(0x0a)
      7  Announce             (0x0b)
## Malformed frames: 0
```

tshark not only dissects every field but **links the message chains** it is
supposed to:

- Sync (44 B): `messageType: Sync (0x0)`, `flags: 0x0200` (PTP_TWO_STEP),
  `controlField: Sync (0)`, `logMessagePeriod: -3 (0.125 s)`,
  `[No Follow Up ...]` → paired to its Follow_Up in the next frame.
- Follow_Up (76 B): `preciseOriginTimestamp 0 s / 125000000 ns`, full
  `Follow Up information TLV` (Organization extension 0x0003, len 28, orgId
  0x8080C2 → shown "32962", subType 1, all-zero rate/phase fields), and tshark's
  own `[calculatedSyncTimestamp: 0.125]` + `[This is a Follow Up to Sync in
  Frame: 10]`.
- Pdelay_Req (54 B): `messageType: Peer_Delay_Req (0x2)`,
  `ClockIdentity 0x000000fffe000002` (EUI-64 of MAC `00:00:00:00:00:02`),
  `logMessagePeriod: -4 (0.0625 s)`.
- Pdelay_Resp (54 B): `flags 0x0200`, `requestReceiptTimestamp`,
  `requestingSourcePortIdentity 0x000000fffe000002` (correctly echoed),
  `logMessagePeriod: 127` (0x7F unknown), `[... Response to Request in Frame: 1]`.
- Pdelay_Resp_Follow_Up (54 B): `responseOriginTimestamp`, echoed
  `requestingSourcePortIdentity`, `[... Follow Up to Response in Frame: 2]`.
- Announce (76 B): `originCurrentUTCOffset 37`, `priority1 128`,
  `grandmasterClockClass 248`, `grandmasterClockAccuracy Accuracy Unknown (0xfe)`,
  `grandmasterClockVariance 65535`, `priority2 128`,
  `grandmasterClockIdentity 0x000000fffe000001`, `localStepsRemoved 0`,
  `TimeSource INTERNAL_OSCILLATOR (0xa0)`, `Path trace TLV` with
  `PathSequence 0x000000fffe000001`.

Verified on **both** transports: `nominal` (CsmaHelper `EnablePcapAll`, real
Ethernet frames) and `congestion` (`SimpleNetDevice` + the manual
`PcapHelper`/`PcapFileWrapper` hook that synthesises a 14-byte Ethernet header).
**Zero malformed across all 68 capture files** (34 nominal + 34 congestion).
`check_pcap_gptp.py` also PASSes on both.

To reproduce:

```
build/scratch/syncsim-gptp/ns3.45-gptp-spike --simTime=1.0 --pcapPrefix=/tmp/g
tshark -r /tmp/g-0-0.pcap -Y ptp -T fields -e ptp.v2.messagetype
tshark -r /tmp/g-0-0.pcap -Y "frame.number==11" -O ptp   # a Follow_Up + its TLV
tshark -r /tmp/g-0-0.pcap -Y "frame.number==12" -O ptp   # an Announce + Path Trace TLV
```

## Precision & the one honest number change (disclosed)

This is a pure wire-**encoding** rewrite: `GptpEntity`'s algorithm (`ApplyServo`,
the peer-delay math, `neighborRateRatio`, the residence relay) is byte-for-byte
unchanged and still operates on `ns3::Time` values extracted from the header.

- **Timestamps are lossless.** ns-3's default time resolution is nanoseconds, so
  every `Time` in this model is already an integer number of nanoseconds, exactly
  representable as the PTP `secondsField`(48)+`nanosecondsField`(32) pair. Encode
  → decode round-trips identically; no precision is lost vs. the old femtosecond
  slots.
- **Frame sizes changed, and ns-3 charges per-byte serialization.** The real
  802.1AS frames are larger than the old 19-byte header (Pdelay 54 B, Sync 44 B,
  Follow_Up 76 B), and per simplification **S1** the measured peer delay folds in
  one frame serialization time. So the **displayed peer-delay values grow** (e.g.
  `gptp-spike` gm↔sw `6.62 → 7.26 µs`), and that larger constant peer-delay DC
  bias ripples through the **pre-lock transient peaks** by ~1–7% depending on
  scenario. This is the S1 serialization property acting faithfully on realistic
  frame sizes — **not** a byte-layout bug (tshark's clean dissection is the proof
  the layout is correct). The **gated properties are unaffected**: every final
  offset is still exactly `0.000` (the servo nulls the constant bias each cycle),
  the drift-proportional peak ordering holds, the M3 isolation shape is still
  exact (16/17 nodes at ratio 1.0×), the M4 coupling non-finding is preserved
  (all deltas `0.000`), servo counts are unchanged in the lossless scenarios, and
  all four gates PASS deterministically. See `NS3_PARITY_PLAN.md`'s P3c section
  for the full before/after table.
