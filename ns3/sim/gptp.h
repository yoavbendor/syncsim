// SPDX-License-Identifier: Apache-2.0
//
// syncsim ns-3 migration POC -- Phase 2 (R-GPTP).
//
// Clean-room, permissively-licensed minimal IEEE 802.1AS (gPTP) mechanism for
// ns-3: the peer-delay measurement, Sync/Follow_Up propagation with
// residence-time correction at a time-aware bridge, and a servo that steers a
// Phase-1 syncsim::Clock. Written from the public 802.1AS protocol description
// and INET's M1 *output numbers* (as the empirical target to reproduce, NOT as
// source to copy). INET's Gptp.cc was deliberately NOT read/ported, and no
// linuxptp source was copied -- only the well-understood public control-loop
// idea (phase step + integral frequency correction) was reimplemented here.
// This keeps the copyright on these files entirely ours (Apache-2.0 SPDX),
// distinct from ns-3 core's GPL-2.0-only headers.
//
// Honest licensing caveat (same as clock.h / the POC plan): the Apache-2.0
// header covers THIS file's copyright only. Because this links against ns-3
// core (GPL-2.0-only), the combined, distributed binary is still GPLv2.
//
// ---------------------------------------------------------------------------
// Simplifications taken for this first spike (stated, not hidden):
//
//   S1. TIMESTAMPING. "Hardware" timestamps are the time a Simulator::Schedule'd
//       send fires and the time a NetDevice receive callback fires. This is
//       coarser than INET's streaming-PHY receptionStarted/transmissionStarted
//       signals (which timestamp at the SFD): our receive callback fires at the
//       *end* of frame reception, so a measured "link delay" folds in one frame
//       serialization time in addition to the channel propagation delay. That is
//       fine here -- it is a real, non-trivial, positive, stable value, which is
//       all Gate 2 asks the peer-delay mechanism to prove.
//
//   S2. 2-STEP framing -- CLOSED (P2b, Tier 2). Formerly 1-step: Pdelay_Resp
//       carried both (t2, t3) and Sync carried the origin timestamp + correction
//       directly. Now split into the real two-message form:
//         - Pdelay_Resp carries only the request-receipt timestamp (t2); a
//           separate Pdelay_Resp_Follow_Up carries the response-origin timestamp
//           (t3). The requester holds pending state (t2, t4) after the Resp and
//           completes the peer-delay math when the Follow_Up arrives.
//         - Sync is a bare marker (its arrival defines the slave's syncRxLocal);
//           a separate Follow_Up carries the preciseOriginTimestamp + correction
//           field. The slave holds pending state (syncRxLocal, seq) after the
//           Sync and runs the servo / bridge relay when the Follow_Up arrives.
//       P2b's verification confirmed this is informationally identical to the
//       1-step form IN LOSSLESS / light-loss conditions (Gate 2, M2, M4 all match
//       the 1-step numbers to <=1 ns, finals 0.000, servo counts identical):
//       because the bare Sync has the same wire size and slot as the old combined
//       Sync, the slave's recvLocal is unchanged, and the residence-time
//       correction self-compensates for the extra Sync->Follow_Up gap at a bridge.
//       HONEST EXCEPTION, found by verifying rather than assuming (M3 congestion):
//       under heavy sustained loss the one-extra-frame-per-cycle is a REAL
//       behavioral difference -- a cycle now counts only if BOTH the Sync and its
//       Follow_Up survive the lossy queue, so the effective sync rate roughly
//       halves (coreClient 170 -> 90 servo corrections) and the worst-delayed
//       samples (whose partner frame was dropped) are filtered out, LOWERING the
//       surviving congested peak (510 -> 429 us). This is faithful 802.1AS 2-step
//       behavior (the 1-step form was simply more loss-robust by carrying
//       everything in one frame); the M3 isolation shape and gate are unchanged.
//       See congestion/README.md for the full write-up.
//
//   S3. neighborRateRatio -- CLOSED (P2a, Tier 2). Formerly assumed = 1 (both
//       link ends treated as equal-rate clocks). Now DERIVED per port from two
//       successive Pdelay exchanges: neighborRateRatio = (neighbor-elapsed) /
//       (local-elapsed), comparing how far the responder's reported timestamp
//       (t3) advanced against how far our own receive timestamp (t4) advanced
//       between the two exchanges, and folded into BOTH the peer-delay math (the
//       responder turnaround (t3-t2), measured on the neighbor's clock, is
//       divided by the ratio to express it in our local time base) and the
//       residence-time correction (a bridge's residence, measured on its local
//       clock, is scaled by its upstream port's ratio to express it toward GM
//       time). As S3 always documented and P2a's verification CONFIRMED: at this
//       project's drift magnitudes (tens-hundreds of ppm) over microsecond
//       durations the term is provably sub-picosecond, so no observed number
//       moved to 3+ significant figures -- this closes S3 for protocol
//       completeness, not to change any result.
//
//   S4. gPTP frames are terminated per-port (NOT transparently L2-forwarded).
//       Phase 0's smoke test installed a BridgeNetDevice on sw; this spike does
//       NOT, because real 802.1AS gPTP frames use a link-local reserved
//       multicast address that bridges do not forward -- each port is a gPTP
//       endpoint and the time-aware bridge *regenerates* Sync downstream with a
//       fresh correction field. We keep Phase 0's exact CsmaHelper per-link
//       construction (same 4-node/3-link shape) and attach our own per-device
//       receive callbacks instead of a bridge. See gptp-spike.cc.
// ---------------------------------------------------------------------------
// WIRE FORMAT (P3c, Tier 3 -- CLOSED). Formerly a pragmatic 19-byte custom
// header (type:1 + seq:2 + ta:8 + tb:8). Now the byte-exact IEEE 802.1AS-2011 /
// IEEE 1588-2008 wire format, so real captures are dissectable by unmodified
// tools (tshark's PTPv2 dissector, Wireshark) -- not just this project's own
// check_pcap_gptp.py. Scope is STRICT 802.1AS: peer-delay (P2P) only, no E2E
// Delay_Req/Resp, no PTPv1, no Signaling/Management. Supported message types:
// Announce, Sync, Follow_Up (with the 802.1 Follow_Up Information TLV),
// Pdelay_Req, Pdelay_Resp, Pdelay_Resp_Follow_Up. The 34-byte common PTP header
// (transportSpecific=0x1 gPTP marker, versionPTP=2, messageLength,
// correctionField as ns*2^16, sourcePortIdentity = EUI-64 ClockIdentity +
// portNumber, sequenceId, controlField, logMessageInterval) precedes each
// message's body; see ns3/gptp/WIRE_FORMAT.md for the exact, tshark-verified
// per-type byte tables. EtherType is now the IEEE-assigned 0x88F7 (was 0x88b6),
// and every message is sent to the reserved non-forwarded multicast destination
// 01-80-C2-00-00-0E (was the peer's unicast address) -- both faithful to real
// gPTP and both empirically confirmed to deliver over this project's 2-device
// SimpleNetDevice / CsmaChannel point-to-point links.
//
// This is a pure wire-ENCODING rewrite: GptpEntity's algorithm (ApplyServo,
// peer-delay math, neighborRateRatio, residence relay) is byte-for-byte
// unchanged and still operates on ns3::Time values extracted from the header.
// Because ns-3's default time resolution is nanoseconds, every Time in this
// model is already an integer number of nanoseconds, so encoding a timestamp as
// the PTP secondsField(48)+nanosecondsField(32) pair is LOSSLESS -- no offset,
// peak, servo-count, or isolation number moves. The one honest, expected
// consequence (disclosed, not hidden -- same discipline as every prior phase):
// the real 802.1AS frames are larger than the old 19-byte header (Pdelay 54 B,
// Sync 44 B, Follow_Up 76 B vs 19 B), and per S1 the measured peer delay folds
// in one frame serialization time, so the DISPLAYED peer-delay values grow to
// reflect the real, larger frames. This is the S1 serialization property acting
// faithfully on realistic frame sizes; the gated properties (convergence to ~0,
// drift-proportional peaks, isolation shape, servo counts) are unaffected
// because the servo nulls the constant peer-delay DC offset every cycle. See
// WIRE_FORMAT.md for the before/after peer-delay figures and the tshark
// dissection transcript.
// ---------------------------------------------------------------------------
// SERVO (P1a, Tier 1 -- hardened; supersedes the Phase-2 first-spike servo).
//
// The Phase-2 servo was a DEADBEAT phase step (null the entire measured offset
// every Sync) plus a naive integral frequency term that re-derived a fresh rate
// estimate `offset / elapsed` each cycle, where `elapsed` was the ACTUAL global
// time since the previous Sync. Under congestion, when a Sync toward a node is
// sporadically dropped or badly delayed in the shared egress queue, `elapsed`
// balloons to a multiple of the Sync interval and that single-interval rate
// estimate goes wild -- an unbounded, badly-wrong frequency correction that the
// deadbeat phase step then rings on for several cycles. That is the sole source
// of M3's 24x-outlier congested peak (see congestion/README.md); the isolation
// SHAPE was always correct, only this one magnitude was untrustworthy.
//
// The hardened servo reimplements the well-understood public linuxptp PI
// control-loop IDEA (same clean-room discipline as the original -- the public
// algorithm, never ptp4l source), with two robustness properties the first
// spike lacked:
//
//   (a) PROPORTIONAL phase + BOUNDED INTEGRAL frequency. The phase loop steps a
//       damped fraction (kPhaseGain < 1) of the measured offset instead of the
//       full deadbeat step, so a single jittered/late Sync cannot inject a full
//       excursion. The frequency loop accumulates a low-pass correction into a
//       persistent integral (kIntegralGain per cycle), CLAMPED to +-kFreqClampPpm,
//       and -- crucially -- normalized by the NOMINAL Sync interval (a constant),
//       never by the actual elapsed time, so a ballooned gap cannot scale it.
//   (b) MISSED-SYNC OUTLIER HANDLING. The nominal Sync interval is inferred at
//       run time as the minimum inter-Sync gap observed (missed Syncs only ever
//       lengthen the gap). A cycle whose elapsed time exceeds kMissedSyncFactor x
//       that nominal is treated as a missed-Sync recovery: its phase is still
//       corrected (damped), but its frequency-integral update is SKIPPED, so a
//       single anomalously-long gap can no longer corrupt the frequency estimate.
//
// Still simplified vs. a production port: single fixed PI gain pair (no adaptive
// state machine / spike-rejection thresholds / step-vs-lock modes that real
// ptp4l carries), and phase steering via an idealized offset step rather than a
// frequency-only lock. What it fixes is exactly the undamped, unbounded
// single-sample overreaction; it does not claim ptp4l-grade servo behavior.
// ---------------------------------------------------------------------------

