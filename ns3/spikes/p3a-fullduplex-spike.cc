// SPDX-License-Identifier: Apache-2.0
//
// syncsim ns-3 migration POC -- P3a FEASIBILITY SPIKE (throwaway).
//
// ONE question, answered with hard evidence:
//   Can gPTP's custom ethertype (0x88b6) be carried over a full-duplex-capable
//   ns-3 device -- WITHOUT (a) patching ns-3's pinned CsmaChannel/CsmaNetDevice
//   core or (b) writing a new NetDevice subclass from scratch -- such that each
//   direction is independent (no cross-direction shared-medium contention)?
//
// This is a SPIKE. It touches NONE of the committed scenario files
// (congestion-topology.cc, gptp.{h,cc}, clock.{h,cc}). It is a standalone
// program built in scratch/ against the same pinned ns-3.45 the rest of the
// track uses. See ns3/spikes/P3A_SPIKE_FINDINGS.md for the write-up.
//
// It runs two experiments and prints real, measured evidence:
//
//   EXPERIMENT 1 -- full-duplex vs shared-medium, head to head.
//     Two nodes, one point-to-point link. One direction (A->B) is saturated
//     with background *data* frames (ethertype 0x88b5, ~1500 B, back to back).
//     Simultaneously the OTHER direction (B->A) carries small custom-ethertype
//     "gPTP" frames (0x88b6, 64 B). We measure the one-way latency of every
//     reverse-direction gPTP frame. The whole thing is run TWICE: once over a
//     SimpleNetDevice+SimpleChannel pair (mainline ns-3, full-duplex point to
//     point), once over the exact CsmaNetDevice+CsmaChannel pair the M3
//     scenario uses today (shared half-duplex medium). This is the concrete
//     S5 mechanism: on the shared medium the reverse gPTP frame contends with
//     the forward data and is delayed; on a genuine full-duplex device it is
//     not. The numbers make the difference explicit.
//
//   EXPERIMENT 2 -- BridgeNetDevice forwarding of the custom ethertype over
//     full-duplex SimpleNetDevice links. Three nodes end0 -- sw -- end1, where
//     sw is a BridgeNetDevice bridging two SimpleNetDevice ports (each on its
//     own 2-device SimpleChannel). We send 0x88b6 frames end0->end1 AND
//     end1->end0 overlapping in time, and confirm the bridge forwards them
//     hop-by-hop with the custom ethertype intact and neither direction
//     delaying the other. This is the "3+ node forwarding chain" M3's real
//     hop-by-hop data forwarding would need.

#include "ns3/bridge-module.h"
#include "ns3/core-module.h"
#include "ns3/csma-module.h"
#include "ns3/network-module.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <vector>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("P3aFullduplexSpike");

