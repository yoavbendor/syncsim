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
//   S2. 1-STEP variants. Pdelay_Resp directly carries the responder's receive
//       (t2) and transmit (t3) timestamps, instead of the real 2-step
//       Pdelay_Resp + Pdelay_Resp_Follow_Up. Likewise Sync carries the precise
//       origin timestamp + correction field directly (1-step Sync), folding in
//       what real 802.1AS splits into Sync + Follow_Up. The information content
//       is identical; only the number of frames differs.
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

#include "ns3/callback.h"
#include "ns3/header.h"
#include "ns3/net-device.h"
#include "ns3/nstime.h"
#include "ns3/traced-callback.h"

#include <cstdint>
#include <string>
#include <vector>

namespace syncsim {

// gPTP message discriminator carried in GptpHeader::m_type.
enum class GptpMsgType : uint8_t
{
    PdelayReq = 0,  // requester -> responder: "when did this reach you / leave you?"
    PdelayResp = 1, // responder -> requester: carries t2 (rx) and t3 (tx) [1-step, S2]
    Sync = 2        // master -> slave: carries preciseOriginTimestamp + correctionField
};

// Minimal on-the-wire gPTP message. A pragmatic custom ns3::Header (per the
// Phase 2 task: no IEEE TLV wire-format fidelity needed for Gate 2). Two int64
// timestamp/field slots are reused per message type:
//   PdelayReq : ta = requester tx time (t1), tb unused
//   PdelayResp: ta = responder rx time (t2), tb = responder tx time (t3)
//   Sync      : ta = preciseOriginTimestamp,  tb = correctionField
// Timestamps are serialized as int64 femtoseconds (lossless for ns-resolution
// Time; ample headroom for a 60 s run).
class GptpHeader : public ns3::Header
{
  public:
    GptpHeader() = default;

    void SetType(GptpMsgType t) { m_type = t; }
    GptpMsgType GetType() const { return m_type; }
    void SetSeq(uint16_t s) { m_seq = s; }
    uint16_t GetSeq() const { return m_seq; }
    void SetTa(ns3::Time t) { m_ta = t.GetFemtoSeconds(); }
    ns3::Time GetTa() const { return ns3::FemtoSeconds(m_ta); }
    void SetTb(ns3::Time t) { m_tb = t.GetFemtoSeconds(); }
    ns3::Time GetTb() const { return ns3::FemtoSeconds(m_tb); }

    static ns3::TypeId GetTypeId();
    ns3::TypeId GetInstanceTypeId() const override;
    void Print(std::ostream& os) const override;
    uint32_t GetSerializedSize() const override;
    void Serialize(ns3::Buffer::Iterator start) const override;
    uint32_t Deserialize(ns3::Buffer::Iterator start) override;

  private:
    GptpMsgType m_type{GptpMsgType::Sync};
    uint16_t m_seq{0};
    int64_t m_ta{0};
    int64_t m_tb{0};
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
        ns3::Address peerMac;
        bool isMaster;
        ns3::Time linkDelay{0};   // measured mean propagation delay (S1: incl. one serialization)
        uint16_t pdelaySeq{0};    // our outstanding Pdelay_Req seq
        ns3::Time pdelayT1{0};    // local tx time of our outstanding Pdelay_Req
        // neighborRateRatio (S3, closed by P2a): neighbor clock rate / local
        // clock rate on this link, estimated from consecutive Pdelay exchanges.
        // 1.0 until the second exchange establishes it. rrPrevT3/rrPrevT4 hold
        // the previous exchange's responder-tx (t3) and our-rx (t4) timestamps.
        double neighborRateRatio{1.0};
        bool haveRrPrev{false};
        ns3::Time rrPrevT3{0};
        ns3::Time rrPrevT4{0};
    };

    void SendFrame(uint32_t portIndex, const GptpHeader& hdr);
    void HandlePdelayReq(uint32_t portIndex, const GptpHeader& in);
    void HandlePdelayResp(uint32_t portIndex, const GptpHeader& in);
    void HandleSync(uint32_t portIndex, const GptpHeader& in);
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
    uint16_t m_syncSeq{0};         // GM: outgoing Sync seq

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
    // Ethertype for our gPTP frames (IEEE 802 local-experimental, same family as
    // Phase 0's smoke test). Not 0x88F7 -- we are not claiming wire fidelity.
    static constexpr uint16_t kGptpProtocol = 0x88b6;
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