#ifndef SYNCSIM_GPTP_H
#define SYNCSIM_GPTP_H

#include "clock.h"

#include "ns3/address.h"
#include "ns3/callback.h"
#include "ns3/header.h"
#include "ns3/mac48-address.h"
#include "ns3/net-device.h"
#include "ns3/nstime.h"
#include "ns3/traced-callback.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace syncsim {

// gPTP message discriminator. Values are the REAL IEEE 1588-2008 messageType
// 4-bit codes (P3c) -- the low nibble of the first wire byte (the high nibble is
// transportSpecific = 0x1, the 802.1AS gPTP marker). They no longer index the
// old custom header slots; they ARE the on-wire messageType field.
enum class GptpMsgType : uint8_t
{
    Sync = 0x0,               // master -> slave: bare marker (its arrival = syncRxLocal) [2-step]
    PdelayReq = 0x2,          // requester -> responder: peer-delay request
    PdelayResp = 0x3,         // responder -> requester: carries t2 (requestReceiptTimestamp)
    FollowUp = 0x8,           // master -> slave: preciseOriginTimestamp + correctionField + TLV
    PdelayRespFollowUp = 0xA, // responder -> requester: carries t3 (responseOriginTimestamp)
    Announce = 0xB            // GM -> downstream: periodic BMCA/GM-quality broadcast (additive)
};