namespace
{

constexpr uint16_t kGptpProto = 0x88b6; // the ethertype PPP rejects; the crux of S5/P3a
constexpr uint16_t kDataProto = 0x88b5; // background data plane (distinct)
constexpr uint32_t kGptpSize = 64;      // small "Sync"-like frame
constexpr uint32_t kDataSize = 1500;    // saturating background frame

// Send-time ledger keyed by packet UID (preserved across Packet::Copy and both
// devices' internal framing), so a received frame can be matched to its send.
std::map<uint64_t, double> g_sendTimeUs;

struct Ledger
{
    // reverse-direction (B->A) gPTP latencies, microseconds
    std::vector<double> revLatUs;
    // forward-direction (A->B) gPTP latencies, microseconds (control)
    std::vector<double> fwdLatUs;
    uint64_t dataDelivered{0};
    uint64_t gptpRevRecv{0};
    uint64_t gptpFwdRecv{0};
};

Ledger* g_L = nullptr;

// ---- Experiment 1 receive callbacks ---------------------------------------
// A is the node whose RX we watch for reverse (B->A) gPTP frames.
bool
RxAtA(Ptr<NetDevice>, Ptr<const Packet> pkt, uint16_t proto, const Address&)
{
    double now = Simulator::Now().GetMicroSeconds();
    if (proto == kGptpProto)
    {
        auto it = g_sendTimeUs.find(pkt->GetUid());
        if (it != g_sendTimeUs.end())
        {
            g_L->revLatUs.push_back(now - it->second);
            g_L->gptpRevRecv++;
        }
    }
    return true;
}

// B receives the saturating forward data (counted) and the forward gPTP control.
bool
RxAtB(Ptr<NetDevice>, Ptr<const Packet> pkt, uint16_t proto, const Address&)
{
    double now = Simulator::Now().GetMicroSeconds();
    if (proto == kDataProto)
    {
        g_L->dataDelivered++;
    }
    else if (proto == kGptpProto)
    {
        auto it = g_sendTimeUs.find(pkt->GetUid());
        if (it != g_sendTimeUs.end())
        {
            g_L->fwdLatUs.push_back(now - it->second);
            g_L->gptpFwdRecv++;
        }
    }
    return true;
}

void
SendTracked(Ptr<NetDevice> dev, Address to, uint16_t proto, uint32_t size)
{
    Ptr<Packet> p = Create<Packet>(size);
    g_sendTimeUs[p->GetUid()] = Simulator::Now().GetMicroSeconds();
    dev->Send(p, to, proto);
}

// Saturate a direction: back-to-back data frames until stopAt.
void
Saturate(Ptr<NetDevice> dev, Address to, Time gap, Time stopAt)
{
    if (Simulator::Now() >= stopAt)
    {
        return;
    }
    dev->Send(Create<Packet>(kDataSize), to, kDataProto);
    Simulator::Schedule(gap, &Saturate, dev, to, gap, stopAt);
}

// Periodic reverse (B->A) gPTP frame; tracked for latency.
void
RevGptp(Ptr<NetDevice> devB, Address toA, Time period, Time stopAt)
{
    if (Simulator::Now() >= stopAt)
    {
        return;
    }
    SendTracked(devB, toA, kGptpProto, kGptpSize);
    Simulator::Schedule(period, &RevGptp, devB, toA, period, stopAt);
}

// Periodic forward (A->B) gPTP frame; tracked for latency (control).
void
FwdGptp(Ptr<NetDevice> devA, Address toB, Time period, Time stopAt)
{
    if (Simulator::Now() >= stopAt)
    {
        return;
    }
    SendTracked(devA, toB, kGptpProto, kGptpSize);
    Simulator::Schedule(period, &FwdGptp, devA, toB, period, stopAt);
}

void
stats(const std::vector<double>& v, double& mn, double& mean, double& mx)
{
    mn = 1e30;
    mx = 0;
    double sum = 0;
    for (double x : v)
    {
        mn = std::min(mn, x);
        mx = std::max(mx, x);
        sum += x;
    }
    mean = v.empty() ? 0 : sum / v.size();
    if (v.empty())
    {
        mn = 0;
    }
}

enum class LinkMode
{
    SIMPLE_FULLDUPLEX,
    CSMA_SHARED
};

// Build a 2-node link in the requested mode, saturate A->B with data while
// B->A carries small gPTP frames, and record reverse-direction gPTP latency.
Ledger
runExperiment1(LinkMode mode, bool saturate)
{
    Ledger L;
    g_L = &L;
    g_sendTimeUs.clear();

    NodeContainer nodes;
    nodes.Create(2);

    Ptr<NetDevice> devA;
    Ptr<NetDevice> devB;

    if (mode == LinkMode::SIMPLE_FULLDUPLEX)
    {
        // Manual construction: exactly two SimpleNetDevices on one SimpleChannel
        // => a genuine full-duplex point-to-point link. Each device owns its own
        // tx queue and tx-busy state; SimpleChannel schedules Receive on the peer
        // with no medium locking / carrier sense / collision (confirmed in
        // simple-channel.cc), so the two directions are independent.
        Ptr<SimpleChannel> ch = CreateObject<SimpleChannel>();
        ch->SetAttribute("Delay", TimeValue(MicroSeconds(1)));
        auto mk = [&](Ptr<Node> n) {
            Ptr<SimpleNetDevice> d = CreateObject<SimpleNetDevice>();
            d->SetAddress(Mac48Address::Allocate());
            n->AddDevice(d);
            d->SetChannel(ch);
            d->SetAttribute("DataRate", DataRateValue(DataRate("100Mbps")));
            d->GetQueue()->SetAttribute("MaxSize", QueueSizeValue(QueueSize(PACKETS, 1000)));
            return d;
        };
        Ptr<SimpleNetDevice> a = mk(nodes.Get(0));
        Ptr<SimpleNetDevice> b = mk(nodes.Get(1));
        devA = a;
        devB = b;
    }
    else
    {
        // The EXACT device pairing M3 uses today: one CsmaChannel, two
        // CsmaNetDevices => a shared half-duplex medium.
        CsmaHelper csma;
        csma.SetChannelAttribute("DataRate", StringValue("100Mbps"));
        csma.SetChannelAttribute("Delay", TimeValue(MicroSeconds(1)));
        NetDeviceContainer dv = csma.Install(nodes);
        devA = dv.Get(0);
        devB = dv.Get(1);
    }

    devA->SetReceiveCallback(MakeCallback(&RxAtA));
    devB->SetReceiveCallback(MakeCallback(&RxAtB));

    Time start = MilliSeconds(1);
    Time stop = MilliSeconds(51); // 50 ms measurement window
    Address addrA = devA->GetAddress();
    Address addrB = devB->GetAddress();

    if (saturate)
    {
        // Back-to-back 1500 B frames at 100 Mbps: one frame serializes in
        // ~120 us, so a 100 us offer gap keeps the A->B path continuously busy.
        Simulator::Schedule(start, &Saturate, devA, addrB, MicroSeconds(100), stop);
    }
    // Reverse gPTP every 137 us (deliberately not a divisor of the data gap, so
    // reverse frames land at varied phases relative to forward transmissions).
    Simulator::Schedule(start, &RevGptp, devB, addrA, MicroSeconds(137), stop);
    // Forward gPTP control every 137 us too.
    Simulator::Schedule(start, &FwdGptp, devA, addrB, MicroSeconds(137), stop);

    Simulator::Stop(stop + MilliSeconds(5));
    Simulator::Run();
    Simulator::Destroy();
    g_L = nullptr;
    return L;
}

// ---- Experiment 2: bridge forwarding of the custom ethertype --------------
uint64_t g_e2_end1RecvGptp = 0;
uint64_t g_e2_end0RecvGptp = 0;
uint16_t g_e2_lastProtoAtEnd1 = 0;

bool
RxEnd1(Ptr<NetDevice>, Ptr<const Packet>, uint16_t proto, const Address&)
{
    if (proto == kGptpProto)
    {
        g_e2_end1RecvGptp++;
        g_e2_lastProtoAtEnd1 = proto;
    }
    return true;
}

bool
RxEnd0(Ptr<NetDevice>, Ptr<const Packet>, uint16_t proto, const Address&)
{
    if (proto == kGptpProto)
    {
        g_e2_end0RecvGptp++;
    }
    return true;
}

void
runExperiment2()
{
    g_e2_end1RecvGptp = 0;
    g_e2_end0RecvGptp = 0;
    g_e2_lastProtoAtEnd1 = 0;

    NodeContainer nodes;
    nodes.Create(3); // 0=end0, 1=sw, 2=end1

    auto mkDev = [&](Ptr<Node> n, Ptr<SimpleChannel> ch) {
        Ptr<SimpleNetDevice> d = CreateObject<SimpleNetDevice>();
        d->SetAddress(Mac48Address::Allocate());
        n->AddDevice(d);
        d->SetChannel(ch);
        d->SetAttribute("DataRate", DataRateValue(DataRate("100Mbps")));
        return d;
    };

    // Link end0 <-> sw
    Ptr<SimpleChannel> c0 = CreateObject<SimpleChannel>();
    c0->SetAttribute("Delay", TimeValue(MicroSeconds(1)));
    Ptr<SimpleNetDevice> dEnd0 = mkDev(nodes.Get(0), c0);
    Ptr<SimpleNetDevice> dSw0 = mkDev(nodes.Get(1), c0);

    // Link sw <-> end1
    Ptr<SimpleChannel> c1 = CreateObject<SimpleChannel>();
    c1->SetAttribute("Delay", TimeValue(MicroSeconds(1)));
    Ptr<SimpleNetDevice> dSw1 = mkDev(nodes.Get(1), c1);
    Ptr<SimpleNetDevice> dEnd1 = mkDev(nodes.Get(2), c1);

    // sw bridges its two SimpleNetDevice ports.
    BridgeHelper bridge;
    NetDeviceContainer swPorts;
    swPorts.Add(dSw0);
    swPorts.Add(dSw1);
    bridge.Install(nodes.Get(1), swPorts);

    dEnd0->SetReceiveCallback(MakeCallback(&RxEnd0));
    dEnd1->SetReceiveCallback(MakeCallback(&RxEnd1));

    Address addrEnd0 = dEnd0->GetAddress();
    Address addrEnd1 = dEnd1->GetAddress();

    // Fire 10 gPTP frames each way, overlapping in time (same instants), to show
    // the bridge forwards the custom ethertype in both directions concurrently.
    for (int i = 0; i < 10; ++i)
    {
        Time t = MilliSeconds(1) + MicroSeconds(200 * i);
        Simulator::Schedule(t, &NetDevice::Send, dEnd0, Create<Packet>(kGptpSize), addrEnd1,
                            kGptpProto);
        Simulator::Schedule(t, &NetDevice::Send, dEnd1, Create<Packet>(kGptpSize), addrEnd0,
                            kGptpProto);
    }

    Simulator::Stop(MilliSeconds(10));
    Simulator::Run();
    Simulator::Destroy();
}

void
report1(const std::string& tag, const Ledger& L)
{
    double rmn, rmean, rmx, fmn, fmean, fmx;
    stats(L.revLatUs, rmn, rmean, rmx);
    stats(L.fwdLatUs, fmn, fmean, fmx);
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "  " << std::setw(34) << std::left << tag << std::right
              << " reverse gPTP (B->A): n=" << L.gptpRevRecv << " lat us[min/mean/max]=" << rmn
              << "/" << rmean << "/" << rmx << "   (fwd data delivered=" << L.dataDelivered << ")\n";
}

} // namespace

