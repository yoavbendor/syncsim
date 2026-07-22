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
// gPTP frames and data frames share the SAME per-device CSMA egress queue: both
// are emitted via NetDevice::Send() on the same CsmaNetDevice, which owns one
// finite DropTailQueue (Phase 0's proven drop mechanism -- smoke-topology.cc).
// On the swCore->coreClient device, three ~50 Mbps data flows are offered onto a
// 100 Mbps link, so that device's queue runs full and drops ~1/3 of them. The
// gPTP Sync/Pdelay frames swCore *regenerates* toward coreClient must sit in --
// or be dropped from -- that same congested queue. So coreClient's Sync arrives
// late (extra, unmodeled queueing delay folds straight into recvLocal -
// reconstructedGmTime) or not at all (a missed servo cycle => longer free-run =>
// bigger offset). EVERY other switch egress queue carries only negligible gPTP,
// never fills, never drops, and those nodes' sync is untouched. That asymmetry
// IS the finding -- it emerges from the shared-queue mechanism, it is not
// hand-coded per node.
//
// ---------------------------------------------------------------------------
// SIMPLIFICATION S5 (new this phase, stated not hidden): the three background
// flows are injected at their CONVERGENCE egress -- swCore's coreClient-facing
// CsmaNetDevice -- rather than L2-forwarded hop-by-hop from clientsA/B/C[0]
// through swA/B/C. Reason: ns-3's CsmaChannel is a single shared CSMA/CD medium
// (no full-duplex mode in mainline ns-3.45; a full-duplex point-to-point device
// was tried but its PPP framer rejects the vendored gPTP ethertype 0x88b6, and
// gptp.cc must stay byte-identical). On a shared medium, 50 Mbps of transit data
// on a zone link spuriously delays the *reverse-direction* gPTP Sync on that
// SAME medium, coupling every node's sync to the load -- an artifact absent from
// INET's FULL-DUPLEX Ethernet links (independent tx/rx), which is exactly why
// INET gets clean isolation. Injecting the aggregate at the one genuinely
// oversubscribed egress reproduces the real phenomenon (the shared bottleneck
// queue coreClient's Sync competes in) without the shared-medium artifact on
// transit links. The convergence, the ~150-into-100 oversubscription, the real
// drops, and the shared-queue coupling all land exactly where congestion.ini
// puts them: the swCore->coreClient egress. The zone links carry only gPTP and
// are therefore isolated -- exactly INET's result.
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
#include "ns3/csma-module.h"
#include "ns3/network-module.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
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

// State that lives across a single runScenario() pass. A pointer to the active
// pass's state is stashed in g_active so static trampolines (bound into ns-3
// callbacks) can reach it without per-callback captures.
struct PassState
{
    std::map<int, std::vector<OffsetSample>> traj;
    std::vector<syncsim::GptpEntity*> ent; // indexed by node id
    uint64_t dataOffered{0};               // frames injected by the 3 sources
    uint64_t dataDelivered{0};             // frames reaching the coreClient sink
    uint64_t bottleneckDrops{0};           // drops on swCore->coreClient queue
    double backlogSum{0};                  // running sum of periodic length samples
    uint64_t backlogSamples{0};
    uint32_t backlogMax{0};
    Ptr<Queue<Packet>> bottleneckQueue;
};

PassState* g_active = nullptr;

void
OffsetSink(int id, Time global, double offsetSec)
{
    g_active->traj[id].push_back({global.GetSeconds(), offsetSec * 1e6});
}