// The 6-byte EUI-64 ClockIdentity type (8 bytes) derived from a device MAC.
using ClockIdentity = std::array<uint8_t, 8>;

// Standard 802.1AS default ClockIdentity from a 6-byte MAC via EUI-64: insert
// 0xFF 0xFE in the middle (AA:BB:CC:DD:EE:FF -> AA:BB:CC:FF:FE:DD:EE:FF).
ClockIdentity MacToClockIdentity(const ns3::Address& mac);

// The reserved, non-forwarded gPTP multicast destination MAC (01-80-C2-00-00-0E)
// that real 802.1AS uses for every message type. Delivery to it over this
// project's 2-device point-to-point links was empirically confirmed on both
// SimpleNetDevice and CsmaNetDevice transports (P3c spike).
ns3::Address GptpMulticastAddress();

// On-the-wire gPTP message: byte-exact IEEE 802.1AS-2011 / IEEE 1588-2008
// (P3c). A 34-byte common PTP header + a per-messageType body (+ TLV for
// Follow_Up / Announce). See ns3/gptp/WIRE_FORMAT.md for the tshark-verified
// byte tables. The in-memory representation keeps only the fields this project's
// mechanism reads or writes; all fixed/static fields (versionPTP, domain,
// control, flags, the Follow_Up/Path-Trace TLVs, Announce's GM-quality
// constants) are synthesized in Serialize() and skipped in Deserialize().
//
//   ta  = the message's primary body timestamp:
//           Follow_Up            -> preciseOriginTimestamp
//           Pdelay_Resp          -> requestReceiptTimestamp (t2)
//           Pdelay_Resp_Follow_Up-> responseOriginTimestamp (t3)
//           Sync / Pdelay_Req / Announce -> unused (wire value all-zero)
//   tb  = correctionField (common header, Follow_Up only; ns * 2^16 on the wire)
// Both are lossless: ns-3's default ns time resolution means every Time is an
// integer number of nanoseconds, exactly representable as PTP seconds+ns.
class GptpHeader : public ns3::Header
{
  public:
    GptpHeader() = default;

