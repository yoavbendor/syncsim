// SPDX-License-Identifier: Apache-2.0
//
// syncsim ns-3 migration POC -- Phase 3 (M2 / R-BRIDGE) proof scenario.
//
// Reproduces syncsim's M2 ("Nominal") multi-hop gPTP scenario on ns-3: a
// grandmaster, a core time-aware bridge, and three zone bridges, each fanning
// out to four end stations -- 18 nodes across THREE hop depths from the GM.
// Every node is an independent syncsim::Clock (Phase 1) at M2's drift rates.
// The gPTP mechanism is the *unchanged* Phase-2 gptp.{h,cc} (peer-delay +
// Sync/residence-time correction + servo); the only new code is this topology
// builder + scenario driver. The question this scenario answers is whether the
// Phase-2 GptpEntity (written as "1 slave + N master ports, additive correction
// field", never assuming exactly one bridge hop) ALREADY generalizes to a
// chain of time-aware bridges two levels deep, with zero changes to gptp.{h,cc}.
//
// FINDING (see nominal/README.md for the evidence): it does. gptp.{h,cc} here
// are a byte-identical vendored copy of ns3/gptp/'s -- no edits. The
// correction-field accumulation (upstreamCorrection + linkDelay + residence)
// composes hop-by-hop by construction: swCore forwards [d(gm,swCore) +
// residence_swCore]; swA/B/C add [d(swCore,swZone) + residence_swZone] on top;
// each leaf client reconstructs GM time from the full accumulated correction +
// its own last-hop peer delay. Nothing in HandleSync/RelayDownstream/ApplyServo
// counts hops or assumes depth 1 -- a bridge is simply "a node with >1 port",
// and swCore AND each zone switch play the identical dual slave+master role the
// single Phase-2 sw already proved.
//
// Topology (from simulations/nominal.ned; port order = connection order):
//
//     gm(0)
//       |                                    hop 1 from GM: swCore
//     swCore(50)
//       |-- coreClient(150)                  hop 2: coreClient
//       |-- swA(80) --- clientsA[0..3]        hop 2: swA/B/C
//       |-- swB(-60) -- clientsB[0..3]        hop 3: clientsA/B/C[0..3]
//       |-- swC(100) -- clientsC[0..3]
//
//   swCore: eth0=gm (SLAVE), eth1=coreClient, eth2=swA, eth3=swB, eth4=swC (MASTER)
//   swZone: eth0=swCore (SLAVE), eth1..eth4=clients (MASTER)
//   leaf clients + coreClient: single SLAVE port.
//
// Drift rates (nominal.ini): gm=0, swCore=50, swA=80, swB=-60, swC=100,
// coreClient=150 ppm; the 12 zone clients draw uniform(-200,200) ppm. INET's
// scenario uses uniform(-200ppm,200ppm) per client; we cannot (and per the task
// need not) match INET's specific RNG draws, so we use ns-3's own seeded
// UniformRandomVariable (fixed SetSeed/SetRun, same pattern Phase 0 used) for a
// deterministic, reproducible per-client spread across that range.
//
// WHAT THE GATE GATES ON (per NS3_MIGRATION_POC_PLAN.md / analyze.py's "gate on
// model-correctness, never on result magnitude"): every one of the 18 nodes'
// offset-from-GM genuinely converges toward ~0 and holds, across ALL THREE hop
// depths -- proving hop-by-hop peer-delay + residence-time propagation works
// through a CHAIN of time-aware bridges, not just one. NOT gated on matching
// INET's microsecond digits.
//
// INET M2 reference (recorded in README.md, for orientation, NOT a target to
// match): hops=1 mean/max peak ~7.36us; hops=2 mean ~12.19 max ~18.75us;
// hops=3 mean ~8.40 max ~17.90us -- with the non-obvious finding that peak
// error does NOT grow monotonically with hop count (hops=3 avg < hops=2 avg)
// because each node's own local drift between corrections dominates its error
// more than compounding upstream error does. We report whatever our real run
// shows and note whether the same pattern appears.
//
// Determinism: the ONLY RNG use is the 12 seeded client drift draws; seed/run
// are pinned, so two runs are byte-identical (confirmed).

#include "gptp.h"

