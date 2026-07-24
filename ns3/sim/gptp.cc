// SPDX-License-Identifier: Apache-2.0
//
// syncsim ns-3 migration POC -- Phase 2 (R-GPTP). See gptp.h for the full
// header: clean-room provenance, the licensing caveat, and the simplifications
// (S1 timestamping, S2 2-step framing -- CLOSED by P2b, S3 neighborRateRatio -- CLOSED by P2a,
// S4 per-port termination instead of a transparent bridge).

#include "gptp.h"

#include "ns3/packet.h"
#include "ns3/simulator.h"

#include <algorithm>
#include <cmath>
#include <ostream>

namespace syncsim {

using ns3::Time;

// ============================ free helpers ================================

ClockIdentity
MacToClockIdentity(const ns3::Address& mac)
{
    // EUI-64 from a 6-byte MAC: AA:BB:CC:DD:EE:FF -> AA:BB:CC:FF:FE:DD:EE:FF.
    uint8_t m[6] = {0, 0, 0, 0, 0, 0};
    ns3::Mac48Address::ConvertFrom(mac).CopyTo(m);
    ClockIdentity id{};
    id[0] = m[0];
    id[1] = m[1];
    id[2] = m[2];
    id[3] = 0xFF;
    id[4] = 0xFE;
    id[5] = m[3];
    id[6] = m[4];
    id[7] = m[5];
    return id;
}

ns3::Address
GptpMulticastAddress()
{
    return ns3::Mac48Address("01:80:C2:00:00:0E");
}

namespace {

// Write a PTP Timestamp: secondsField (48-bit BE) + nanosecondsField (32-bit BE),
// from a whole-nanosecond value. Lossless because every Time here is ns-quantized.
void
WriteTimestamp(ns3::Buffer::Iterator& it, int64_t wholeNs)
{
    if (wholeNs < 0)
    {
        wholeNs = 0; // PTP timestamps are unsigned; no negative body ts occurs here
    }
    uint64_t sec = static_cast<uint64_t>(wholeNs) / 1000000000ULL;
    uint32_t nsec = static_cast<uint32_t>(static_cast<uint64_t>(wholeNs) % 1000000000ULL);
    it.WriteHtonU16(static_cast<uint16_t>((sec >> 32) & 0xFFFFULL)); // high 16 of the 48
    it.WriteHtonU32(static_cast<uint32_t>(sec & 0xFFFFFFFFULL));     // low 32 of the 48
    it.WriteHtonU32(nsec);
}

int64_t
ReadTimestamp(ns3::Buffer::Iterator& it)
{
    uint64_t hi = it.ReadNtohU16();
    uint64_t lo = it.ReadNtohU32();
    uint64_t sec = (hi << 32) | lo;
    uint32_t nsec = it.ReadNtohU32();
    return static_cast<int64_t>(sec) * 1000000000LL + static_cast<int64_t>(nsec);
}

void
WriteClockId(ns3::Buffer::Iterator& it, const ClockIdentity& id)
{
    for (uint8_t b : id)
    {
        it.WriteU8(b);
    }
}

ClockIdentity
ReadClockId(ns3::Buffer::Iterator& it)
{
    ClockIdentity id{};
    for (auto& b : id)
    {
        b = it.ReadU8();
    }
    return id;
}

} // namespace

// ============================ GptpHeader ==================================

ns3::TypeId
GptpHeader::GetTypeId()
{
    static ns3::TypeId tid = ns3::TypeId("syncsim::GptpHeader")
                                 .SetParent<ns3::Header>()
                                 .AddConstructor<GptpHeader>();
    return tid;
}

ns3::TypeId
GptpHeader::GetInstanceTypeId() const
{
    return GetTypeId();
}

void
GptpHeader::Print(std::ostream& os) const
{
    os << "msgType=0x" << std::hex << static_cast<uint32_t>(m_type) << std::dec << " seq=" << m_seq
       << " ta=" << m_taNs << "ns corr=" << (m_corr / 65536) << "ns len=" << GetSerializedSize();
}

uint32_t
GptpHeader::GetSerializedSize() const
{
    // 34-byte common PTP header + per-messageType body (+ TLV where present).
    uint32_t body = 0;
    switch (m_type)
    {
    case GptpMsgType::Sync:
        body = 10; // originTimestamp
        break;
    case GptpMsgType::FollowUp:
        body = 10 + 32; // preciseOriginTimestamp + Follow_Up Information TLV
        break;
    case GptpMsgType::PdelayReq:
        body = 20; // originTimestamp + reserved
        break;
    case GptpMsgType::PdelayResp:
    case GptpMsgType::PdelayRespFollowUp:
        body = 20; // {requestReceipt|responseOrigin} ts + requestingPortIdentity
        break;
    case GptpMsgType::Announce:
        body = 30 + 12; // announce body + Path Trace TLV (one ClockIdentity)
        break;
    }
    return 34 + body;
}

void
GptpHeader::Serialize(ns3::Buffer::Iterator start) const
{
    // ---- 34-byte common PTP header ----
    start.WriteU8(static_cast<uint8_t>(0x10 | (static_cast<uint8_t>(m_type) & 0x0F))); // transp|type
    start.WriteU8(0x02);                              // reserved(0) | versionPTP(2)
    start.WriteHtonU16(static_cast<uint16_t>(GetSerializedSize())); // messageLength
    start.WriteU8(0);                                 // domainNumber
    start.WriteU8(0);                                 // reserved
    bool twoStep = (m_type == GptpMsgType::Sync || m_type == GptpMsgType::PdelayResp);
    start.WriteU8(twoStep ? 0x02 : 0x00);             // flagField byte 0 (twoStepFlag)
    start.WriteU8(0);                                 // flagField byte 1
    start.WriteHtonU64(static_cast<uint64_t>(m_corr)); // correctionField (ns * 2^16)
    start.WriteHtonU32(0);                            // reserved (4)
    WriteClockId(start, m_srcClockId);                // sourcePortIdentity.clockIdentity
    start.WriteHtonU16(m_srcPort);                    // sourcePortIdentity.portNumber
    start.WriteHtonU16(m_seq);                        // sequenceId
    uint8_t control = (m_type == GptpMsgType::Sync)       ? 0
                      : (m_type == GptpMsgType::FollowUp) ? 2
                                                          : 5; // v1 legacy controlField
    start.WriteU8(control);
    start.WriteU8(static_cast<uint8_t>(m_logInterval)); // logMessageInterval (Int8)

    // ---- per-messageType body ----
    switch (m_type)
    {
    case GptpMsgType::Sync:
        WriteTimestamp(start, 0); // originTimestamp all-zero (2-step)
        break;
    case GptpMsgType::FollowUp:
        WriteTimestamp(start, m_taNs); // preciseOriginTimestamp
        // Follow_Up Information TLV (ORGANIZATION_EXTENSION, 802.1 subtype 1).
        start.WriteHtonU16(0x0003);    // tlvType = ORGANIZATION_EXTENSION
        start.WriteHtonU16(28);        // lengthField (value bytes that follow)
        start.WriteU8(0x00);           // organizationId = 00-80-C2 (IEEE 802.1 OUI)
        start.WriteU8(0x80);
        start.WriteU8(0xC2);
        start.WriteU8(0x00);           // organizationSubType = 00-00-01
        start.WriteU8(0x00);
        start.WriteU8(0x01);
        start.WriteHtonU32(0);         // cumulativeScaledRateOffset (informational)
        start.WriteHtonU16(0);         // gmTimeBaseIndicator
        start.WriteHtonU64(0);         // lastGmPhaseChange (ScaledNs, high 8 bytes)
        start.WriteHtonU16(0);         //   (ScaledNs, next 2 bytes) -> 10 ns bytes
        start.WriteHtonU16(0);         //   fractional-ns (2 bytes) -> 12 total
        start.WriteHtonU32(0);         // scaledLastGmFreqChange
        break;
    case GptpMsgType::PdelayReq:
        WriteTimestamp(start, 0); // originTimestamp all-zero (2-step)
        WriteTimestamp(start, 0); // reserved 10 bytes (as a zero timestamp)
        break;
    case GptpMsgType::PdelayResp:
        WriteTimestamp(start, m_taNs);        // requestReceiptTimestamp (t2)
        WriteClockId(start, m_reqClockId);    // requestingPortIdentity.clockIdentity
        start.WriteHtonU16(m_reqPort);        // requestingPortIdentity.portNumber
        break;
    case GptpMsgType::PdelayRespFollowUp:
        WriteTimestamp(start, m_taNs);        // responseOriginTimestamp (t3)
        WriteClockId(start, m_reqClockId);    // requestingPortIdentity.clockIdentity
        start.WriteHtonU16(m_reqPort);        // requestingPortIdentity.portNumber
        break;
    case GptpMsgType::Announce:
        WriteTimestamp(start, 0);   // originTimestamp all-zero
        start.WriteHtonU16(37);     // currentUtcOffset (Int16) = 37 (TAI-UTC)
        start.WriteU8(0);           // reserved
        start.WriteU8(128);         // grandmasterPriority1 (1588 default)
        start.WriteU8(248);         // grandmasterClockQuality.clockClass
        start.WriteU8(0xFE);        //   .clockAccuracy (unknown)
        start.WriteHtonU16(0xFFFF); //   .offsetScaledLogVariance (unknown)
        start.WriteU8(128);         // grandmasterPriority2
        WriteClockId(start, m_srcClockId); // grandmasterIdentity (GM's own ClockId)
        start.WriteHtonU16(0);      // stepsRemoved (GM only, no BMCA relay)
        start.WriteU8(0xA0);        // timeSource = internal oscillator
        // Path Trace TLV (PATH_TRACE): one entry, the GM's own ClockIdentity.
        start.WriteHtonU16(0x0008); // tlvType = PATH_TRACE
        start.WriteHtonU16(8);      // lengthField (one ClockIdentity)
        WriteClockId(start, m_srcClockId); // pathSequence[0]
        break;
    }
}

uint32_t
GptpHeader::Deserialize(ns3::Buffer::Iterator start)
{
    // ---- 34-byte common PTP header ----
    uint8_t b0 = start.ReadU8();
    m_type = static_cast<GptpMsgType>(b0 & 0x0F);
    start.ReadU8();          // versionPTP
    start.ReadNtohU16();     // messageLength
    start.ReadU8();          // domainNumber
    start.ReadU8();          // reserved
    start.ReadU8();          // flagField byte 0
    start.ReadU8();          // flagField byte 1
    m_corr = static_cast<int64_t>(start.ReadNtohU64()); // correctionField
    start.ReadNtohU32();     // reserved (4)
    m_srcClockId = ReadClockId(start);
    m_srcPort = start.ReadNtohU16();
    m_seq = start.ReadNtohU16();
    start.ReadU8();          // controlField
    m_logInterval = static_cast<int8_t>(start.ReadU8());

    // ---- per-messageType body: read the fields this project's mechanism uses ----
    // Every type's body opens with a 10-byte timestamp at offset 34; map it to
    // m_taNs (preciseOrigin / requestReceipt t2 / responseOrigin t3; 0 for the
    // rest). PdelayResp/RespFollowUp additionally carry requestingPortIdentity.
    m_taNs = ReadTimestamp(start);
    if (m_type == GptpMsgType::PdelayResp || m_type == GptpMsgType::PdelayRespFollowUp)
    {
        m_reqClockId = ReadClockId(start);
        m_reqPort = start.ReadNtohU16();
    }
    // Trailing fixed/TLV bytes are not read (Peek-only use); size is authoritative.
    return GetSerializedSize();
}

// ============================ GptpEntity ==================================

GptpEntity::GptpEntity(std::string name, Clock* clock, bool isGm)
    : m_name(std::move(name)),
      m_clock(clock),
      m_isGm(isGm)
{
}

uint32_t
GptpEntity::AddPort(ns3::Ptr<ns3::NetDevice> dev, ns3::Address peerMac, bool isMaster)
{
    Port p;
    p.dev = dev;
    p.peerMac = peerMac;
    p.isMaster = isMaster;
    p.clockId = MacToClockIdentity(dev->GetAddress()); // this port's EUI-64 ClockIdentity
    m_ports.push_back(p);
    uint32_t idx = m_ports.size() - 1;
    if (!isMaster)
    {
        m_slavePort = static_cast<int>(idx);
    }
    return idx;
}

void
GptpEntity::ConnectOffsetTrace(ns3::Callback<void, Time, double> cb)
{
    m_offsetTrace.ConnectWithoutContext(cb);
}

Time
GptpEntity::GetLinkDelay(uint32_t portIndex) const
{
    return m_ports.at(portIndex).linkDelay;
}

double
GptpEntity::GetNeighborRateRatio(uint32_t portIndex) const
{
    return m_ports.at(portIndex).neighborRateRatio;
}

void
GptpEntity::SendFrame(uint32_t portIndex, GptpHeader& hdr)
{
    Port& p = m_ports[portIndex];
    // Stamp the sending port's sourcePortIdentity (EUI-64 ClockIdentity + 1-based
    // portNumber) and the per-type logMessageInterval. These are wire fields
    // only; no handler reads them into the servo/offset math.
    hdr.SetSourceIdentity(p.clockId, static_cast<uint16_t>(portIndex + 1));
    switch (hdr.GetType())
    {
    case GptpMsgType::Sync:
    case GptpMsgType::FollowUp:
        hdr.SetLogInterval(m_logSyncInterval);
        break;
    case GptpMsgType::PdelayReq:
        hdr.SetLogInterval(m_logPdelayInterval);
        break;
    case GptpMsgType::Announce:
        hdr.SetLogInterval(m_logAnnounceInterval);
        break;
    case GptpMsgType::PdelayResp:
    case GptpMsgType::PdelayRespFollowUp:
        hdr.SetLogInterval(static_cast<int8_t>(0x7F)); // "unknown" per 802.1AS
        break;
    }
    ns3::Ptr<ns3::Packet> pkt = ns3::Create<ns3::Packet>();
    pkt->AddHeader(hdr);
    // Real gPTP sends every message to the reserved multicast (P3c); p.peerMac is
    // that multicast address (set at each AddPort call site).
    p.dev->Send(pkt, p.peerMac, kGptpProtocol);
}

// ---- Peer delay ----------------------------------------------------------
//
// Requester (this end) at t1 sends Pdelay_Req. Responder records rx time t2 and
// sends a Pdelay_Resp carrying t2, then a Pdelay_Resp_Follow_Up carrying its tx
// time t3 (2-step, S2 closed by P2b). The requester records its rx time t4 of the
// Resp and, on the Follow_Up, computes the mean link delay:
//
//     meanLinkDelay = ((t4 - t1) - (t3 - t2)) / 2
//
// t1,t4 are on the requester's local clock; t2,t3 on the responder's. S3 is now
// closed (P2a): neighborRateRatio is derived from consecutive Pdelay exchanges
// and the responder turnaround (t3-t2) is divided by it to express it in local
// time (see HandlePdelayRespFollowUp). At these few-microsecond durations the
// correction is sub-picosecond, so the measured delay is unchanged to 3+ sig figs.
// Splitting into two frames is informationally identical (S2). Per S1 the timestamps are
// the send-fires / receive-callback-fires instants, so meanLinkDelay includes
// one frame serialization time on top of the channel propagation delay -- a
// real, small, positive, stable value, which is all Gate 2 needs.

void
GptpEntity::StartPdelay(Time pdelayInterval)
{
    // Capture the logMessageInterval for Pdelay_Req wire fields (informational).
    m_logPdelayInterval =
        static_cast<int8_t>(std::lround(std::log2(pdelayInterval.GetSeconds())));
    for (uint32_t i = 0; i < m_ports.size(); ++i)
    {
        Port& p = m_ports[i];
        // Both link ends want the delay: the slave end needs it for its offset
        // reconstruction / bridge correction; the GM's master end initiates too
        // so the gm<->sw link is measured from the sw side (sw is slave there).
        // Concretely: slave ports initiate. (The GM has only a master port and
        // does not need a link delay for anything, so it stays a pure responder.)
        if (p.isMaster)
        {
            continue;
        }
        GptpHeader req;
        req.SetType(GptpMsgType::PdelayReq);
        req.SetSeq(++p.pdelaySeq);
        // t1 is tracked locally by the requester (real 2-step Pdelay_Req carries
        // an all-zero originTimestamp on the wire); no SetTa needed.
        p.pdelayT1 = m_clock->GetLocalTime(); // t1, local
        SendFrame(i, req);
    }
    ns3::Simulator::Schedule(pdelayInterval, &GptpEntity::StartPdelay, this, pdelayInterval);
}

void
GptpEntity::HandlePdelayReq(uint32_t portIndex, const GptpHeader& in)
{
    // We are the responder (2-step, S2 closed). t2 = local rx time (now). Send a
    // Pdelay_Resp carrying ONLY t2, then capture t3 (the egress instant of that
    // Resp on our local clock) and send a Pdelay_Resp_Follow_Up carrying t3. Real
    // 802.1AS splits these so t3 can be a hardware egress timestamp rather than a
    // value predicted before the frame leaves; here the turnaround (t3 - t2) is
    // ~0 and cancels in the peer-delay math either way.
    Time t2 = m_clock->GetLocalTime();
    // Echo the requester's sourcePortIdentity back as requestingPortIdentity --
    // read out of the Pdelay_Req we are replying to (a real, meaningful field).
    ClockIdentity reqId = in.GetSourceClockId();
    uint16_t reqPort = in.GetSourcePortNumber();
    GptpHeader resp;
    resp.SetType(GptpMsgType::PdelayResp);
    resp.SetSeq(in.GetSeq());
    resp.SetTa(t2); // requestReceiptTimestamp (t2)
    resp.SetRequestingIdentity(reqId, reqPort);
    Time t3 = m_clock->GetLocalTime(); // egress timestamp of the Pdelay_Resp
    SendFrame(portIndex, resp);
    GptpHeader fu;
    fu.SetType(GptpMsgType::PdelayRespFollowUp);
    fu.SetSeq(in.GetSeq());
    fu.SetTa(t3); // responseOriginTimestamp (t3)
    fu.SetRequestingIdentity(reqId, reqPort);
    SendFrame(portIndex, fu);
}

void
GptpEntity::HandlePdelayResp(uint32_t portIndex, const GptpHeader& in)
{
    // Phase 1 of the 2-step exchange (S2 closed): the Pdelay_Resp carries only t2.
    // Record t4 (our rx of the Resp) and t2, and mark the exchange pending; the
    // peer-delay math is completed in HandlePdelayRespFollowUp when t3 arrives.
    Port& p = m_ports[portIndex];
    if (in.GetSeq() != p.pdelaySeq)
    {
        return; // stale / mismatched response
    }
    p.pdelayT4 = m_clock->GetLocalTime(); // t4 = rx of the Pdelay_Resp
    p.pdelayT2 = in.GetTa();              // t2 (responder rx)
    p.pdelayRespSeen = true;
}

void
GptpEntity::HandlePdelayRespFollowUp(uint32_t portIndex, const GptpHeader& in)
{
    // Phase 2 of the 2-step exchange: the Pdelay_Resp_Follow_Up carries t3. It
    // must match the seq of a Pdelay_Resp we already saw for this port.
    Port& p = m_ports[portIndex];
    if (in.GetSeq() != p.pdelaySeq || !p.pdelayRespSeen)
    {
        return; // stale, or Follow_Up without its Resp (e.g. Resp was dropped)
    }
    p.pdelayRespSeen = false;
    Time t4 = p.pdelayT4;
    Time t1 = p.pdelayT1;
    Time t2 = p.pdelayT2;
    Time t3 = in.GetTa();

    // neighborRateRatio (S3, closed by P2a). Between two successive Pdelay
    // exchanges, compare how far the NEIGHBOR's clock advanced (its reported tx
    // timestamp t3) against how far OUR clock advanced (our rx timestamp t4):
    //   neighborRateRatio = (t3_now - t3_prev) / (t4_now - t4_prev)
    // i.e. neighbor-elapsed / local-elapsed = neighbor rate relative to ours. A
    // guard rejects physically-implausible ratios (>1% off unity), which also
    // rejects a t4 inflated by the M3 shared-medium contention artifact (that
    // balloons local-elapsed and drags the ratio far from 1) -- the same class of
    // outlier the running-minimum peer-delay filter below rejects.
    if (p.haveRrPrev)
    {
        double localElapsed = (t4 - p.rrPrevT4).GetSeconds();
        double neighElapsed = (t3 - p.rrPrevT3).GetSeconds();
        if (localElapsed > 0.0 && neighElapsed > 0.0)
        {
            double rr = neighElapsed / localElapsed;
            if (rr > 0.99 && rr < 1.01)
            {
                p.neighborRateRatio = rr;
            }
        }
    }
    p.rrPrevT3 = t3;
    p.rrPrevT4 = t4;
    p.haveRrPrev = true;

    // meanLinkDelay = ((t4 - t1) - (t3 - t2)/neighborRateRatio) / 2.
    // The responder turnaround (t3 - t2) is measured on the NEIGHBOR's clock, so
    // divide by neighborRateRatio to express it in our local time base before
    // subtracting it from the local round trip (t4 - t1). With S3 closed this is
    // the rate-consistent peer delay; at these drift/timescales the correction is
    // sub-picosecond (P2a: no observed number moved), and the responder turnaround
    // (t3 - t2) is ~0 anyway, so the fold is exact and vanishingly small here.
    double rttLocal = (t4 - t1).GetSeconds();
    double turnaround = (t3 - t2).GetSeconds() / p.neighborRateRatio;
    Time cand = ns3::Seconds((rttLocal - turnaround) / 2.0);

    // Peer-delay OUTLIER FILTER (P1a). The true link delay is a physical FLOOR
    // (propagation + one serialization, S1) and static for a given link;
    // contention only ever ADDS to a measured round trip. Under M3's congestion
    // the Pdelay handshake frames contend on the saturated shared CSMA medium
    // (the S5 shared-medium artifact reaching the peer-delay path), inflating a
    // single measured `cand` to tens of milliseconds -- ~1000x the real ~few-us
    // delay. A corrupted peer delay feeds straight into HandleSync's
    // reconstructed GM time and injects a false offset that ANY servo would
    // faithfully (and wrongly) chase, which is what dominated M3's congested
    // peak once the servo loop itself was hardened. Because the true delay is a
    // floor, a running MINIMUM of the positive samples is a robust,
    // self-calibrating estimator immune to congestion inflation: the ~20 clean
    // pre-congestion Pdelay exchanges establish it, and later contention-inflated
    // samples never lower it. Non-positive candidates (a transient from a servo
    // phase step landing mid-handshake) are rejected outright.
    if (cand.GetSeconds() > 0.0)
    {
        if (p.linkDelay == Time(0) || cand < p.linkDelay)
        {
            p.linkDelay = cand;
        }
    }
}

// ---- Sync / Follow_Up with residence-time correction ---------------------

void
GptpEntity::SendSync(Time syncInterval)
{
    // GM only. GM is drift-free by construction, so its local time IS global sim
    // time (the same trick simdata.py's parse_offset_series relies on: offset =
    // clock_time - event_time is valid only because the GM's driftRate is 0).
    m_logSyncInterval = static_cast<int8_t>(std::lround(std::log2(syncInterval.GetSeconds())));
    uint16_t seq = ++m_syncSeq;
    for (uint32_t i = 0; i < m_ports.size(); ++i)
    {
        if (!m_ports[i].isMaster)
        {
            continue;
        }
        // 2-step (S2 closed): a bare Sync marker, immediately followed by a
        // Follow_Up carrying the preciseOriginTimestamp (captured at Sync egress)
        // and correctionField = 0 at the source. The slave's syncRxLocal is the
        // bare Sync's arrival instant; the Follow_Up only supplies the numbers.
        GptpHeader sync;
        sync.SetType(GptpMsgType::Sync);
        sync.SetSeq(seq);
        Time originTs = m_clock->GetLocalTime(); // preciseOriginTimestamp at Sync egress
        SendFrame(i, sync);
        GptpHeader fu;
        fu.SetType(GptpMsgType::FollowUp);
        fu.SetSeq(seq);
        fu.SetTa(originTs);  // preciseOriginTimestamp
        fu.SetTb(Time(0));   // correctionField = 0 at the source
        SendFrame(i, fu);
    }
    ns3::Simulator::Schedule(syncInterval, &GptpEntity::SendSync, this, syncInterval);
}

void
GptpEntity::SendAnnounce(Time announceInterval)
{
    // GM only, additive (P3c). Emits a static Announce on every master port for
    // wire-realism/dissectability. No receiver consumes it (this project has no
    // dynamic BMCA -- the GM is fixed by construction). Scheduled on the same
    // cadence as SendSync; ordered AFTER the Sync/Follow_Up burst by the
    // scenario so it never delays a measured frame's serialization (verified: no
    // gate number moves). All Announce content is fixed/static; the only
    // per-node value is the GM's own ClockIdentity, stamped in SendFrame.
    m_logAnnounceInterval =
        static_cast<int8_t>(std::lround(std::log2(announceInterval.GetSeconds())));
    uint16_t seq = ++m_announceSeq;
    for (uint32_t i = 0; i < m_ports.size(); ++i)
    {
        if (!m_ports[i].isMaster)
        {
            continue;
        }
        GptpHeader ann;
        ann.SetType(GptpMsgType::Announce);
        ann.SetSeq(seq);
        SendFrame(i, ann);
    }
    ns3::Simulator::Schedule(announceInterval, &GptpEntity::SendAnnounce, this, announceInterval);
}

void
GptpEntity::HandleAnnounce(uint32_t /*portIndex*/, const GptpHeader& /*in*/)
{
    // Observability only: count receipts. Announce is NOT consumed by the servo
    // or offset math in any way (no dynamic BMCA in this project's static-GM
    // model), so this cannot perturb any gate.
    ++m_announceRxCount;
}

void
GptpEntity::HandleSync(uint32_t portIndex, const GptpHeader& in)
{
    // 2-step (S2 closed): the Sync is a bare marker. It must arrive on our slave
    // port to be meaningful. Record its arrival instant (syncRxLocal) and seq,
    // and mark a Sync pending; the offset math + servo + relay all happen in
    // HandleFollowUp when the matching Follow_Up arrives. A new Sync before its
    // Follow_Up simply overwrites the pending slot -- so if a Follow_Up is lost,
    // the next Sync cleanly supersedes it (that cycle is just skipped).
    if (static_cast<int>(portIndex) != m_slavePort)
    {
        return;
    }
    m_syncRxLocal = m_clock->GetLocalTime(); // local time at Sync receipt
    m_syncPendingSeq = in.GetSeq();
    m_syncPending = true;
}

void
GptpEntity::HandleFollowUp(uint32_t portIndex, const GptpHeader& in)
{
    // 2-step (S2 closed): the Follow_Up carries the numbers the bare Sync did not.
    // It must arrive on the slave port and match the pending Sync's seq.
    if (static_cast<int>(portIndex) != m_slavePort || !m_syncPending ||
        in.GetSeq() != m_syncPendingSeq)
    {
        return;
    }
    m_syncPending = false;
    Time recvLocal = m_syncRxLocal;        // the bare Sync's arrival (NOT the Follow_Up's)
    Time originTs = in.GetTa();            // GM's precise origin ts
    Time upstreamCorr = in.GetTb();        // accumulated correction
    Time d = m_ports[portIndex].linkDelay; // our measured peer delay

    // Reconstructed "GM time right now" at the instant the Sync reached us:
    //   gmTime = originTimestamp + correctionField + thisLinkPeerDelay
    Time gmTime = originTs + upstreamCorr + d;
    double offsetSec = (recvLocal - gmTime).GetSeconds(); // local - GM = offset-from-GM

    if (m_ports.size() == 1)
    {
        // Pure end station: no downstream to relay. Servo immediately.
        m_offsetTrace(ns3::Simulator::Now(), offsetSec);
        ApplyServo(offsetSec);
        return;
    }

    // Time-aware bridge (sw): regenerate Sync downstream after the residence
    // time, THEN servo our own clock. Servo is applied after the relay so the
    // phase step cannot corrupt the residence duration (which is read off the
    // local clock across the residence window). The relay itself is independent
    // of our absolute clock offset -- it forwards only durations (peer delay +
    // residence) + the GM's origin timestamp, so downstream reconstruction does
    // not depend on how well *we* are synced (proven in gptp-spike.cc's notes).
    // Residence is measured from the bare Sync's arrival (recvLocal), so it
    // naturally absorbs the small Sync->Follow_Up gap; the correction-field math
    // self-compensates, leaving downstream offsets unchanged vs the 1-step form.
    ns3::Simulator::Schedule(ns3::MicroSeconds(kResidenceDelayUs),
                             &GptpEntity::RelayDownstream,
                             this,
                             originTs,
                             upstreamCorr,
                             recvLocal,
                             d,
                             offsetSec);
}

void
GptpEntity::RelayDownstream(Time originTs,
                            Time upstreamCorrection,
                            Time slaveRecvLocal,
                            Time slaveLinkDelay,
                            double offsetSecForServo)
{
    Time sendLocal = m_clock->GetLocalTime();
    Time residence = sendLocal - slaveRecvLocal; // local elapsed since Sync receipt
    // correctionField accumulates: upstream correction + this link's peer delay
    // + our residence time. (S3 closed by P2a: the residence is measured on OUR
    // local clock, so scale it by the slave port's neighborRateRatio -- our
    // upstream neighbor's rate relative to ours, the best available proxy for the
    // rate toward GM -- to express it in the upstream/GM time base before folding
    // it in. At these timescales this is a sub-picosecond adjustment.)
    double rr = (m_slavePort >= 0) ? m_ports[m_slavePort].neighborRateRatio : 1.0;
    Time residenceCorrected = ns3::Seconds(residence.GetSeconds() * rr);
    Time newCorr = upstreamCorrection + slaveLinkDelay + residenceCorrected;
    // 2-step (S2 closed): regenerate a bare Sync + a Follow_Up carrying the
    // GM origin timestamp and the accumulated correction on every master port.
    // Both share a fresh seq so downstream slaves can pair them.
    uint16_t relaySeq = ++m_syncSeq;
    for (uint32_t i = 0; i < m_ports.size(); ++i)
    {
        if (!m_ports[i].isMaster)
        {
            continue;
        }
        GptpHeader sync;
        sync.SetType(GptpMsgType::Sync);
        sync.SetSeq(relaySeq);
        SendFrame(i, sync);
        GptpHeader fu;
        fu.SetType(GptpMsgType::FollowUp);
        fu.SetSeq(relaySeq);
        fu.SetTa(originTs);
        fu.SetTb(newCorr);
        SendFrame(i, fu);
    }
    // Now servo our own clock (bridge is a slave toward the GM too).
    m_offsetTrace(ns3::Simulator::Now(), offsetSecForServo);
    ApplyServo(offsetSecForServo);
}

// ---- Servo (P1a hardened PI loop) ----------------------------------------
//
// Proportional (damped) phase step + BOUNDED integral frequency correction,
// steering the Phase-1 Clock via AdjustOffset / AdjustRate, with missed-Sync
// outlier handling. See the SERVO block in gptp.h for the full rationale; this
// reimplements the public linuxptp PI control-loop idea (no ptp4l source read).
//
// Why this replaces the Phase-2 deadbeat+`offset/elapsed` servo: that servo
// re-derived a fresh, unbounded rate estimate from a single inter-Sync interval
// every cycle and applied the whole measured offset as a phase step. When a Sync
// was dropped/delayed in a congested queue, `elapsed` ballooned and that estimate
// went wild, and the deadbeat phase rang on it -- the sole cause of M3's 46 ms
// congested peak. The hardened loop (a) normalizes the frequency term by the
// inferred NOMINAL interval, not the actual elapsed, and bounds its accumulation;
// (b) skips the frequency update entirely on a missed-Sync-length cycle; and
// (c) damps the phase step so no single sample causes a full excursion.

void
GptpEntity::ApplyServo(double offsetSec)
{
    Time nowG = ns3::Simulator::Now();

    // Inter-Sync gap on the GLOBAL timeline. A dropped Sync makes this balloon to
    // a multiple of the nominal interval -- the missed-Sync signature.
    double elapsed = m_haveLastSync ? (nowG - m_lastSyncGlobal).GetSeconds() : 0.0;

    // Infer the nominal Sync interval as the shortest gap seen so far (missed
    // Syncs only lengthen gaps), then flag anomalously long ones.
    bool anomalous = false;
    if (m_haveLastSync && elapsed > 0.0)
    {
        if (m_nominalInterval <= 0.0 || elapsed < m_nominalInterval)
        {
            m_nominalInterval = elapsed;
        }
        anomalous = elapsed > kMissedSyncFactor * m_nominalInterval;
    }

    // Integral frequency term: only updated on a CLEAN cycle. Normalize the
    // implied per-cycle rate error by the NOMINAL interval (constant), never the
    // actual elapsed, so a ballooned gap cannot scale it; accumulate a low-pass
    // fraction into a persistent integral, clamped so it can never run away. The
    // achieved increment (post-clamp) is what we push onto the clock via the
    // incremental AdjustRate, so m_freqIntegral mirrors the applied correction.
    if (m_haveLastSync && !anomalous && m_nominalInterval > 0.0)
    {
        double impliedPpm = (offsetSec / m_nominalInterval) * 1e6;
        double step = -kIntegralGain * impliedPpm;
        // Per-cycle step clamp: one late-from-a-congested-queue Sync reads a big
        // phase offset that is not a frequency error, so cap how far a single
        // sample can move the integral (opposite-signed jitter then averages out).
        step = std::clamp(step, -kMaxFreqStepPpm, kMaxFreqStepPpm);
        double target = std::clamp(m_freqIntegral + step, -kFreqClampPpm, kFreqClampPpm);
        double achievedInc = target - m_freqIntegral;
        m_clock->AdjustRate(achievedInc);
        m_freqIntegral = target;
    }

    // Proportional phase term: step a damped fraction of the measured offset back
    // onto the reconstructed GM time (deadbeat = 1.0 would amplify one late Sync
    // into a full excursion; kPhaseGain < 1 rejects a single bad sample while
    // still converging in a few clean cycles).
    m_clock->AdjustOffset(ns3::Seconds(-kPhaseGain * offsetSec));

    m_lastSyncGlobal = nowG;
    m_haveLastSync = true;
    ++m_servoCount;
}

// ---- Receive trampoline --------------------------------------------------

bool
GptpEntity::OnDeviceReceive(uint32_t portIndex, ns3::Ptr<const ns3::Packet> packet, const ns3::Address&)
{
    GptpHeader hdr;
    packet->PeekHeader(hdr);
    switch (hdr.GetType())
    {
    case GptpMsgType::PdelayReq:
        HandlePdelayReq(portIndex, hdr);
        break;
    case GptpMsgType::PdelayResp:
        HandlePdelayResp(portIndex, hdr);
        break;
    case GptpMsgType::PdelayRespFollowUp:
        HandlePdelayRespFollowUp(portIndex, hdr);
        break;
    case GptpMsgType::Sync:
        HandleSync(portIndex, hdr);
        break;
    case GptpMsgType::FollowUp:
        HandleFollowUp(portIndex, hdr);
        break;
    case GptpMsgType::Announce:
        HandleAnnounce(portIndex, hdr);
        break;
    }
    return true;
}

} // namespace syncsim