    void SetType(GptpMsgType t) { m_type = t; }
    GptpMsgType GetType() const { return m_type; }
    void SetSeq(uint16_t s) { m_seq = s; }
    uint16_t GetSeq() const { return m_seq; }
    // Primary body timestamp (see class comment). Wire: secondsField(48)+ns(32).
    void SetTa(ns3::Time t) { m_taNs = t.GetNanoSeconds(); }
    ns3::Time GetTa() const { return ns3::NanoSeconds(m_taNs); }
    // correctionField (Follow_Up). Wire: Int64 BE = whole-ns * 2^16 (sub-ns 0).
    void SetTb(ns3::Time t) { m_corr = t.GetNanoSeconds() * 65536; }
    ns3::Time GetTb() const { return ns3::NanoSeconds(m_corr / 65536); }

    // sourcePortIdentity (set for every outgoing message from the sending port).
    void SetSourceIdentity(const ClockIdentity& id, uint16_t portNumber)
    {
        m_srcClockId = id;
        m_srcPort = portNumber;
    }
    const ClockIdentity& GetSourceClockId() const { return m_srcClockId; }
    uint16_t GetSourcePortNumber() const { return m_srcPort; }

    // requestingPortIdentity (Pdelay_Resp / Pdelay_Resp_Follow_Up: echoed back
    // from the Pdelay_Req this responds to -- a real, meaningful field).
    void SetRequestingIdentity(const ClockIdentity& id, uint16_t portNumber)
    {
        m_reqClockId = id;
        m_reqPort = portNumber;
    }
    const ClockIdentity& GetRequestingClockId() const { return m_reqClockId; }
    uint16_t GetRequestingPortNumber() const { return m_reqPort; }

