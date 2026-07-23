// SPDX-License-Identifier: Apache-2.0
//
// syncsim ns-3 migration POC -- Phase 3 (M3): finite queues + background
// congestion, reproducing the "congestion degrades sync only for whoever shares
// the congested egress queue, not globally" finding.
//
// This reuses the EXACT M2 ("Nominal") 18-node multi-hop topology, clocks, and
// the *unchanged* Phase-2 gPTP mechanism (gptp.{h,cc} here are byte-identical
// vendored copies of ns3/gptp/'s -- confirmed by md5sum, same as ns3/nominal/).
// The only new code is (a) a finite, real-dropping DropTailQueue on every switch
// egress port and (b) a lightweight background *data* plane (distinct ethertype)
// that offers three ~50 Mbps flows onto the single swCore->coreClient 100 Mbps
// link -- a guaranteed ~150-into-100 Mbps oversubscription.
//
// Reproduces simulations/congestion.ini (M3). Read that file for the INET-side
// scenario. The numbers below are NOT a digit-match target -- the gate is the
// mechanism/shape (coreClient's sync degrades, everyone else stays near
// baseline), per NS3_MIGRATION_POC_PLAN.md / analyze.py's philosophy.
//
// ---------------------------------------------------------------------------
// HOW THE COUPLING ARISES (why this reproduces the finding at all)
//
// gPTP frames and data frames share the SAME per-device egress queue at the
// bottleneck: both are emitted via NetDevice::Send() on swCore's coreClient-facing
// SimpleNetDevice, which owns one finite DropTailQueue. Three ~50 Mbps flows are
// forwarded hop-by-hop from clientsA/B/C[0] and converge on that one 100 Mbps
// egress, so its queue runs full and drops ~1/3 of everything in it. The gPTP
// Sync/Pdelay frames swCore *regenerates* toward coreClient must sit in -- or be
// dropped from -- that same congested queue. So coreClient's Sync arrives late
// (extra, unmodeled queueing delay folds straight into recvLocal -
// reconstructedGmTime) or not at all (a missed servo cycle => longer free-run =>
// bigger offset). EVERY other egress queue carries at most one un-oversubscribed
// forwarded flow plus negligible gPTP, never fills, never drops, and those nodes'
// sync is untouched -- AND, because the links are now full-duplex, the forward
// transit data on a zone link no longer delays the reverse-direction gPTP Sync on
// that same link (the CSMA shared-medium artifact that S5 used to dodge by
// injecting at the egress). That asymmetry IS the finding -- it emerges from the
// shared-queue mechanism over genuine hop-by-hop forwarding, not hand-coded per node.
//
// ---------------------------------------------------------------------------
// S5 CLOSED (Tier 3 / P3a real fix): the three background flows now GENUINELY
// originate at clientsA/B/C[0] and are L2-forwarded hop-by-hop through swA/B/C ->
// swCore -> coreClient, exactly as INET's full-duplex Ethernet forwards them --
// no longer injected directly at swCore's convergence egress.
//
// This became possible by swapping the link transport from CsmaNetDevice/
// CsmaChannel (a single shared half-duplex CSMA/CD medium -- the S5 root cause)
// to ns-3.45's SimpleNetDevice/SimpleChannel (mainline, no external module). Each
// SimpleNetDevice owns its own tx queue and tx-busy state and SimpleChannel does
// no carrier sense / collision, so with exactly two devices per channel each link
// is a genuine full-duplex point-to-point link: forward transit data on a link no
// longer contends with the reverse-direction gPTP Sync on that SAME link. That is
// precisely the coupling that destroyed a prior real-forwarding attempt on CSMA
// (every zone switch spuriously degraded to ~5 ms). The P3a spike proved these two
// primitives on a throwaway program (SimpleNetDevice carries 0x88b6 full-duplex,
// reverse latency 1.000x under forward saturation vs CSMA's 1488x); this file is
// the real fix on the actual 18-node M3 topology. See ns3/spikes/P3A_SPIKE_FINDINGS.md.
//
// Data forwarding is a static, hand-coded L2 table (the topology is a fixed tree,
// so no dynamic MAC learning is needed) implemented in CombinedRx: each zone
// switch forwards data-ethertype frames out its swCore-facing uplink; swCore
// forwards them out its coreClient-facing egress. gPTP frames are NOT forwarded --
// they keep their per-port termination (S4) via the same CombinedRx dispatch,
// untouched. We deliberately do NOT use a BridgeNetDevice (which would transparently
// forward and clash with S4's per-port gPTP termination); the spike recommended
// this manual route. The ~150-into-100 oversubscription, the real drops, and the
// shared-queue coupling all still land on the swCore->coreClient egress -- but now
// as the emergent result of real hop-by-hop forwarding, and the zone links are
// isolated because full-duplex tx/rx are independent, exactly INET's result.
//
// The scenario is run TWICE inside one process -- background OFF (baseline, ==
// M2 with finite queues but no data) then background ON -- and the two per-node
// peak-offset columns are printed side by side, so the isolation shape is a
// direct within-binary comparison, not a cross-run eyeball. The 12 client drift
// rates are drawn ONCE up front and reused across both passes, so the only
// difference between the two passes is the presence of background traffic.
//
// Determinism: RNG use is the 12 seeded client drift draws (once) + the seeded
// ExponentialRandomVariable inter-packet timing (background pass only). Seed/run
// are pinned, so two process runs are byte-identical (confirmed).