#include "ns3/core-module.h"
#include "ns3/csma-module.h"
#include "ns3/network-module.h"

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

NS_LOG_COMPONENT_DEFINE("SyncsimNominalTopology");

namespace
{

// Node metadata, indexed by node id (0=gm ... 17=clientsC[3]).
struct NodeMeta
{
    std::string name;
    double ppm;
    int hops; // hop depth from GM (gm itself = 0; not traced)
};

struct OffsetSample
{
    double global; // seconds
    double offset; // microseconds (local - reconstructed GM)
};

std::map<int, std::vector<OffsetSample>> g_traj; // node id -> samples

void
OffsetSink(int id, Time global, double offsetSec)
{
    g_traj[id].push_back({global.GetSeconds(), offsetSec * 1e6});
}

// P1b (M5 / observability): per-switch-egress queue-length time series, exported
// as queueLength:vector rows (see WriteVectorsCsv). Nominal has no background
// traffic, so these backlogs sit near 0 -- exported anyway for schema consistency
// with congestion/feedback (the task's explicit ask). Read-only sampling, so the
// M2 gate numbers are byte-identical whether or not it runs.
std::vector<Ptr<Queue<Packet>>> g_qSampleQueues;
std::vector<std::string> g_qSampleNames;
std::vector<std::vector<double>> g_qLenTime;
std::vector<std::vector<double>> g_qLenVal;

void
SampleQueueLengths(Time interval, Time stopAt)
{
    if (Simulator::Now() >= stopAt)
    {
        return;
    }
    double now = Simulator::Now().GetSeconds();
    for (size_t i = 0; i < g_qSampleQueues.size(); ++i)
    {
        g_qLenTime[i].push_back(now);
        g_qLenVal[i].push_back(g_qSampleQueues[i]->GetNPackets());
    }
    Simulator::Schedule(interval, &SampleQueueLengths, interval, stopAt);
}

// ns-3 ReceiveCallback signature -> GptpEntity::OnDeviceReceive.
bool
RxTrampoline(syncsim::GptpEntity* ent,
             uint32_t port,
             Ptr<NetDevice>,
             Ptr<const Packet> pkt,
             uint16_t,
             const Address& from)
{
    return ent->OnDeviceReceive(port, pkt, from);
}

// ---------------------------------------------------------------------------
// Phase 4 (M5 / observability) -- export the per-node offset trajectories in
// the SAME long-form CSV schema OMNeT++'s opp_scavetool produces, so the real
// OMNeT++-side analyzer (scripts/analyze.py, via simdata.py) reads this ns-3
// run with ZERO reimplementation. analyze.py filters
//   df[(df["name"] == "timeChanged:vector") & df["module"].str.endswith(".clock")]
// and simdata.parse_offset_series computes offset = vecvalue - vectime, treating
// vecvalue as each sample's LOCAL clock time and vectime as the global sim time.
// So we emit, per traced clock, one row with module "Nominal.<node>.clock" (the
// exact form simdata.HOP_MAPS["Nominal"] already regex-matches for hop grouping),
// name "timeChanged:vector", vectime = global seconds, and vecvalue = local clock
// time = global + offset (offset is stored here in us, so *1e-6). This is purely
// additive: nothing above changes, and the file is only written when --resultDir
// is set (default off), so the proven stdout/gate behavior is untouched.
void
WriteVectorsCsv(const std::string& dir,
                const std::vector<NodeMeta>& meta,
                const std::map<int, std::vector<OffsetSample>>& traj)
{
    std::string path = dir + "/vectors.csv";
    std::ofstream f(path);
    if (!f)
    {
        std::cerr << "[nominal] WARN: cannot write " << path << "\n";
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
    // P1b: one queueLength:vector row per traced switch-egress queue (module
    // "Nominal.<node>.eth<port>.macLayer.queue", vectime = global time (s),
    // vecvalue = backlog packets) -- same schema/naming as congestion/feedback.
    uint32_t qwritten = 0;
    for (size_t i = 0; i < g_qSampleNames.size(); ++i)
    {
        if (g_qLenTime[i].empty())
        {
            continue;
        }
        std::ostringstream vt, vv;
        vt << std::setprecision(15);
        vv << std::setprecision(15);
        for (size_t k = 0; k < g_qLenTime[i].size(); ++k)
        {
            if (k)
            {
                vt << ' ';
                vv << ' ';
            }
            vt << g_qLenTime[i][k];
            vv << g_qLenVal[i][k];
        }
        f << g_qSampleNames[i] << ",queueLength:vector,\"" << vt.str() << "\",\"" << vv.str()
          << "\"\n";
        ++qwritten;
    }
    std::cout << "[nominal] wrote " << path << " (" << written << " clock vectors, " << qwritten
              << " queueLength vectors, opp_scavetool-schema)\n";
}

} // namespace

int
main(int argc, char* argv[])
{
    double simTime = 60.0; // P2d: normalized to OMNeT++'s 60s (was 30s); override with --simTime
    double syncIntervalMs = 125.0;  // INET default gPTP syncInterval
    double pdelayIntervalMs = 50.0; // peer-delay refresh
    double finalTolUs = 2.0;        // "final offset near 0" tolerance
    std::string resultDir = "";     // Phase 4: dir for vectors.csv (empty = skip)
    CommandLine cmd(__FILE__);
    cmd.AddValue("simTime", "Simulation duration (s)", simTime);
    cmd.AddValue("syncInterval", "gPTP Sync interval (ms)", syncIntervalMs);
    cmd.AddValue("pdelayInterval", "Peer-delay interval (ms)", pdelayIntervalMs);
    cmd.AddValue("finalTol", "Final-offset tolerance (us)", finalTolUs);
    cmd.AddValue("resultDir",
                 "Phase 4: directory to write vectors.csv (opp_scavetool schema) "
                 "for scripts/analyze.py; empty = skip (default)",
                 resultDir);
    std::string pcapPrefix = ""; // P2c: pcap capture prefix (empty = off, default)
    cmd.AddValue("pcapPrefix",
                 "P2c: enable pcap capture on every CSMA device (gPTP path) with this "
                 "file prefix; empty = off (default)",
                 pcapPrefix);
    cmd.Parse(argc, argv);

    // Deterministic. The only RNG use below is the 12 client drift draws; pinning
    // seed+run makes those reproducible (same pattern as Phase 0's smoke test).
    RngSeedManager::SetSeed(1);
    RngSeedManager::SetRun(1);

    // ---- Node ids / metadata -----------------------------------------------
    // 0=gm, 1=swCore, 2=coreClient, 3=swA, 4=swB, 5=swC,
    // 6..9=clientsA[0..3], 10..13=clientsB[0..3], 14..17=clientsC[0..3]
    constexpr int GM = 0, SWCORE = 1, CORECLIENT = 2, SWA = 3, SWB = 4, SWC = 5;
    constexpr int NNODES = 18;

    std::vector<NodeMeta> meta(NNODES);
    meta[GM] = {"gm", 0.0, 0};
    meta[SWCORE] = {"swCore", 50.0, 1};
    meta[CORECLIENT] = {"coreClient", 150.0, 2};
    meta[SWA] = {"swA", 80.0, 2};
    meta[SWB] = {"swB", -60.0, 2};
    meta[SWC] = {"swC", 100.0, 2};

    // 12 zone clients: uniform(-200,200) ppm, seeded ns-3 RNG (deterministic).
    Ptr<UniformRandomVariable> driftRv = CreateObject<UniformRandomVariable>();
    driftRv->SetAttribute("Min", DoubleValue(-200.0));
    driftRv->SetAttribute("Max", DoubleValue(200.0));
    const char* zoneLetters[3] = {"A", "B", "C"};
    for (int z = 0; z < 3; ++z)
    {
        for (int i = 0; i < 4; ++i)
        {
            int id = 6 + z * 4 + i;
            double ppm = driftRv->GetValue();
            meta[id] = {"clients" + std::string(zoneLetters[z]) + "[" + std::to_string(i) + "]",
                        ppm,
                        3};
        }
    }

    // ---- Nodes -------------------------------------------------------------
    NodeContainer nodes;
    nodes.Create(NNODES);

    CsmaHelper csma;
    csma.SetChannelAttribute("DataRate", StringValue("100Mbps"));
    csma.SetChannelAttribute("Delay", TimeValue(MicroSeconds(1)));

    // ---- Clocks + entities (stable storage) --------------------------------
    std::vector<std::unique_ptr<syncsim::Clock>> clocks(NNODES);
    std::vector<std::unique_ptr<syncsim::GptpEntity>> ent(NNODES);
    for (int id = 0; id < NNODES; ++id)
    {
        clocks[id] = std::make_unique<syncsim::Clock>(meta[id].ppm);
        ent[id] = std::make_unique<syncsim::GptpEntity>(meta[id].name, clocks[id].get(),
                                                        /*isGm=*/id == GM);
    }

    // ---- Link builder ------------------------------------------------------
    // Adds a CSMA link a<->b, registers a port on each entity (a's side isMaster
    // = aIsMaster, b's side is the opposite), and wires each device's receive
    // callback to the owning entity + its port index. aIsSwitch/bIsSwitch mark
    // switch egresses (P1b: collected for the queueLength:vector export, matching
    // congestion/feedback's switch set exactly -- swCore + swA/B/C, all ports).
    std::vector<Ptr<CsmaNetDevice>> switchDevs;
    std::vector<std::string> switchDevNames;
    auto qname = [&](int node, uint32_t port) {
        return "Nominal." + meta[node].name + ".eth" + std::to_string(port) + ".macLayer.queue";
    };
    auto link = [&](int a, int b, bool aIsMaster, bool aIsSwitch, bool bIsSwitch) {
        NetDeviceContainer dv = csma.Install(NodeContainer(nodes.Get(a), nodes.Get(b)));
        uint32_t pa = ent[a]->AddPort(dv.Get(0), dv.Get(1)->GetAddress(), aIsMaster);
        uint32_t pb = ent[b]->AddPort(dv.Get(1), dv.Get(0)->GetAddress(), !aIsMaster);
        dv.Get(0)->SetReceiveCallback(MakeBoundCallback(&RxTrampoline, ent[a].get(), pa));
        dv.Get(1)->SetReceiveCallback(MakeBoundCallback(&RxTrampoline, ent[b].get(), pb));
        if (aIsSwitch)
        {
            switchDevs.push_back(DynamicCast<CsmaNetDevice>(dv.Get(0)));
            switchDevNames.push_back(qname(a, pa));
        }
        if (bIsSwitch)
        {
            switchDevs.push_back(DynamicCast<CsmaNetDevice>(dv.Get(1)));
            switchDevNames.push_back(qname(b, pb));
        }
    };

    // gm -> swCore : gm is MASTER (sources Sync), swCore is SLAVE.
    link(GM, SWCORE, /*aIsMaster=*/true, false, true);
    // swCore -> its four children : swCore is MASTER on each.
    link(SWCORE, CORECLIENT, true, true, false);
    link(SWCORE, SWA, true, true, true);
    link(SWCORE, SWB, true, true, true);
    link(SWCORE, SWC, true, true, true);
    // Each zone switch -> its four clients : zone switch is MASTER on each.
    int zoneSw[3] = {SWA, SWB, SWC};
    for (int z = 0; z < 3; ++z)
    {
        for (int i = 0; i < 4; ++i)
        {
            int client = 6 + z * 4 + i;
            link(zoneSw[z], client, /*aIsMaster=*/true, true, false);
        }
    }

    // P1b: register the switch-egress queues for the queueLength:vector sampler.
    g_qSampleNames = switchDevNames;
    g_qSampleQueues.clear();
    for (auto& d : switchDevs)
    {
        g_qSampleQueues.push_back(d->GetQueue());
    }
    g_qLenTime.assign(switchDevs.size(), {});
    g_qLenVal.assign(switchDevs.size(), {});

    // ---- Offset trajectories (every non-GM node) ---------------------------
    for (int id = 1; id < NNODES; ++id)
    {
        ent[id]->ConnectOffsetTrace(MakeBoundCallback(&OffsetSink, id));
    }

    // ---- Drivers -----------------------------------------------------------
    Time pdelayInterval = MilliSeconds(pdelayIntervalMs);
    Time syncInterval = MilliSeconds(syncIntervalMs);
    // Every non-GM node initiates peer-delay on its slave port (bridges respond
    // as pure responders on their master ports). Start before the first Sync so
    // link delays are measured up front.
    for (int id = 1; id < NNODES; ++id)
    {
        Simulator::Schedule(MilliSeconds(5), &syncsim::GptpEntity::StartPdelay, ent[id].get(),
                            pdelayInterval);
    }
    // GM sources Sync every syncInterval.
    Simulator::Schedule(syncInterval, &syncsim::GptpEntity::SendSync, ent[GM].get(), syncInterval);

    // P1b: queueLength:vector sampling over the whole run (5 ms cadence), only
    // when --resultDir is set. Read-only, so unset => byte-identical to before.
    if (!resultDir.empty())
    {
        Simulator::Schedule(MilliSeconds(5), &SampleQueueLengths, MilliSeconds(5),
                            Seconds(simTime));
    }

    // P2c: opt-in pcap on every CSMA device (gPTP path). Off by default (empty
    // prefix) so gates/stdout are byte-identical when unset. Reuses Phase 0's
    // EnablePcapAll; captures this project's own GptpHeader wire format (verify
    // with ns3/scripts/check_pcap_gptp.py -- not Wireshark-dissectable 802.1AS).
    if (!pcapPrefix.empty())
    {
        csma.EnablePcapAll(pcapPrefix, false);
    }

    Simulator::Stop(Seconds(simTime));
    Simulator::Run();
    Simulator::Destroy();

    // ======================= Evidence reporting =============================
    std::cout << std::fixed;

    // Client drift draws (deterministic, seeded) -- print for reproducibility.
    std::cout << "\n[nominal] seeded client drift draws (ns-3 UniformRandomVariable, "
                 "seed=1 run=1):\n";
    std::cout << std::setprecision(2);
    for (int id = 6; id < NNODES; ++id)
    {
        std::cout << "  " << std::setw(14) << meta[id].name << " : " << std::setw(8) << meta[id].ppm
                  << " ppm\n";
    }

    // Per-node peak (max |offset|) and final offset.
    struct Stat
    {
        double peak;
        double finalOff;
        int hops;
        double ppm;
        std::string name;
        uint32_t servos;
    };
    std::vector<Stat> st;
    for (int id = 1; id < NNODES; ++id)
    {
        double peak = 0;
        double finalOff = 0;
        if (!g_traj[id].empty())
        {
            for (const auto& s : g_traj[id])
            {
                peak = std::max(peak, std::fabs(s.offset));
            }
            finalOff = g_traj[id].back().offset;
        }
        st.push_back({peak, finalOff, meta[id].hops, meta[id].ppm, meta[id].name,
                      ent[id]->GetServoCount()});
    }

    // ---- Per-node table ----------------------------------------------------
    std::cout << "\n[nominal] per-node offset-from-GM summary (local - reconstructed GM):\n";
    std::cout << std::string(74, '-') << "\n";
    std::cout << std::setw(15) << "node" << " | " << std::setw(4) << "hops" << " | " << std::setw(9)
              << "ppm" << " | " << std::setw(10) << "peak us" << " | " << std::setw(10) << "final us"
              << " | servos\n";
    std::cout << std::string(74, '-') << "\n";
    std::cout << std::setprecision(3);
    for (const auto& s : st)
    {
        std::cout << std::setw(15) << s.name << " | " << std::setw(4) << s.hops << " | "
                  << std::setw(9) << std::setprecision(1) << s.ppm << std::setprecision(3) << " | "
                  << std::setw(10) << s.peak << " | " << std::setw(10) << s.finalOff << " | "
                  << s.servos << "\n";
    }

    // ---- Hop-grouped table (INET's own reporting shape) --------------------
    std::cout << "\n[nominal] peak offset grouped by hop depth from GM "
                 "(INET's own reporting shape):\n";
    std::cout << std::string(56, '-') << "\n";
    std::cout << std::setw(6) << "hops" << " | " << std::setw(6) << "nodes" << " | " << std::setw(12)
              << "mean peak us" << " | " << std::setw(12) << "max peak us" << "\n";
    std::cout << std::string(56, '-') << "\n";
    std::map<int, std::vector<double>> peaksByHop;
    for (const auto& s : st)
    {
        peaksByHop[s.hops].push_back(s.peak);
    }
    for (const auto& [hops, peaks] : peaksByHop)
    {
        double sum = 0, mx = 0;
        for (double p : peaks)
        {
            sum += p;
            mx = std::max(mx, p);
        }
        std::cout << std::setw(6) << hops << " | " << std::setw(6) << peaks.size() << " | "
                  << std::setw(12) << (sum / peaks.size()) << " | " << std::setw(12) << mx << "\n";
    }
    std::cout << "  INET M2 reference (orientation, NOT a match target): hops=1 mean/max ~7.36;\n"
                 "  hops=2 mean ~12.19 max ~18.75; hops=3 mean ~8.40 max ~17.90 us.\n";

    // ---- A few representative peer delays (mechanism sanity) ---------------
    // Slave-side measured peer delay on: swCore(<->gm), swA(<->swCore),
    // clientsA[0](<->swA). Slave port is always port 0 on our bridges/leaves.
    Time dGmSwCore = ent[SWCORE]->GetLinkDelay(0);
    Time dCoreSwA = ent[SWA]->GetLinkDelay(0);
    Time dSwAClient = ent[6]->GetLinkDelay(0);
    std::cout << "\n[nominal] representative measured peer delays (1-step Pdelay, S1/S3):\n";
    std::cout << std::setprecision(3);
    std::cout << "  gm      <-> swCore        : " << dGmSwCore.GetSeconds() * 1e6 << " us\n";
    std::cout << "  swCore  <-> swA           : " << dCoreSwA.GetSeconds() * 1e6 << " us\n";
    std::cout << "  swA     <-> clientsA[0]   : " << dSwAClient.GetSeconds() * 1e6 << " us\n";

    // ======================= Gate checks ====================================
    // Every one of the 18 nodes' offset converges near 0 across all hop depths.
    bool finalOk = true;
    bool loopOk = true;
    for (const auto& s : st)
    {
        if (std::fabs(s.finalOff) >= finalTolUs)
        {
            finalOk = false;
        }
        if (s.servos <= 10)
        {
            loopOk = false;
        }
    }
    // Peer delays sane: positive and small (< 100 us) on the sampled links.
    auto sane = [](Time d) { return d.GetSeconds() > 0 && d.GetSeconds() < 100e-6; };
    bool delayOk = sane(dGmSwCore) && sane(dCoreSwA) && sane(dSwAClient);
    // Every traced node actually produced samples (the loop reached all depths).
    bool coverageOk = true;
    for (int id = 1; id < NNODES; ++id)
    {
        if (g_traj[id].empty())
        {
            coverageOk = false;
        }
    }

    bool pass = finalOk && loopOk && delayOk && coverageOk;

    std::cout << "\n[nominal] Gate checks (M2 / R-BRIDGE):\n";
    std::cout << "  [" << (finalOk ? "PASS" : "FAIL")
              << "] every one of the 18 nodes' final offset near 0 (|.| < " << finalTolUs
              << " us)\n";
    std::cout << "  [" << (coverageOk ? "PASS" : "FAIL")
              << "] all three hop depths reached (every traced node produced samples)\n";
    std::cout << "  [" << (delayOk ? "PASS" : "FAIL")
              << "] representative peer delays measured, positive and small\n";
    std::cout << "  [" << (loopOk ? "PASS" : "FAIL")
              << "] servo closed the loop at every node (>10 corrections)\n";
    std::cout << "\n[nominal] "
              << (pass ? "GATE PASS: unchanged Phase-2 gPTP generalizes to a multi-hop chain "
                         "of time-aware bridges on ns-3.45"
                       : "GATE FAIL")
              << std::endl;

    // ---- Phase 4: optional CSV export for scripts/analyze.py ----------------
    if (!resultDir.empty())
    {
        // std::ofstream silently fails (no exception, just a bad stream) if the
        // target directory doesn't exist yet -- found during independent
        // verification of this phase's work (a WARN was printed and the file
        // never appeared). scripts/run.sh's mkdir -p "$RESULT_DIR" for the
        // OMNeT++ path does this implicitly; do the ns-3 equivalent here so
        // --resultDir doesn't require the caller to pre-create the directory.
        std::filesystem::create_directories(resultDir);
        WriteVectorsCsv(resultDir, meta, g_traj);
    }

    return pass ? 0 : 1;
}