    // logMessageInterval (Int8, informational). 0x7F = "unknown" default.
    void SetLogInterval(int8_t v) { m_logInterval = v; }
    int8_t GetLogInterval() const { return m_logInterval; }

    static ns3::TypeId GetTypeId();
    ns3::TypeId GetInstanceTypeId() const override;
    void Print(std::ostream& os) const override;
    uint32_t GetSerializedSize() const override;
    void Serialize(ns3::Buffer::Iterator start) const override;
    uint32_t Deserialize(ns3::Buffer::Iterator start) override;

  private:
    GptpMsgType m_type{GptpMsgType::Sync};
    uint16_t m_seq{0};
    int64_t m_taNs{0};                     // primary body timestamp, whole ns
    int64_t m_corr{0};                     // correctionField, ns * 2^16
    int8_t m_logInterval{0x7F};            // logMessageInterval (informational)
    ClockIdentity m_srcClockId{};          // sourcePortIdentity.clockIdentity
    uint16_t m_srcPort{1};                 // sourcePortIdentity.portNumber (1-based)
    ClockIdentity m_reqClockId{};          // requestingPortIdentity.clockIdentity
    uint16_t m_reqPort{0};                 // requestingPortIdentity.portNumber
};

// The gPTP protocol entity for one node. Owns a set of ports (one per link),
// each either a MASTER port (sends Sync downstream, responds to Pdelay) or a
// SLAVE port (initiates Pdelay, receives Sync, feeds the servo). A node may be:
//   - the grandmaster (isGm): drift-free by construction, sources Sync, no slave
//     port, never runs a servo.
//   - a time-aware bridge (sw): ONE slave port + one-or-more master ports; it
//     receives Sync on the slave port, servos its own clock, and regenerates
//     Sync downstream on every master port with an updated correction field
//     (folding in the upstream peer delay + its own residence time).
//   - an end station (client): a single slave port; servo only.
//
// All message timestamps are taken from the node's local syncsim::Clock, not
// from Simulator::Now() -- routing timestamps through node-local time is exactly
// the invasive plumbing the survey flagged as R-GPTP's core difficulty.
class GptpEntity
{
  public:
    // clock is owned by the caller (the scenario) and outlives this entity.
    GptpEntity(std::string name, Clock* clock, bool isGm);

    // Register a port. peerMac is the neighbor device's address on this link
    // (Pdelay/Sync frames are unicast to it). isMaster selects the port role.
    // Returns the port index (used to route the device receive callback back in
    // via OnDeviceReceive).
    uint32_t AddPort(ns3::Ptr<ns3::NetDevice> dev, ns3::Address peerMac, bool isMaster);

    // The single receive trampoline: the scenario binds a NetDevice receive
    // callback to (this, portIndex) so every gPTP frame lands here.
    bool OnDeviceReceive(uint32_t portIndex,
                         ns3::Ptr<const ns3::Packet> packet,
                         const ns3::Address& from);

    // --- driver hooks the scenario schedules -----------------------------
    // Kick off a peer-delay exchange on every port whose local end initiates it
    // (slave ports; the GM initiates on its master port too so both link ends of
    // gm<->sw get measured). Reschedules itself every pdelayInterval.
    void StartPdelay(ns3::Time pdelayInterval);
    // GM only: emit a Sync on every master port. Reschedules every syncInterval.
    void SendSync(ns3::Time syncInterval);
    // GM only (P3c, additive): emit an Announce on every master port. Purely for
    // wire-realism/dissectability -- carries static GM-quality/BMCA fields (this
    // project has no dynamic BMCA; the GM is fixed by construction) and is NOT
    // consumed by any receiver's servo/offset math. Reschedules every interval,
    // on the same cadence pattern the scenario uses for SendSync.
    void SendAnnounce(ns3::Time announceInterval);