#include "gptp.h"

#include "ns3/core-module.h"
#include "ns3/network-module.h" // SimpleNetDevice/SimpleChannel/-Helper (S5 full-duplex transport)

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("SyncsimCongestionTopology");

namespace
{

constexpr int GM = 0, SWCORE = 1, CORECLIENT = 2, SWA = 3, SWB = 4, SWC = 5;
constexpr int NNODES = 18;
constexpr uint16_t kGptpProtocol = 0x88b6; // must match gptp.cc's kGptpProtocol
constexpr uint16_t kDataProtocol = 0x88b5; // background data plane (distinct)
constexpr uint32_t kDataPayload = 946;     // 1000B - 54B, per congestion.ini

struct NodeMeta
{
    std::string name;
    double ppm;
    int hops;
};

struct OffsetSample
{
    double global;
    double offset; // microseconds
};

// Phase 4 (M5 / observability): per-egress-queue counters, accumulated from the
// ns-3 Queue Enqueue/Dequeue/Drop traces, then written out as scalars in the
// exact schema INET's queue scalars use so scripts/analyze.py's congestion
// summary (regex \.macLayer\.queue$ + names incomingPackets:count etc.) reads
// this ns-3 run unchanged. incoming = enqueued + dropped (everything that
// arrived at the queue); dropped = overflow drops; outgoing = dequeued. Lengths
// are in BITS (INET's PacketQueue convention), so we sum GetSize()*8.
struct QueueCounters
{
    std::string module;
    uint64_t enqPk{0};
    uint64_t deqPk{0};
    uint64_t dropPk{0};
    double enqBits{0};
    double deqBits{0};
    double dropBits{0};
};

void
QEnqueue(QueueCounters* q, Ptr<const Packet> p)
{
    q->enqPk++;
    q->enqBits += p->GetSize() * 8.0;
}

void
QDequeue(QueueCounters* q, Ptr<const Packet> p)
{
    q->deqPk++;
    q->deqBits += p->GetSize() * 8.0;
}

void
QDrop(QueueCounters* q, Ptr<const Packet> p)
{
    q->dropPk++;
    q->dropBits += p->GetSize() * 8.0;
}

// S5 fix: one entry of the static hop-by-hop data-forwarding table. For a switch
// node it is the egress device + peer MAC + port index toward coreClient; for a
// source client it is the uplink toward its zone switch (used to ORIGINATE the
// flow). The topology is a fixed tree, so this table is hand-coded, not learned.
struct DataUplink
{
    Ptr<NetDevice> dev;
    Address peer;
    uint32_t port{0};
};

// State that lives across a single runScenario() pass. A pointer to the active
// pass's state is stashed in g_active so static trampolines (bound into ns-3
// callbacks) can reach it without per-callback captures.
struct PassState
{
    std::map<int, std::vector<OffsetSample>> traj;
    std::vector<syncsim::GptpEntity*> ent; // indexed by node id
    std::map<int, DataUplink> uplink;      // S5: nodeId -> its data uplink toward coreClient
    uint64_t dataOffered{0};               // frames injected by the 3 sources
    uint64_t dataDelivered{0};             // frames reaching the coreClient sink
    uint64_t bottleneckDrops{0};           // drops on swCore->coreClient queue
    double backlogSum{0};                  // running sum of periodic length samples
    uint64_t backlogSamples{0};
    uint32_t backlogMax{0};
    Ptr<Queue<Packet>> bottleneckQueue;
    std::vector<QueueCounters> queueCounters; // Phase 4: per-switch-egress scalars
    // P1b: per-switch-egress queue-length time series (queueLength:vector export).
    // Parallel to queueCounters: qSampleQueues[i]/qSampleNames[i] describe queue i,
    // qLenTime[i]/qLenVal[i] are its sampled (global time, backlog packets) series.
    std::vector<Ptr<Queue<Packet>>> qSampleQueues;
    std::vector<std::string> qSampleNames;
    std::vector<std::vector<double>> qLenTime;
    std::vector<std::vector<double>> qLenVal;
};

PassState* g_active = nullptr;

// P2c: pcap capture prefix. Empty (default) = capture OFF, so existing gate
// behavior and stdout are byte-identical when unset. When set (via --pcapPrefix),
// only the congested pass is captured (the interesting one), via link()'s
// manual per-device PcapCapture hook (see its comment -- SimpleNetDeviceHelper
// has no EnablePcapAll wrapper, but ns-3's own low-level pcap primitives work).
std::string g_pcapPrefix;

void
OffsetSink(int id, Time global, double offsetSec)
{
    g_active->traj[id].push_back({global.GetSeconds(), offsetSec * 1e6});
}

// P2c pcap capture (restored under the S5 transport): ns-3's own low-level
// pcap primitives -- PcapHelper::CreateFile + PcapFileWrapper::Write -- are the
// SAME machinery CsmaHelper::EnablePcapInternal calls internally; the only
// thing SimpleNetDeviceHelper lacks is the EnablePcapAll *convenience wrapper*
// (confirmed against the pinned ns-3.45 tree: SimpleNetDevice exposes no
// PromiscSniffer/Sniffer trace source for a helper to auto-hook). So this hooks
// the primitives by hand instead, at the one place every frame this topology
// carries already passes through: CombinedRx. Capturing on RX ONLY (once per
// device, for whatever that device receives) is content-equivalent to CSMA's
// old per-device TX+RX capture on this point-to-point topology -- every frame
// sent by one device is received by exactly one other, so it is captured
// exactly once, just filed under the receiver's pcap instead of duplicated
// under both link ends. This needs no gptp.cc change (gPTP frames arrive at
// CombinedRx the same as data frames; capture happens before dispatch).
// SimpleNetDevice doesn't carry a real Ethernet header on the wire (addressing
// is out-of-band via a SimpleTag), so a real 14-byte header is synthesized
// (dst = this device's own address, src = `from`, ethertype = protocol) --
// check_pcap_gptp.py parses exactly this classic 14-byte-header + DLT_EN10MB
// format, unchanged from the old CSMA capture.
void
PcapCapture(Ptr<PcapFileWrapper> file,
           Address dst,
           Ptr<const Packet> pkt,
           uint16_t protocol,
           const Address& from)
{
    EthernetHeader hdr(false);
    hdr.SetSource(Mac48Address::ConvertFrom(from));
    hdr.SetDestination(Mac48Address::ConvertFrom(dst));
    hdr.SetLengthType(protocol);
    file->Write(Simulator::Now(), hdr, pkt);
}

// Combined receive callback: dispatch by ethertype.
//   gPTP  -> the node's GptpEntity (per-port termination, S4 -- NOT forwarded).
//   data  -> at coreClient (the sink) counted as delivered; at a switch node,
//            L2-forwarded hop-by-hop out that node's static data uplink toward
//            coreClient (S5 real forwarding). This is the whole S5 fix: data
//            genuinely traverses clientsX[0] -> swX -> swCore -> coreClient,
//            enqueuing into (and, at the oversubscribed bottleneck, dropping
//            from) each hop's real egress queue.
bool
CombinedRx(int nodeId,
           syncsim::GptpEntity* ent,
           uint32_t port,
           Ptr<PcapFileWrapper> pcapFile,
           Ptr<NetDevice> device,
           Ptr<const Packet> pkt,
           uint16_t protocol,
           const Address& from)
{
    if (pcapFile)
    {
        PcapCapture(pcapFile, device->GetAddress(), pkt, protocol, from);
    }
    if (protocol == kGptpProtocol)
    {
        return ent->OnDeviceReceive(port, pkt, from);
    }
    if (protocol == kDataProtocol)
    {
        if (nodeId == CORECLIENT)
        {
            g_active->dataDelivered++;
            return true;
        }
        // Switch forwarding: send out this node's data uplink toward coreClient,
        // unless the frame arrived on that uplink (loop guard; never happens in
        // this tree, but keeps the static table honest).
        auto it = g_active->uplink.find(nodeId);
        if (it != g_active->uplink.end() && port != it->second.port)
        {
            it->second.dev->Send(pkt->Copy(), it->second.peer, kDataProtocol);
        }
    }
    return true;
}

void
BottleneckDropTrace(Ptr<const Packet>)
{
    g_active->bottleneckDrops++;
}

void
SampleBacklog(Time interval, Time stopAt)
{
    if (Simulator::Now() >= stopAt)
    {
        return;
    }
    uint32_t n = g_active->bottleneckQueue->GetNPackets();
    g_active->backlogSum += n;
    g_active->backlogSamples++;
    g_active->backlogMax = std::max(g_active->backlogMax, n);
    Simulator::Schedule(interval, &SampleBacklog, interval, stopAt);
}

// P1b (M5 / observability): periodically sample every traced switch-egress
// queue's current backlog into a per-queue time series, exported as
// queueLength:vector rows (see WriteVectorsCsv). Read-only (GetNPackets only),
// so it does not perturb the simulation -- the gate numbers are byte-identical
// whether or not this runs; it is scheduled only when --resultDir is set.
void
SampleQueueLengths(Time interval, Time stopAt)
{
    if (Simulator::Now() >= stopAt)
    {
        return;
    }
    double now = Simulator::Now().GetSeconds();
    for (size_t i = 0; i < g_active->qSampleQueues.size(); ++i)
    {
        g_active->qLenTime[i].push_back(now);
        g_active->qLenVal[i].push_back(g_active->qSampleQueues[i]->GetNPackets());
    }
    Simulator::Schedule(interval, &SampleQueueLengths, interval, stopAt);
}

// One background source: ORIGINATE a data frame on `dev` (a source client's
// uplink device) addressed to `peer` (its zone switch), then reschedule after an
// exponential(mean) gap (seeded). Runs only in [start,stop]. From there the frame
// is L2-forwarded hop-by-hop to coreClient by CombinedRx (S5 real forwarding).
void
DataSource(Ptr<NetDevice> dev,
           Address peer,
           Ptr<ExponentialRandomVariable> gap,
           Time stopAt)
{
    if (Simulator::Now() >= stopAt)
    {
        return;
    }
    dev->Send(Create<Packet>(kDataPayload), peer, kDataProtocol);
    g_active->dataOffered++;
    Simulator::Schedule(Seconds(gap->GetValue()), &DataSource, dev, peer, gap, stopAt);
}

// ---------------------------------------------------------------------------
// Phase 4 (M5 / observability): CSV export in OMNeT++ opp_scavetool schema so
// scripts/analyze.py reads this ns-3 run with zero reimplementation. See
// nominal-topology.cc's WriteVectorsCsv comment for the offset-from-GM trick;
// here we additionally emit queue scalars (INET's names) from the congested
// pass so analyze.py's Mbps/pps/drop-ppm congestion summary lights up too.
void
WriteVectorsCsv(const std::string& dir,
                const std::vector<NodeMeta>& meta,
                const std::map<int, std::vector<OffsetSample>>& traj,
                const PassState& state)
{
    std::string path = dir + "/vectors.csv";
    std::ofstream f(path);
    if (!f)
    {
        std::cerr << "[congestion] WARN: cannot write " << path << "\n";
        return;
    }
    f << "module,name,vectime,vecvalue\n";
    uint32_t written = 0;
    for (const auto& [id, samples] : traj)
    {
        if (samples.empty())
        {
            continue;
        }
        std::string module = "Nominal." + meta[id].name + ".clock";
        std::ostringstream vt, vv;
        vt << std::setprecision(15);
        vv << std::setprecision(15);
        for (size_t i = 0; i < samples.size(); ++i)
        {
            if (i)
            {
                vt << ' ';
                vv << ' ';
            }
            double g = samples[i].global;
            vt << g;
            vv << (g + samples[i].offset * 1e-6); // local clock time = global + offset
        }
        f << module << ",timeChanged:vector,\"" << vt.str() << "\",\"" << vv.str() << "\"\n";
        ++written;
    }
    // P1b: one queueLength:vector row per traced switch-egress queue -- module
    // "Nominal.<node>.eth<port>.macLayer.queue" (the bracket-free ns-3 form
    // scripts/plot_results.py's BOTTLENECK/_resolve_bottleneck already match, so
    // its backlog + offset-vs-backlog-coupling plots render). vectime = global
    // sample time (s), vecvalue = queue backlog (packets) at that time.
    uint32_t qwritten = 0;
    for (size_t i = 0; i < state.qSampleNames.size(); ++i)
    {
        if (state.qLenTime[i].empty())
        {
            continue;
        }
        std::ostringstream vt, vv;
        vt << std::setprecision(15);
        vv << std::setprecision(15);
        for (size_t k = 0; k < state.qLenTime[i].size(); ++k)
        {
            if (k)
            {
                vt << ' ';
                vv << ' ';
            }
            vt << state.qLenTime[i][k];
            vv << state.qLenVal[i][k];
        }
        f << state.qSampleNames[i] << ",queueLength:vector,\"" << vt.str() << "\",\"" << vv.str()
          << "\"\n";
        ++qwritten;
    }
    std::cout << "[congestion] wrote " << path << " (" << written << " clock vectors, " << qwritten
              << " queueLength vectors)\n";
}

void
WriteScalarsCsv(const std::string& path, const std::vector<QueueCounters>& qcs)
{
    std::ofstream f(path);
    if (!f)
    {
        std::cerr << "[congestion] WARN: cannot write " << path << "\n";
        return;
    }
    f << "module,name,value\n";
    f << std::setprecision(15);
    for (const auto& q : qcs)
    {
        uint64_t inPk = q.enqPk + q.dropPk;       // everything that arrived
        double inBits = q.enqBits + q.dropBits;
        f << q.module << ",incomingPackets:count," << inPk << "\n";
        f << q.module << ",outgoingPackets:count," << q.deqPk << "\n";
        f << q.module << ",droppedPacketsQueueOverflow:count," << q.dropPk << "\n";
        f << q.module << ",incomingPacketLengths:sum," << inBits << "\n";
        f << q.module << ",outgoingPacketLengths:sum," << q.deqBits << "\n";
        f << q.module << ",droppedPacketLengths:sum," << q.dropBits << "\n";
    }
    std::cout << "[congestion] wrote " << path << " (" << qcs.size() << " queue scalar groups)\n";
}

struct Stat
{
    double peak;
    double finalOff;
    int hops;
    double ppm;
    std::string name;
    uint32_t servos;
};

// Run the whole 18-node scenario once. background selects whether the 3 data
// sources are active. `meta` carries the (once-drawn) client drift rates so both
// passes use identical clocks.
void
runScenario(bool background,
            const std::vector<NodeMeta>& meta,
            double simTime,
            double bgStart,
            double syncIntervalMs,
            double pdelayIntervalMs,
            uint32_t queueCap,
            double meanGapUs,
            std::vector<Stat>& st,
            PassState& state,
            bool sampleQueues)
{
    g_active = &state;

    NodeContainer nodes;
    nodes.Create(NNODES);

    // S5 fix: full-duplex point-to-point links via SimpleNetDevice/SimpleChannel
    // (mainline ns-3.45). DataRate is a per-device attribute here (CSMA had it on
    // the channel); Delay stays a channel attribute. Install(NodeContainer(a,b))
    // makes exactly two devices on one fresh channel == a genuine full-duplex
    // point-to-point link (each direction independent), matching CSMA's 100 Mbps /
    // ~1 us so the timing-sensitive numbers stay comparable.
    SimpleNetDeviceHelper simple;
    simple.SetDeviceAttribute("DataRate", DataRateValue(DataRate("100Mbps")));
    simple.SetChannelAttribute("Delay", TimeValue(MicroSeconds(1)));

    std::vector<std::unique_ptr<syncsim::Clock>> clocks(NNODES);
    std::vector<std::unique_ptr<syncsim::GptpEntity>> ent(NNODES);
    state.ent.assign(NNODES, nullptr);
    for (int id = 0; id < NNODES; ++id)
    {
        clocks[id] = std::make_unique<syncsim::Clock>(meta[id].ppm);
        ent[id] = std::make_unique<syncsim::GptpEntity>(meta[id].name, clocks[id].get(),
                                                        /*isGm=*/id == GM);
        state.ent[id] = ent[id].get();
    }

    // Switch egress devices get the finite queue. switchDevNames carries the
    // INET-style module path (Nominal.<node>.eth<port>.macLayer.queue) for each,
    // for the Phase 4 scalar export -- port index = the gPTP port index, which
    // matches nominal.ned's port order (swCore: eth0=gm, eth1=coreClient, ...).
    std::vector<Ptr<SimpleNetDevice>> switchDevs;
    std::vector<std::string> switchDevNames;
    auto qname = [&](int node, uint32_t port) {
        return "Nominal." + meta[node].name + ".eth" + std::to_string(port) + ".macLayer.queue";
    };

    // A built link: the 2-device container plus the two AddPort'd port indices, so
    // callers can register the endpoints in the S5 data-forwarding table.
    struct LinkResult
    {
        NetDeviceContainer dv;
        uint32_t pa;
        uint32_t pb;
    };

    // link a<->b; aIsMaster picks gPTP roles. aIsSwitch/bIsSwitch mark switch
    // egresses (finite queue). Returns the devices + port indices for data wiring.
    auto link = [&](int a, int b, bool aIsMaster, bool aIsSwitch, bool bIsSwitch) -> LinkResult {
        NetDeviceContainer dv = simple.Install(NodeContainer(nodes.Get(a), nodes.Get(b)));
        uint32_t pa = ent[a]->AddPort(dv.Get(0), dv.Get(1)->GetAddress(), aIsMaster);
        uint32_t pb = ent[b]->AddPort(dv.Get(1), dv.Get(0)->GetAddress(), !aIsMaster);
        // P2c pcap: one capture file per device, RX-hooked in CombinedRx (see
        // PcapCapture's comment). Only the congested pass is captured (the
        // interesting one), matching the original CsmaHelper::EnablePcapAll scope.
        Ptr<PcapFileWrapper> fileA, fileB;
        if (background && !g_pcapPrefix.empty())
        {
            PcapHelper ph;
            fileA = ph.CreateFile(g_pcapPrefix + "-" + meta[a].name + "-" + std::to_string(pa) +
                                      ".pcap",
                                  std::ios::out, PcapHelper::DLT_EN10MB);
            fileB = ph.CreateFile(g_pcapPrefix + "-" + meta[b].name + "-" + std::to_string(pb) +
                                      ".pcap",
                                  std::ios::out, PcapHelper::DLT_EN10MB);
        }
        dv.Get(0)->SetReceiveCallback(MakeBoundCallback(&CombinedRx, a, ent[a].get(), pa, fileA));
        dv.Get(1)->SetReceiveCallback(MakeBoundCallback(&CombinedRx, b, ent[b].get(), pb, fileB));
        if (aIsSwitch)
        {
            switchDevs.push_back(DynamicCast<SimpleNetDevice>(dv.Get(0)));
            switchDevNames.push_back(qname(a, pa));
        }
        if (bIsSwitch)
        {
            switchDevs.push_back(DynamicCast<SimpleNetDevice>(dv.Get(1)));
            switchDevNames.push_back(qname(b, pb));
        }
        return {dv, pa, pb};
    };

    link(GM, SWCORE, true, false, true);
    LinkResult coreLink = link(SWCORE, CORECLIENT, true, true, false);
    NetDeviceContainer coreToCoreClient = coreLink.dv;
    // S5 forwarding table: swCore forwards data out its coreClient-facing egress.
    state.uplink[SWCORE] = {coreToCoreClient.Get(0), coreToCoreClient.Get(1)->GetAddress(),
                            coreLink.pa};
    LinkResult coreToA = link(SWCORE, SWA, true, true, true);
    LinkResult coreToB = link(SWCORE, SWB, true, true, true);
    LinkResult coreToC = link(SWCORE, SWC, true, true, true);
    // Each zone switch forwards data out its swCore-facing uplink (the b-side of
    // the SWCORE<->SW* link, port pb).
    state.uplink[SWA] = {coreToA.dv.Get(1), coreToA.dv.Get(0)->GetAddress(), coreToA.pb};
    state.uplink[SWB] = {coreToB.dv.Get(1), coreToB.dv.Get(0)->GetAddress(), coreToB.pb};
    state.uplink[SWC] = {coreToC.dv.Get(1), coreToC.dv.Get(0)->GetAddress(), coreToC.pb};
    int zoneSw[3] = {SWA, SWB, SWC};
    for (int z = 0; z < 3; ++z)
    {
        for (int i = 0; i < 4; ++i)
        {
            int clientId = 6 + z * 4 + i;
            LinkResult cl = link(zoneSw[z], clientId, true, true, false);
            // Client's uplink toward its zone switch -- used to ORIGINATE its flow.
            state.uplink[clientId] = {cl.dv.Get(1), cl.dv.Get(0)->GetAddress(), cl.pb};
        }
    }

    // ---- Finite, real-dropping egress queues on every switch port (M3) ------
    for (auto& d : switchDevs)
    {
        d->GetQueue()->SetAttribute("MaxSize", QueueSizeValue(QueueSize(PACKETS, queueCap)));
    }
    // Phase 4: per-queue counters for the scalar export. Sized once (pointers into
    // it must stay stable), then Enqueue/Dequeue/Drop traces accumulate per queue.
    state.queueCounters.assign(switchDevs.size(), QueueCounters{});
    for (size_t i = 0; i < switchDevs.size(); ++i)
    {
        state.queueCounters[i].module = switchDevNames[i];
        QueueCounters* qc = &state.queueCounters[i];
        Ptr<Queue<Packet>> q = switchDevs[i]->GetQueue();
        q->TraceConnectWithoutContext("Enqueue", MakeBoundCallback(&QEnqueue, qc));
        q->TraceConnectWithoutContext("Dequeue", MakeBoundCallback(&QDequeue, qc));
        q->TraceConnectWithoutContext("Drop", MakeBoundCallback(&QDrop, qc));
    }
    // P1b: per-queue backlog time series (queueLength:vector export). Same queues,
    // same module names as the scalar counters; sampled periodically below.
    state.qSampleNames = switchDevNames;
    state.qSampleQueues.clear();
    for (auto& d : switchDevs)
    {
        state.qSampleQueues.push_back(d->GetQueue());
    }
    state.qLenTime.assign(switchDevs.size(), {});
    state.qLenVal.assign(switchDevs.size(), {});
    // Bottleneck = swCore->coreClient egress queue: trace drops + sample backlog.
    Ptr<NetDevice> bottleneckDev = coreToCoreClient.Get(0); // swCore's side
    state.bottleneckQueue = DynamicCast<SimpleNetDevice>(bottleneckDev)->GetQueue();
    state.bottleneckQueue->TraceConnectWithoutContext("Drop", MakeCallback(&BottleneckDropTrace));

    // ---- Offset trajectories (every non-GM node) ----------------------------
    for (int id = 1; id < NNODES; ++id)
    {
        ent[id]->ConnectOffsetTrace(MakeBoundCallback(&OffsetSink, id));
    }

    // ---- gPTP drivers -------------------------------------------------------
    Time pdelayInterval = MilliSeconds(pdelayIntervalMs);
    Time syncInterval = MilliSeconds(syncIntervalMs);
    for (int id = 1; id < NNODES; ++id)
    {
        Simulator::Schedule(MilliSeconds(5), &syncsim::GptpEntity::StartPdelay, ent[id].get(),
                            pdelayInterval);
    }
    Simulator::Schedule(syncInterval, &syncsim::GptpEntity::SendSync, ent[GM].get(), syncInterval);

    // ---- Background data sources (S5: originated at clientsX[0], forwarded) --
    // Three ~50 Mbps flows, one per zone, each ORIGINATING at that zone's first
    // client (clientsA/B/C[0] = node ids 6/10/14) and L2-forwarded hop-by-hop up
    // to coreClient, where they aggregate onto the single 100 Mbps bottleneck
    // (~150-into-100 oversubscription) exactly as INET forwards them.
    std::vector<Ptr<ExponentialRandomVariable>> gaps;
    if (background)
    {
        const int srcClients[3] = {6, 10, 14}; // clientsA[0], clientsB[0], clientsC[0]
        for (int s = 0; s < 3; ++s)
        {
            Ptr<ExponentialRandomVariable> g = CreateObject<ExponentialRandomVariable>();
            g->SetAttribute("Mean", DoubleValue(meanGapUs * 1e-6));
            gaps.push_back(g);
            const DataUplink& up = state.uplink[srcClients[s]];
            Simulator::Schedule(Seconds(bgStart), &DataSource, up.dev, up.peer, g,
                                Seconds(simTime));
        }
        Simulator::Schedule(Seconds(bgStart), &SampleBacklog, MilliSeconds(1), Seconds(simTime));
    }

    // P1b: queueLength:vector sampling over the whole run (5 ms cadence). Only
    // scheduled when the caller wants the CSV (--resultDir set); read-only, so
    // when it is not scheduled the run is byte-identical to before.
    if (sampleQueues)
    {
        Simulator::Schedule(MilliSeconds(5), &SampleQueueLengths, MilliSeconds(5),
                            Seconds(simTime));
    }

    // P2c pcap capture is wired up above, in link()'s per-device PcapCapture
    // hook -- see that function's comment for how it works under SimpleNetDevice.

    Simulator::Stop(Seconds(simTime));
    Simulator::Run();
    Simulator::Destroy();

    // ---- Per-node offset stats ----------------------------------------------
    st.clear();
    for (int id = 1; id < NNODES; ++id)
    {
        double peak = 0, finalOff = 0;
        if (!state.traj[id].empty())
        {
            for (const auto& sm : state.traj[id])
            {
                peak = std::max(peak, std::fabs(sm.offset));
            }
            finalOff = state.traj[id].back().offset;
        }
        st.push_back({peak, finalOff, meta[id].hops, meta[id].ppm, meta[id].name,
                      state.ent[id]->GetServoCount()});
    }
    g_active = nullptr;
}

} // namespace