// Combined receive callback: dispatch by ethertype. gPTP -> the node's
// GptpEntity; data -> counted at the coreClient sink (dropped otherwise).
bool
CombinedRx(int nodeId,
           syncsim::GptpEntity* ent,
           uint32_t port,
           Ptr<NetDevice>,
           Ptr<const Packet> pkt,
           uint16_t protocol,
           const Address& from)
{
    if (protocol == kGptpProtocol)
    {
        return ent->OnDeviceReceive(port, pkt, from);
    }
    if (protocol == kDataProtocol && nodeId == CORECLIENT)
    {
        g_active->dataDelivered++;
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

// One background source: inject a data frame on `dev` toward coreClient, then
// reschedule after an exponential(mean) gap (seeded). Runs only in [start,stop].
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
            PassState& state)
{
    g_active = &state;

    NodeContainer nodes;
    nodes.Create(NNODES);

    CsmaHelper csma;
    csma.SetChannelAttribute("DataRate", StringValue("100Mbps"));
    csma.SetChannelAttribute("Delay", TimeValue(MicroSeconds(1)));

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

    // Switch egress devices get the finite queue.
    std::vector<Ptr<CsmaNetDevice>> switchDevs;

    // link a<->b; aIsMaster picks gPTP roles. aIsSwitch/bIsSwitch mark switch
    // egresses (finite queue). Returns the device container for data wiring.
    auto link = [&](int a, int b, bool aIsMaster, bool aIsSwitch, bool bIsSwitch) {
        NetDeviceContainer dv = csma.Install(NodeContainer(nodes.Get(a), nodes.Get(b)));
        uint32_t pa = ent[a]->AddPort(dv.Get(0), dv.Get(1)->GetAddress(), aIsMaster);
        uint32_t pb = ent[b]->AddPort(dv.Get(1), dv.Get(0)->GetAddress(), !aIsMaster);
        dv.Get(0)->SetReceiveCallback(MakeBoundCallback(&CombinedRx, a, ent[a].get(), pa));
        dv.Get(1)->SetReceiveCallback(MakeBoundCallback(&CombinedRx, b, ent[b].get(), pb));
        if (aIsSwitch)
        {
            switchDevs.push_back(DynamicCast<CsmaNetDevice>(dv.Get(0)));
        }
        if (bIsSwitch)
        {
            switchDevs.push_back(DynamicCast<CsmaNetDevice>(dv.Get(1)));
        }
        return dv;
    };

    link(GM, SWCORE, true, false, true);
    NetDeviceContainer coreToCoreClient = link(SWCORE, CORECLIENT, true, true, false);
    link(SWCORE, SWA, true, true, true);
    link(SWCORE, SWB, true, true, true);
    link(SWCORE, SWC, true, true, true);
    int zoneSw[3] = {SWA, SWB, SWC};
    for (int z = 0; z < 3; ++z)
    {
        for (int i = 0; i < 4; ++i)
        {
            link(zoneSw[z], 6 + z * 4 + i, true, true, false);
        }
    }

    // ---- Finite, real-dropping egress queues on every switch port (M3) ------
    for (auto& d : switchDevs)
    {
        d->GetQueue()->SetAttribute("MaxSize", QueueSizeValue(QueueSize(PACKETS, queueCap)));
    }
    // Bottleneck = swCore->coreClient egress queue: trace drops + sample backlog.
    Ptr<NetDevice> bottleneckDev = coreToCoreClient.Get(0); // swCore's side
    Address coreClientPeer = coreToCoreClient.Get(1)->GetAddress();
    state.bottleneckQueue = DynamicCast<CsmaNetDevice>(bottleneckDev)->GetQueue();
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

    // ---- Background data sources (S5: injected at the convergence egress) ---
    std::vector<Ptr<ExponentialRandomVariable>> gaps;
    if (background)
    {
        for (int s = 0; s < 3; ++s) // three ~50 Mbps flows onto the 100 Mbps link
        {
            Ptr<ExponentialRandomVariable> g = CreateObject<ExponentialRandomVariable>();
            g->SetAttribute("Mean", DoubleValue(meanGapUs * 1e-6));
            gaps.push_back(g);
            Simulator::Schedule(Seconds(bgStart), &DataSource, bottleneckDev, coreClientPeer, g,
                                Seconds(simTime));
        }
        Simulator::Schedule(Seconds(bgStart), &SampleBacklog, MilliSeconds(1), Seconds(simTime));
    }

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
    double simTime = 30.0;
    double bgStart = 1.0; // let gPTP converge first, then turn on congestion
    double syncIntervalMs = 125.0;
    double pdelayIntervalMs = 50.0;
    uint32_t queueCap = 10;      // congestion.ini packetCapacity = 10
    double meanGapUs = 160.0;    // congestion.ini productionInterval = exponential(160us)
    double degradeFactor = 5.0;  // coreClient peak must degrade >= this x baseline
    double isolationTolUs = 5.0; // every other node must stay within this of baseline
    CommandLine cmd(__FILE__);
    cmd.AddValue("simTime", "Simulation duration (s)", simTime);
    cmd.AddValue("bgStart", "When background traffic starts (s)", bgStart);
    cmd.AddValue("syncInterval", "gPTP Sync interval (ms)", syncIntervalMs);
    cmd.AddValue("pdelayInterval", "Peer-delay interval (ms)", pdelayIntervalMs);
    cmd.AddValue("queueCap", "Switch egress queue capacity (packets)", queueCap);
    cmd.AddValue("meanGap", "Mean data inter-packet gap (us)", meanGapUs);
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
                meanGapUs, stBase, base);

    // ---- Pass 2: congested (finite queues + background) --------------------
    std::vector<Stat> stBg;
    PassState bg;
    runScenario(true, meta, simTime, bgStart, syncIntervalMs, pdelayIntervalMs, queueCap,
                meanGapUs, stBg, bg);

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

    return pass ? 0 : 1;
}