    // --- observability ---------------------------------------------------
    // Fired every time this node's servo processes a Sync-derived offset:
    // (globalNow, offsetSeconds = localTime - reconstructedGmTime). This is the
    // node's gPTP-measured offset-from-GM, the exact analog of what simdata.py
    // derives for the INET run. The scenario records this as the trajectory.
    void ConnectOffsetTrace(ns3::Callback<void, ns3::Time, double> cb);

    const std::string& GetName() const { return m_name; }
    // Measured mean link delay on a port (0 until first Pdelay completes).
    ns3::Time GetLinkDelay(uint32_t portIndex) const;
    // Measured neighborRateRatio on a port (S3, P2a): neighbor rate / local rate
    // (1.0 until two Pdelay exchanges establish it). For scenario reporting.
    double GetNeighborRateRatio(uint32_t portIndex) const;
    uint32_t GetPortCount() const { return m_ports.size(); }
    uint32_t GetServoCount() const { return m_servoCount; }

  private:
    struct Port
    {
        ns3::Ptr<ns3::NetDevice> dev;
        ns3::Address peerMac;      // gPTP multicast destination (P3c) frames are sent to
        bool isMaster;
        ClockIdentity clockId{};   // this port's EUI-64 ClockIdentity (from dev MAC)
        ns3::Time linkDelay{0};   // measured mean propagation delay (S1: incl. one serialization)
        uint16_t pdelaySeq{0};    // our outstanding Pdelay_Req seq
        ns3::Time pdelayT1{0};    // local tx time of our outstanding Pdelay_Req
        // 2-step Pdelay (S2 closed): requester pending state between receiving the
        // Pdelay_Resp (carries t2) and its Pdelay_Resp_Follow_Up (carries t3).
        bool pdelayRespSeen{false};
        ns3::Time pdelayT2{0};    // responder rx (t2), from the Pdelay_Resp
        ns3::Time pdelayT4{0};    // our rx of the Pdelay_Resp (t4)
        // neighborRateRatio (S3, closed by P2a): neighbor clock rate / local
        // clock rate on this link, estimated from consecutive Pdelay exchanges.
        // 1.0 until the second exchange establishes it. rrPrevT3/rrPrevT4 hold
        // the previous exchange's responder-tx (t3) and our-rx (t4) timestamps.
        double neighborRateRatio{1.0};
        bool haveRrPrev{false};
        ns3::Time rrPrevT3{0};
        ns3::Time rrPrevT4{0};
    };

    // Serializes hdr onto a fresh packet, stamping the sending port's
    // sourcePortIdentity (EUI-64 ClockIdentity + 1-based portNumber) and the
    // per-type logMessageInterval, then sends to the gPTP multicast destination.
    void SendFrame(uint32_t portIndex, GptpHeader& hdr);
    void HandlePdelayReq(uint32_t portIndex, const GptpHeader& in);
    void HandlePdelayResp(uint32_t portIndex, const GptpHeader& in);
    void HandlePdelayRespFollowUp(uint32_t portIndex, const GptpHeader& in);
    void HandleSync(uint32_t portIndex, const GptpHeader& in);
    void HandleFollowUp(uint32_t portIndex, const GptpHeader& in);
    void HandleAnnounce(uint32_t portIndex, const GptpHeader& in);
    // Bridge relay: regenerate Sync on all master ports after the residence time.
    void RelayDownstream(ns3::Time originTs, ns3::Time upstreamCorrection,
                         ns3::Time slaveRecvLocal, ns3::Time slaveLinkDelay,
                         double offsetSecForServo);
    // The servo: proportional (damped) phase step + bounded integral frequency
    // correction on the local Clock, with missed-Sync outlier handling. The
    // inter-Sync interval it needs is derived internally from Simulator::Now()
    // and m_lastSyncGlobal, so the call sites (HandleSync/RelayDownstream) are
    // unchanged. See the SERVO block in the file header for the algorithm.
    void ApplyServo(double offsetSec);