int
main(int argc, char* argv[])
{
    double simTime = 60.0; // P2d: normalized to OMNeT++'s 60s (was 30s); override with --simTime
    double bgStart = 1.0; // let gPTP converge first, then turn on congestion
    double syncIntervalMs = 125.0;
    double pdelayIntervalMs = 50.0;
    uint32_t queueCap = 10;      // congestion.ini packetCapacity = 10
    double meanGapUs = 160.0;    // congestion.ini productionInterval = exponential(160us)
    double degradeFactor = 5.0;  // coreClient peak must degrade >= this x baseline
    double isolationTolUs = 5.0; // every other node must stay within this of baseline
    std::string resultDir = "";  // Phase 4: dir for vectors.csv + scalars.csv (empty = skip)
    CommandLine cmd(__FILE__);
    cmd.AddValue("simTime", "Simulation duration (s)", simTime);
    cmd.AddValue("bgStart", "When background traffic starts (s)", bgStart);
    cmd.AddValue("syncInterval", "gPTP Sync interval (ms)", syncIntervalMs);
    cmd.AddValue("pdelayInterval", "Peer-delay interval (ms)", pdelayIntervalMs);
    cmd.AddValue("queueCap", "Switch egress queue capacity (packets)", queueCap);
    // Phase 4 / M5 sweep knob: --queueCapacity is an alias for --queueCap, named
    // to mirror sweep.ini's `${cap = 5, 20, 80}` (packetCapacity) so ns3/scripts/
    // run_sweep.sh drives the same lever OMNeT++'s sweep.ini does.
    cmd.AddValue("queueCapacity", "Alias for queueCap (mirrors sweep.ini cap)", queueCap);
    cmd.AddValue("meanGap", "Mean data inter-packet gap (us)", meanGapUs);
    cmd.AddValue("resultDir",
                 "Phase 4: directory to write vectors.csv + scalars.csv (opp_scavetool "
                 "schema, congested pass) for scripts/analyze.py; empty = skip (default)",
                 resultDir);
    cmd.AddValue("pcapPrefix",
                 "P2c: enable pcap capture (one file per device, congested pass only) with "
                 "this file prefix; empty = off (default). Manual capture under the S5 "
                 "SimpleNetDevice transport -- see link()'s PcapCapture comment.",
                 g_pcapPrefix);
    cmd.Parse(argc, argv);

    RngSeedManager::SetSeed(1);
    RngSeedManager::SetRun(1);

    // ---- Node metadata; draw the 12 client drifts ONCE (reused both passes) --
    std::vector<NodeMeta> meta(NNODES);
    meta[GM] = {"gm", 0.0, 0};
    meta[SWCORE] = {"swCore", 50.0, 1};
    meta[CORECLIENT] = {"coreClient", 150.0, 2};
    meta[SWA] = {"swA", 80.0, 2};
    meta[SWB] = {"swB", -60.0, 2};
    meta[SWC] = {"swC", 100.0, 2};
    Ptr<UniformRandomVariable> driftRv = CreateObject<UniformRandomVariable>();
    driftRv->SetAttribute("Min", DoubleValue(-200.0));
    driftRv->SetAttribute("Max", DoubleValue(200.0));
    const char* zoneLetters[3] = {"A", "B", "C"};
    for (int z = 0; z < 3; ++z)
    {
        for (int i = 0; i < 4; ++i)
        {
            int id = 6 + z * 4 + i;
            meta[id] = {"clients" + std::string(zoneLetters[z]) + "[" + std::to_string(i) + "]",
                        driftRv->GetValue(), 3};
        }
    }

    // ---- Pass 1: baseline (finite queues, NO background) --------------------
    std::vector<Stat> stBase;
    PassState base;
    runScenario(false, meta, simTime, bgStart, syncIntervalMs, pdelayIntervalMs, queueCap,
                meanGapUs, stBase, base, /*sampleQueues=*/false);

    // ---- Pass 2: congested (finite queues + background) --------------------
    // Only the congested pass is exported, so only it needs queue-length sampling
    // (and only when --resultDir is set -- otherwise the run stays byte-identical).
    std::vector<Stat> stBg;
    PassState bg;
    runScenario(true, meta, simTime, bgStart, syncIntervalMs, pdelayIntervalMs, queueCap,
                meanGapUs, stBg, bg, /*sampleQueues=*/!resultDir.empty());

    // ======================= Evidence reporting =============================
    std::cout << std::fixed;
    std::cout << "\n[congestion] M3: finite queues (cap " << queueCap
              << ") + 3x~50Mbps background converging on the swCore->coreClient egress.\n";
    std::cout << "  simTime=" << simTime << "s, background window [" << bgStart << "," << simTime
              << "]s, Sync " << syncIntervalMs << "ms, meanGap " << meanGapUs << "us.\n";

    // ---- Data-plane / bottleneck stats -------------------------------------
    double window = simTime - bgStart;
    double goodputMbps = bg.dataDelivered * kDataPayload * 8.0 / window / 1e6;
    double goodputPps = bg.dataDelivered / window;
    uint64_t offeredIntoQueue = bg.dataDelivered + bg.bottleneckDrops;
    double dropPct = offeredIntoQueue ? 100.0 * bg.bottleneckDrops / offeredIntoQueue : 0.0;
    double meanBacklog = bg.backlogSamples ? bg.backlogSum / bg.backlogSamples : 0.0;
    std::cout << std::setprecision(2);
    std::cout << "\n[congestion] bottleneck (swCore->coreClient egress, cap " << queueCap
              << ") under load:\n";
    std::cout << "  data offered by 3 sources : " << bg.dataOffered << " pkts\n";
    std::cout << "  delivered to coreClient   : " << bg.dataDelivered << " pkts (" << goodputMbps
              << " Mbps, " << goodputPps << " pps)\n";
    std::cout << "  dropped at bottleneck     : " << bg.bottleneckDrops << " pkts (" << dropPct
              << "% of offered-into-queue)\n";
    std::cout << "  queue backlog             : mean " << meanBacklog << "/" << queueCap << ", max "
              << bg.backlogMax << "/" << queueCap << "\n";

    // ---- Side-by-side per-node peak offset: baseline vs congested ----------
    std::cout << "\n[congestion] per-node peak offset-from-GM: baseline vs congested\n";
    std::cout << std::string(78, '-') << "\n";
    std::cout << std::setw(15) << "node" << " | " << std::setw(4) << "hops" << " | " << std::setw(8)
              << "ppm" << " | " << std::setw(11) << "base peak" << " | " << std::setw(11)
              << "cong peak" << " | " << std::setw(8) << "ratio" << "\n";
    std::cout << std::string(78, '-') << "\n";
    std::cout << std::setprecision(3);
    for (size_t i = 0; i < stBase.size(); ++i)
    {
        double bp = stBase[i].peak;
        double cp = stBg[i].peak;
        double ratio = bp > 1e-9 ? cp / bp : 0.0;
        std::cout << std::setw(15) << stBase[i].name << " | " << std::setw(4) << stBase[i].hops
                  << " | " << std::setw(8) << std::setprecision(1) << stBase[i].ppm
                  << std::setprecision(3) << " | " << std::setw(11) << bp << " | " << std::setw(11)
                  << cp << " | " << std::setw(7) << std::setprecision(1) << ratio << "x"
                  << std::setprecision(3) << "\n";
    }

    std::cout << "\n[congestion] coreClient servo corrections: baseline "
              << stBase[CORECLIENT - 1].servos << ", congested " << stBg[CORECLIENT - 1].servos
              << " (fewer under load == missed/dropped Sync frames).\n";

    // ======================= Gate checks ====================================
    bool dropsReal = bg.bottleneckDrops > 0;
    double coreBase = stBase[CORECLIENT - 1].peak;
    double coreCong = stBg[CORECLIENT - 1].peak;
    bool coreDegraded = coreCong > degradeFactor * coreBase && coreCong > 100.0;
    bool othersIsolated = true;
    for (size_t i = 0; i < stBase.size(); ++i)
    {
        if (static_cast<int>(i) == CORECLIENT - 1)
        {
            continue; // coreClient is the one that SHOULD degrade
        }
        if (std::fabs(stBg[i].peak - stBase[i].peak) > isolationTolUs)
        {
            othersIsolated = false;
        }
    }
    bool baselineConverged = true;
    for (const auto& s : stBase)
    {
        if (std::fabs(s.finalOff) >= 2.0)
        {
            baselineConverged = false;
        }
    }

    bool pass = dropsReal && coreDegraded && othersIsolated && baselineConverged;
    std::cout << "\n[congestion] Gate checks (M3):\n";
    std::cout << "  [" << (baselineConverged ? "PASS" : "FAIL")
              << "] baseline (no traffic): every node still converges (|final| < 2 us)\n";
    std::cout << "  [" << (dropsReal ? "PASS" : "FAIL")
              << "] congestion is real: bottleneck queue actually drops packets\n";
    std::cout << "  [" << (coreDegraded ? "PASS" : "FAIL")
              << "] coreClient sync degrades under load (" << std::setprecision(1)
              << (coreBase > 1e-9 ? coreCong / coreBase : 0.0) << "x its baseline, "
              << std::setprecision(3) << coreCong << " us)\n";
    std::cout << "  [" << (othersIsolated ? "PASS" : "FAIL") << "] every OTHER node stays within "
              << isolationTolUs << " us of its baseline (isolation)\n";
    std::cout << "\n[congestion] "
              << (pass ? "GATE PASS: congestion degrades sync ONLY for coreClient (shares the "
                         "congested egress queue), not globally"
                       : "GATE FAIL")
              << std::endl;

    // ---- Phase 4: optional CSV export (the CONGESTED pass) ------------------
    // The congested pass carries both the isolation signature (coreClient's
    // bounded-but-degraded offset, everyone else at baseline) and the real queue
    // drops, so analyze.py --strict reports the full picture off one directory.
    if (!resultDir.empty())
    {
        // See nominal-topology.cc's matching comment: std::ofstream doesn't
        // create missing directories, found during independent verification.
        std::filesystem::create_directories(resultDir);
        WriteVectorsCsv(resultDir, meta, bg.traj, bg);
        WriteScalarsCsv(resultDir + "/scalars.csv", bg.queueCounters);
    }

    return pass ? 0 : 1;
}