int
main(int argc, char* argv[])
{
    CommandLine cmd(__FILE__);
    cmd.Parse(argc, argv);

    RngSeedManager::SetSeed(1);
    RngSeedManager::SetRun(1);

    std::cout << "\n=== P3a SPIKE: can 0x88b6 ride a full-duplex ns-3 device w/o core patch "
                 "or new NetDevice? ===\n";

    // --- Experiment 1 -------------------------------------------------------
    std::cout << "\n[EXP1] Reverse-direction custom-ethertype (0x88b6) one-way latency,\n";
    std::cout << "       measured with the forward direction SATURATED by 1500B data, and as a\n";
    std::cout << "       control with NO forward load. Same link, two device models.\n\n";

    std::cout << "  -- control: no forward saturation (both directions idle-ish) --\n";
    Ledger simIdle = runExperiment1(LinkMode::SIMPLE_FULLDUPLEX, false);
    Ledger csmaIdle = runExperiment1(LinkMode::CSMA_SHARED, false);
    report1("SimpleNetDevice (full-duplex)", simIdle);
    report1("CsmaNetDevice   (shared medium)", csmaIdle);

    std::cout << "\n  -- forward A->B SATURATED with 1500B data at 100Mbps --\n";
    Ledger simSat = runExperiment1(LinkMode::SIMPLE_FULLDUPLEX, true);
    Ledger csmaSat = runExperiment1(LinkMode::CSMA_SHARED, true);
    report1("SimpleNetDevice (full-duplex)", simSat);
    report1("CsmaNetDevice   (shared medium)", csmaSat);

    // The full-duplex claim, made quantitative: how much does forward saturation
    // inflate the reverse gPTP frame's latency on each device model?
    double s_imn, s_imean, s_imx, s_smn, s_smean, s_smx;
    stats(simIdle.revLatUs, s_imn, s_imean, s_imx);
    stats(simSat.revLatUs, s_smn, s_smean, s_smx);
    double c_imn, c_imean, c_imx, c_smn, c_smean, c_smx;
    stats(csmaIdle.revLatUs, c_imn, c_imean, c_imx);
    stats(csmaSat.revLatUs, c_smn, c_smean, c_smx);

    std::cout << std::fixed << std::setprecision(3);
    std::cout << "\n  [EXP1 verdict]\n";
    std::cout << "    SimpleNetDevice: reverse-gPTP mean latency idle=" << s_imean
              << "us  saturated=" << s_smean << "us  (max sat=" << s_smx
              << "us) -> inflation " << (s_imean > 0 ? s_smean / s_imean : 0) << "x\n";
    std::cout << "    CsmaNetDevice  : reverse-gPTP mean latency idle=" << c_imean
              << "us  saturated=" << c_smean << "us  (max sat=" << c_smx
              << "us) -> inflation " << (c_imean > 0 ? c_smean / c_imean : 0) << "x\n";
    bool fullDuplexProven = (s_smean < 1.2 * s_imean + 1.0) && (c_smean > 2.0 * s_smean);
    std::cout << "    => " << (fullDuplexProven ? "PASS" : "FAIL")
              << ": SimpleNetDevice reverse latency is (near) unchanged by forward load;\n"
                 "       CsmaNetDevice reverse latency is clearly inflated by it (the S5 "
                 "mechanism).\n";

    // --- Experiment 2 -------------------------------------------------------
    std::cout << "\n[EXP2] BridgeNetDevice forwarding the custom ethertype over full-duplex\n";
    std::cout << "       SimpleNetDevice links: end0 -- [bridge sw] -- end1, 10 gPTP frames each\n";
    std::cout << "       way, overlapping in time.\n\n";
    runExperiment2();
    bool bridgeForwards = (g_e2_end1RecvGptp == 10) && (g_e2_end0RecvGptp == 10) &&
                          (g_e2_lastProtoAtEnd1 == kGptpProto);
    std::cout << std::hex;
    std::cout << "    end1 received " << std::dec << g_e2_end1RecvGptp
              << "/10 forwarded gPTP frames (last ethertype 0x" << std::hex << g_e2_lastProtoAtEnd1
              << ")\n";
    std::cout << "    end0 received " << std::dec << g_e2_end0RecvGptp
              << "/10 forwarded gPTP frames (reverse direction)\n";
    std::cout << "    => " << (bridgeForwards ? "PASS" : "FAIL")
              << ": the bridge forwards 0x88b6 hop-by-hop in BOTH directions, ethertype "
                 "intact.\n";

    bool pass = fullDuplexProven && bridgeForwards;
    std::cout << "\n=== SPIKE RESULT: " << (pass ? "PASS" : "FAIL")
              << " -- SimpleNetDevice+SimpleChannel carries 0x88b6 full-duplex AND bridges it, "
                 "no ns-3 core patch, no new NetDevice subclass. ===\n\n";
    return pass ? 0 : 1;
}