    std::string m_name;
    Clock* m_clock;
    bool m_isGm;
    std::vector<Port> m_ports;
    int m_slavePort{-1};           // index of the (single) slave port, -1 if none
    uint16_t m_syncSeq{0};         // GM/bridge: outgoing Sync seq
    uint16_t m_announceSeq{0};      // GM: outgoing Announce seq (independent per 1588)

    // 2-step Sync (S2 closed): slave pending state between receiving the bare
    // Sync (whose arrival time IS syncRxLocal) and its Follow_Up (which carries
    // the origin timestamp + correction field, completing the offset math).
    bool m_syncPending{false};
    uint16_t m_syncPendingSeq{0};
    ns3::Time m_syncRxLocal{0};

    // Per-type logMessageInterval (P3c, informational wire field). Captured at
    // the first SendSync / StartPdelay / SendAnnounce call from the interval the
    // scenario configured (rounded log2 seconds); 0x7F ("unknown") until then.
    // Not read by any handler -- it never affects the servo or offset math.
    int8_t m_logSyncInterval{0x7F};
    int8_t m_logPdelayInterval{0x7F};
    int8_t m_logAnnounceInterval{0x7F};
    // Count of Announce frames received (observability only; never gated).
    uint32_t m_announceRxCount{0};

    // Servo state.
    bool m_haveLastSync{false};
    ns3::Time m_lastSyncGlobal{0};
    uint32_t m_servoCount{0};
    // Bounded integral frequency correction currently applied to the clock
    // (ppm, cumulative via AdjustRate). Clamped to +-kFreqClampPpm.
    double m_freqIntegral{0.0};
    // Inferred nominal inter-Sync interval (s): the minimum inter-Sync gap seen
    // so far. Missed Syncs only lengthen the gap, so the minimum converges to
    // the true Sync period without the interval being passed in. <=0 until the
    // first gap is observed.
    double m_nominalInterval{0.0};

    // Fixed residence delay the bridge waits before regenerating Sync (S1: a
    // real, non-zero processing/queueing time so residence is a measurable,
    // non-trivial duration on the local clock).
    static constexpr double kResidenceDelayUs = 10.0;
    // EtherType for gPTP frames: the real IEEE-assigned PTP EtherType (P3c;
    // was the custom 0x88b6). This is what unmodified tools key their PTPv2
    // dissector on, so a capture is now genuinely dissectable.
    static constexpr uint16_t kGptpProtocol = 0x88F7;
    // Servo gains (P1a hardened PI loop; see the SERVO block in the file header).
    // kPhaseGain (<1): proportional phase step -- the fraction of the measured
    //   offset stepped each Sync. Damped (not the old deadbeat 1.0) so one late
    //   Sync cannot inject a full excursion; still converges in a few cycles.
    // kIntegralGain (<1): fraction of the implied per-cycle rate error folded
    //   into the bounded frequency integral each CLEAN cycle.
    // kFreqClampPpm: symmetric bound on the accumulated frequency integral
    //   (client drifts are within +-200 ppm; this leaves ample headroom while
    //   capping any runaway).
    // kMaxFreqStepPpm: per-cycle bound on how far one Sync can move the frequency
    //   integral. This is the key jitter defense: a Sync delivered late from a
    //   congested queue (up to ~800 us behind a full 10-packet queue) reads a
    //   large phase offset that is NOT a frequency error; capping the per-cycle
    //   step stops that single sample from swinging the frequency estimate, so
    //   opposite-signed queue jitter averages out over a burst instead of ringing.
    // kMissedSyncFactor: a cycle whose elapsed exceeds this multiple of the
    //   inferred nominal Sync interval is a missed-Sync recovery -- phase is
    //   still corrected, but the frequency-integral update is skipped.
    static constexpr double kPhaseGain = 0.7;
    static constexpr double kIntegralGain = 0.4;
    static constexpr double kFreqClampPpm = 500.0;
    static constexpr double kMaxFreqStepPpm = 50.0;
    static constexpr double kMissedSyncFactor = 1.5;

    ns3::TracedCallback<ns3::Time, double> m_offsetTrace;
};

} // namespace syncsim

#endif // SYNCSIM_GPTP_H
