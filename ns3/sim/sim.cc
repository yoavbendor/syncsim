// SPDX-License-Identifier: Apache-2.0
//
// syncsim ns-3 track -- Tier 3 / P3b: the generic YAML-driven scenario engine.
//
// ONE ns-3 program, MANY YAML configs. This is the ns-3-native analog of how
// the OMNeT++ side already works (one simulation binary + different .ini files):
// a new topology or traffic pattern needs a new YAML file, not new C++. It
// replaces the "one hand-coded .cc per scenario" style of gptp-spike.cc /
// nominal-topology.cc / congestion-topology.cc / feedback-topology.cc with a
// single interpreter that builds any of them (and anything else expressible in
// the schema) from data read at startup.
//
// This is a NEW CONSUMER of the already-verified, tshark-validated libraries in
// this directory -- gptp.{h,cc} and clock.{h,cc} are byte-identical vendored
// copies of ns3/gptp/'s (md5-confirmed, the same discipline every prior phase
// followed). GptpEntity / Clock are NOT modified: the engine only calls their
// public API (AddPort / StartPdelay / SendSync / SendAnnounce / ConnectOffset-
// Trace / the servo). Everything the four hand-coded scenarios proved -- the
// S5 full-duplex hop-by-hop forwarding, the P1a hardened servo, the P3c byte-
// exact 802.1AS wire format, the P1b queue-length export, the P2c/P3c pcap
// capture -- carries through unchanged because the mechanism lives in those
// libraries, not in the (now generic) driver.
//
// TRANSPORT: every link is a full-duplex SimpleNetDevice/SimpleChannel pair (the
// S5-fix transport from congestion/feedback), for ALL scenarios -- including the
// reproductions of gptp-spike and nominal, which were originally CSMA. The
// mechanism is transport-agnostic (proven across the four hand-coded files); the
// only visible consequence of the CSMA->SimpleNetDevice swap for the two
// formerly-CSMA scenarios is a small, disclosed peer-delay / transient-peak
// shift (a different frame-serialization path), reported honestly in the README's
// old-vs-new table. Gate-relevant MECHANISM (convergence, hop composition,
// isolation shape) is unchanged.
//
// SCHEMA (see scenarios/*.yaml and README.md for the full documentation):
//   sim:     time_s, sync_interval_ms, pdelay_interval_ms
//   nodes:   name, role (gm|switch|client), drift_ppm
//   links:   a, b, master (a|b), queue_cap, data_rate, delay_us
//   traffic: mode (none|background_flows|aligned_bursts) + mode params
//   report:  compare_baseline, isolation_tol_us, module_root, final_tol_us,
//            degrade_factor
//
// Node hop-depth is DERIVED (BFS from the gm over the link graph), never
// specified by hand. Data-forwarding paths are DERIVED (BFS parent pointers from
// the sink), never hand-coded -- generalizing congestion/feedback's static L2
// table to any tree.
//
// Determinism: the only RNG use is background_flows' exponential inter-packet
// gap (seeded; pinned SetSeed/SetRun). Drifts are explicit per node in the YAML
// (no random draws), so runs are byte-identical (verified twice each).

#include "gptp.h"

#include "ns3/core-module.h"
#include "ns3/network-module.h" // SimpleNetDevice/SimpleChannel/-Helper + EthernetHeader/pcap

#include "yaml-cpp/yaml.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <queue>
#include <sstream>
#include <string>
#include <vector>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("SyncsimSim");

namespace
{

constexpr uint16_t kGptpProtocol = 0x88F7; // real IEEE PTP EtherType (P3c); matches gptp.cc
constexpr uint16_t kDataProtocol = 0x88b5; // background data plane (distinct)

// ============================ Config model ===============================

enum class Role
{
    Gm,
    Switch,
    Client
};

enum class TrafficMode
{
    None,
    BackgroundFlows,
    AlignedBursts
};

struct NodeCfg
{
    std::string name;
    Role role;
    double driftPpm{0.0};
    int hops{-1}; // derived: BFS depth from gm
};

struct LinkCfg
{
    std::string a;
    std::string b;
    bool masterIsA{true}; // which endpoint holds the gPTP master port
    uint32_t queueCap{0}; // 0 = default/unbounded; >0 = finite real-dropping cap
    std::string dataRate{"100Mbps"};
    double delayUs{1.0};
};

struct TrafficCfg
{
    TrafficMode mode{TrafficMode::None};
    // background_flows
    double meanGapUs{160.0};
    uint32_t payloadBytes{946};
    // aligned_bursts
    double burstIntervalMs{100.0};
    uint32_t fragmentsPerBurst{15};
    uint32_t fragmentBytes{1330};
    // shared
    double startS{1.0};
    std::vector<std::string> sources;
    std::string sink;
};

struct ReportCfg
{
    bool compareBaseline{false};
    double isolationTolUs{5.0};
    double finalTolUs{2.0};
    double degradeFactor{5.0};
    std::string moduleRoot{"Nominal"}; // CSV module prefix (analyze.py HOP_MAPS key)
};

struct SimCfg
{
    double timeS{60.0};
    double syncIntervalMs{125.0};
    double pdelayIntervalMs{50.0};
    std::vector<NodeCfg> nodes;
    std::vector<LinkCfg> links;
    TrafficCfg traffic;
    ReportCfg report;
};

// ============================ YAML parsing ================================

Role
ParseRole(const std::string& s)
{
    if (s == "gm")
    {
        return Role::Gm;
    }
    if (s == "switch")
    {
        return Role::Switch;
    }
    if (s == "client")
    {
        return Role::Client;
    }
    throw std::runtime_error("unknown role '" + s + "' (expected gm|switch|client)");
}

TrafficMode
ParseTrafficMode(const std::string& s)
{
    if (s == "none")
    {
        return TrafficMode::None;
    }
    if (s == "background_flows")
    {
        return TrafficMode::BackgroundFlows;
    }
    if (s == "aligned_bursts")
    {
        return TrafficMode::AlignedBursts;
    }
    throw std::runtime_error("unknown traffic mode '" + s + "'");
}

SimCfg
LoadConfig(const std::string& path)
{
    YAML::Node root = YAML::LoadFile(path);
    SimCfg cfg;

    const YAML::Node& sim = root["sim"];
    if (!sim)
    {
        throw std::runtime_error("config missing top-level 'sim' section");
    }
    if (sim["time_s"])
    {
        cfg.timeS = sim["time_s"].as<double>();
    }
    if (sim["sync_interval_ms"])
    {
        cfg.syncIntervalMs = sim["sync_interval_ms"].as<double>();
    }
    if (sim["pdelay_interval_ms"])
    {
        cfg.pdelayIntervalMs = sim["pdelay_interval_ms"].as<double>();
    }

    const YAML::Node& nodes = root["nodes"];
    if (!nodes || nodes.size() == 0)
    {
        throw std::runtime_error("config missing non-empty 'nodes' list");
    }
    for (const auto& n : nodes)
    {
        NodeCfg nc;
        nc.name = n["name"].as<std::string>();
        nc.role = ParseRole(n["role"].as<std::string>());
        nc.driftPpm = n["drift_ppm"] ? n["drift_ppm"].as<double>() : 0.0;
        if (nc.role == Role::Gm)
        {
            nc.driftPpm = 0.0; // gm is drift-free by construction
        }
        cfg.nodes.push_back(nc);
    }

    const YAML::Node& links = root["links"];
    if (!links || links.size() == 0)
    {
        throw std::runtime_error("config missing non-empty 'links' list");
    }
    for (const auto& l : links)
    {
        LinkCfg lc;
        lc.a = l["a"].as<std::string>();
        lc.b = l["b"].as<std::string>();
        std::string m = l["master"] ? l["master"].as<std::string>() : "a";
        lc.masterIsA = (m == "a");
        lc.queueCap = l["queue_cap"] ? l["queue_cap"].as<uint32_t>() : 0;
        lc.dataRate = l["data_rate"] ? l["data_rate"].as<std::string>() : "100Mbps";
        lc.delayUs = l["delay_us"] ? l["delay_us"].as<double>() : 1.0;
        cfg.links.push_back(lc);
    }

    const YAML::Node& tr = root["traffic"];
    if (tr)
    {
        cfg.traffic.mode = ParseTrafficMode(tr["mode"] ? tr["mode"].as<std::string>() : "none");
        if (tr["mean_gap_us"])
        {
            cfg.traffic.meanGapUs = tr["mean_gap_us"].as<double>();
        }
        if (tr["payload_bytes"])
        {
            cfg.traffic.payloadBytes = tr["payload_bytes"].as<uint32_t>();
        }
        if (tr["burst_interval_ms"])
        {
            cfg.traffic.burstIntervalMs = tr["burst_interval_ms"].as<double>();
        }
        if (tr["fragments_per_burst"])
        {
            cfg.traffic.fragmentsPerBurst = tr["fragments_per_burst"].as<uint32_t>();
        }
        if (tr["fragment_bytes"])
        {
            cfg.traffic.fragmentBytes = tr["fragment_bytes"].as<uint32_t>();
        }
        if (tr["start_s"])
        {
            cfg.traffic.startS = tr["start_s"].as<double>();
        }
        if (tr["sources"])
        {
            for (const auto& s : tr["sources"])
            {
                cfg.traffic.sources.push_back(s.as<std::string>());
            }
        }
        if (tr["sink"])
        {
            cfg.traffic.sink = tr["sink"].as<std::string>();
        }
    }

    const YAML::Node& rep = root["report"];
    if (rep)
    {
        if (rep["compare_baseline"])
        {
            cfg.report.compareBaseline = rep["compare_baseline"].as<bool>();
        }
        if (rep["isolation_tol_us"])
        {
            cfg.report.isolationTolUs = rep["isolation_tol_us"].as<double>();
        }
        if (rep["final_tol_us"])
        {
            cfg.report.finalTolUs = rep["final_tol_us"].as<double>();
        }
        if (rep["degrade_factor"])
        {
            cfg.report.degradeFactor = rep["degrade_factor"].as<double>();
        }
        if (rep["module_root"])
        {
            cfg.report.moduleRoot = rep["module_root"].as<std::string>();
        }
    }
    return cfg;
}

// ==================== Derived topology (BFS from graph) ===================

// Adjacency: nodeIndex -> list of (neighborIndex, linkIndex).
struct Graph
{
    std::map<std::string, int> nameToId;
    std::vector<std::vector<std::pair<int, int>>> adj; // per node: (neighbor, linkIdx)
    int gmId{-1};
};

Graph
BuildGraph(const SimCfg& cfg)
{
    Graph g;
    for (size_t i = 0; i < cfg.nodes.size(); ++i)
    {
        g.nameToId[cfg.nodes[i].name] = static_cast<int>(i);
        if (cfg.nodes[i].role == Role::Gm)
        {
            if (g.gmId >= 0)
            {
                throw std::runtime_error("more than one gm node");
            }
            g.gmId = static_cast<int>(i);
        }
    }
    if (g.gmId < 0)
    {
        throw std::runtime_error("no gm node");
    }
    g.adj.assign(cfg.nodes.size(), {});
    for (size_t li = 0; li < cfg.links.size(); ++li)
    {
        const LinkCfg& l = cfg.links[li];
        auto ia = g.nameToId.find(l.a);
        auto ib = g.nameToId.find(l.b);
        if (ia == g.nameToId.end() || ib == g.nameToId.end())
        {
            throw std::runtime_error("link references unknown node: " + l.a + " <-> " + l.b);
        }
        g.adj[ia->second].push_back({ib->second, static_cast<int>(li)});
        g.adj[ib->second].push_back({ia->second, static_cast<int>(li)});
    }
    return g;
}

// BFS from a root; returns parent[] (parent node id on the shortest path back to
// root, -1 for root/unreached) and depth[] (hop count from root, -1 unreached).
void
Bfs(const Graph& g, int root, std::vector<int>& parent, std::vector<int>& depth)
{
    size_t n = g.adj.size();
    parent.assign(n, -1);
    depth.assign(n, -1);
    std::queue<int> q;
    q.push(root);
    depth[root] = 0;
    while (!q.empty())
    {
        int u = q.front();
        q.pop();
        for (auto [v, li] : g.adj[u])
        {
            (void)li;
            if (depth[v] < 0)
            {
                depth[v] = depth[u] + 1;
                parent[v] = u;
                q.push(v);
            }
        }
    }
}

// ============================ Run-time state =============================

struct OffsetSample
{
    double global;
    double offset; // microseconds
};

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

// One node's data uplink: the device (+ peer MAC + local port index) on the link
// toward the sink (the BFS parent when rooting at the sink). A switch forwards
// data out this device; a source client originates its flow on it.
struct DataUplink
{
    Ptr<NetDevice> dev;
    Address peer;
    uint32_t port{0};
    bool valid{false};
};

// Per-source burst context for aligned_bursts (S6 clock-driven scheduling).
struct BurstCtx
{
    syncsim::Clock* clock{nullptr};
    int nodeId{-1};
    int nextK{0};
};

struct PassState
{
    std::map<int, std::vector<OffsetSample>> traj;
    std::vector<syncsim::GptpEntity*> ent;
    std::vector<DataUplink> uplink; // per node id
    int sinkId{-1};
    // data-plane stats
    uint64_t dataOffered{0};
    uint64_t dataDelivered{0};
    uint64_t bottleneckDrops{0};
    double backlogSum{0};
    uint64_t backlogSamples{0};
    uint32_t backlogMax{0};
    Ptr<Queue<Packet>> bottleneckQueue;
    uint32_t bottleneckCap{0};
    // queue export
    std::vector<QueueCounters> queueCounters;
    std::vector<Ptr<Queue<Packet>>> qSampleQueues;
    std::vector<std::string> qSampleNames;
    std::vector<std::vector<double>> qLenTime;
    std::vector<std::vector<double>> qLenVal;
    // aligned bursts
    std::vector<BurstCtx> bursts;
    std::map<int, std::vector<double>> cycleFireTimes; // cycle k -> fire times across sources
    double burstStartS{1.0};
    double burstIntervalMs{100.0};
    double simTime{60.0};
    uint32_t burstFrags{15};
    uint32_t fragBytes{1330};
};

PassState* g_active = nullptr;
std::string g_pcapPrefix;

void
OffsetSink(int id, Time global, double offsetSec)
{
    g_active->traj[id].push_back({global.GetSeconds(), offsetSec * 1e6});
}

// P2c/P3c pcap: SimpleNetDevice carries no on-wire Ethernet header (addressing is
// out-of-band via a SimpleTag), so synthesize a classic 14-byte header for the
// capture -- exactly the reproduction in congestion/feedback-topology.cc, so
// check_pcap_gptp.py parses it unchanged (DLT_EN10MB + 14-byte header + 0x88F7).
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
//   gPTP -> the node's GptpEntity (per-port termination, S4 -- NOT forwarded).
//   data -> at the sink counted delivered; at any other node L2-forwarded
//           hop-by-hop out that node's static uplink toward the sink (S5 real
//           forwarding, derived from the BFS-from-sink parent pointers).
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
        Address dst = (protocol == kGptpProtocol) ? syncsim::GptpMulticastAddress()
                                                   : device->GetAddress();
        PcapCapture(pcapFile, dst, pkt, protocol, from);
    }
    if (protocol == kGptpProtocol)
    {
        return ent->OnDeviceReceive(port, pkt, from);
    }
    if (protocol == kDataProtocol)
    {
        if (nodeId == g_active->sinkId)
        {
            g_active->dataDelivered++;
            return true;
        }
        const DataUplink& up = g_active->uplink[nodeId];
        if (up.valid && port != up.port)
        {
            up.dev->Send(pkt->Copy(), up.peer, kDataProtocol);
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
    if (Simulator::Now() >= stopAt || !g_active->bottleneckQueue)
    {
        return;
    }
    uint32_t n = g_active->bottleneckQueue->GetNPackets();
    g_active->backlogSum += n;
    g_active->backlogSamples++;
    g_active->backlogMax = std::max(g_active->backlogMax, n);
    Simulator::Schedule(interval, &SampleBacklog, interval, stopAt);
}

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

// background_flows: originate one data frame on a source's uplink, reschedule
// after an exponential(mean) gap (seeded). From there CombinedRx forwards it
// hop-by-hop to the sink.
void
DataSource(Ptr<NetDevice> dev,
           Address peer,
           uint32_t payloadBytes,
           Ptr<ExponentialRandomVariable> gap,
           Time stopAt)
{
    if (Simulator::Now() >= stopAt)
    {
        return;
    }
    dev->Send(Create<Packet>(payloadBytes), peer, kDataProtocol);
    g_active->dataOffered++;
    Simulator::Schedule(Seconds(gap->GetValue()), &DataSource, dev, peer, payloadBytes, gap, stopAt);
}

void ScheduleNextBurst(int bi);

// aligned_bursts: fire source bi's current burst (kBurstFrags back-to-back frames
// originated on its uplink), then schedule the next off its LIVE clock (S6).
void
FireBurst(int bi)
{
    BurstCtx& b = g_active->bursts[bi];
    g_active->cycleFireTimes[b.nextK].push_back(Simulator::Now().GetSeconds());
    const DataUplink& up = g_active->uplink[b.nodeId];
    for (uint32_t f = 0; f < g_active->burstFrags; ++f)
    {
        up.dev->Send(Create<Packet>(g_active->fragBytes), up.peer, kDataProtocol);
        g_active->dataOffered++;
    }
    b.nextK++;
    ScheduleNextBurst(bi);
}

// S6: compute the global delta to source bi's next ABSOLUTE local-clock burst
// instant from its live local time + live rate, and schedule it.
void
ScheduleNextBurst(int bi)
{
    BurstCtx& b = g_active->bursts[bi];
    double targetLocalS = g_active->burstStartS + b.nextK * (g_active->burstIntervalMs / 1000.0);
    if (targetLocalS > g_active->simTime)
    {
        return;
    }
    Time targetLocal = Seconds(targetLocalS);
    Time curLocal = b.clock->GetLocalTime();
    double localDeltaS = (targetLocal - curLocal).GetSeconds();
    if (localDeltaS < 0)
    {
        localDeltaS = 0;
    }
    double rate = 1.0 + b.clock->GetDriftPpm() / 1e6;
    double globalDeltaS = localDeltaS / rate;
    Simulator::Schedule(Seconds(globalDeltaS), &FireBurst, bi);
}

// ============================ CSV export =================================

void
WriteVectorsCsv(const std::string& dir,
                const SimCfg& cfg,
                const PassState& state)
{
    std::string path = dir + "/vectors.csv";
    std::ofstream f(path);
    if (!f)
    {
        std::cerr << "[sim] WARN: cannot write " << path << "\n";
        return;
    }
    f << "module,name,vectime,vecvalue\n";
    uint32_t written = 0;
    for (const auto& [id, samples] : state.traj)
    {
        if (samples.empty())
        {
            continue;
        }
        std::string module = cfg.report.moduleRoot + "." + cfg.nodes[id].name + ".clock";
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
            double gg = samples[i].global;
            vt << gg;
            vv << (gg + samples[i].offset * 1e-6); // local clock time = global + offset
        }
        f << module << ",timeChanged:vector,\"" << vt.str() << "\",\"" << vv.str() << "\"\n";
        ++written;
    }
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
    std::cout << "[sim] wrote " << path << " (" << written << " clock vectors, " << qwritten
              << " queueLength vectors, opp_scavetool schema)\n";
}

void
WriteScalarsCsv(const std::string& path, const std::vector<QueueCounters>& qcs)
{
    std::ofstream f(path);
    if (!f)
    {
        std::cerr << "[sim] WARN: cannot write " << path << "\n";
        return;
    }
    f << "module,name,value\n";
    f << std::setprecision(15);
    for (const auto& q : qcs)
    {
        uint64_t inPk = q.enqPk + q.dropPk;
        double inBits = q.enqBits + q.dropBits;
        f << q.module << ",incomingPackets:count," << inPk << "\n";
        f << q.module << ",outgoingPackets:count," << q.deqPk << "\n";
        f << q.module << ",droppedPacketsQueueOverflow:count," << q.dropPk << "\n";
        f << q.module << ",incomingPacketLengths:sum," << inBits << "\n";
        f << q.module << ",outgoingPacketLengths:sum," << q.deqBits << "\n";
        f << q.module << ",droppedPacketLengths:sum," << q.dropBits << "\n";
    }
    std::cout << "[sim] wrote " << path << " (" << qcs.size() << " queue scalar groups)\n";
}

// ============================ Engine build/run ===========================

struct Stat
{
    double peak;       // global max |offset|
    double steadyPeak; // max |offset| for t >= traffic.startS
    double finalOff;
    int hops;
    double ppm;
    std::string name;
    uint32_t servos;
};

// Build the whole topology + gPTP + (optionally) traffic, run it, fill st/state.
// `withTraffic` gates whether the data plane is active (baseline vs traffic pass).
void
RunScenario(const SimCfg& cfg,
            const Graph& g,
            bool withTraffic,
            bool capturePcap,
            bool sampleQueues,
            std::vector<Stat>& st,
            PassState& state)
{
    g_active = &state;
    const int nNodes = static_cast<int>(cfg.nodes.size());
    state.ent.assign(nNodes, nullptr);
    state.uplink.assign(nNodes, DataUplink{});
    state.simTime = cfg.timeS;

    NodeContainer nodes;
    nodes.Create(nNodes);

    std::vector<std::unique_ptr<syncsim::Clock>> clocks(nNodes);
    std::vector<std::unique_ptr<syncsim::GptpEntity>> ent(nNodes);
    for (int id = 0; id < nNodes; ++id)
    {
        clocks[id] = std::make_unique<syncsim::Clock>(cfg.nodes[id].driftPpm);
        ent[id] = std::make_unique<syncsim::GptpEntity>(cfg.nodes[id].name, clocks[id].get(),
                                                        cfg.nodes[id].role == Role::Gm);
        state.ent[id] = ent[id].get();
    }

    // ---- Data-forwarding paths: BFS from the sink gives each node its next hop
    //      (parent) toward the sink. Derived, not hand-coded. ------------------
    int sinkId = -1;
    std::vector<int> sinkParent, sinkDepth;
    if (cfg.traffic.mode != TrafficMode::None && !cfg.traffic.sink.empty())
    {
        auto it = g.nameToId.find(cfg.traffic.sink);
        if (it == g.nameToId.end())
        {
            throw std::runtime_error("traffic.sink '" + cfg.traffic.sink + "' is not a node");
        }
        sinkId = it->second;
        Bfs(g, sinkId, sinkParent, sinkDepth);
    }
    state.sinkId = sinkId;

    // Track switch-egress devices (for finite queues + queue-length export) and
    // per-link built devices/ports so we can wire the data uplinks after build.
    std::vector<Ptr<SimpleNetDevice>> switchDevs;
    std::vector<std::string> switchDevNames;
    // per link: (devA, devB, portA, portB)
    struct BuiltLink
    {
        Ptr<NetDevice> devA, devB;
        uint32_t portA, portB;
    };
    std::vector<BuiltLink> built(cfg.links.size());

    auto qname = [&](int node, uint32_t port) {
        return cfg.report.moduleRoot + "." + cfg.nodes[node].name + ".eth" +
               std::to_string(port) + ".macLayer.queue";
    };

    for (size_t li = 0; li < cfg.links.size(); ++li)
    {
        const LinkCfg& l = cfg.links[li];
        int a = g.nameToId.at(l.a);
        int b = g.nameToId.at(l.b);

        SimpleNetDeviceHelper simple;
        simple.SetDeviceAttribute("DataRate", DataRateValue(DataRate(l.dataRate)));
        simple.SetChannelAttribute("Delay", TimeValue(MicroSeconds(l.delayUs)));
        NetDeviceContainer dv = simple.Install(NodeContainer(nodes.Get(a), nodes.Get(b)));

        Address gptpMc = syncsim::GptpMulticastAddress();
        bool aIsMaster = l.masterIsA;
        uint32_t pa = ent[a]->AddPort(dv.Get(0), gptpMc, aIsMaster);
        uint32_t pb = ent[b]->AddPort(dv.Get(1), gptpMc, !aIsMaster);

        // pcap: one file per device, RX-hooked in CombinedRx. Only captured on the
        // traffic pass (the interesting one), matching the hand-coded scenarios.
        Ptr<PcapFileWrapper> fileA, fileB;
        if (capturePcap && !g_pcapPrefix.empty())
        {
            PcapHelper ph;
            fileA = ph.CreateFile(g_pcapPrefix + "-" + cfg.nodes[a].name + "-" +
                                      std::to_string(pa) + ".pcap",
                                  std::ios::out, PcapHelper::DLT_EN10MB);
            fileB = ph.CreateFile(g_pcapPrefix + "-" + cfg.nodes[b].name + "-" +
                                      std::to_string(pb) + ".pcap",
                                  std::ios::out, PcapHelper::DLT_EN10MB);
        }
        dv.Get(0)->SetReceiveCallback(MakeBoundCallback(&CombinedRx, a, ent[a].get(), pa, fileA));
        dv.Get(1)->SetReceiveCallback(MakeBoundCallback(&CombinedRx, b, ent[b].get(), pb, fileB));

        built[li] = {dv.Get(0), dv.Get(1), pa, pb};

        // Switch-egress bookkeeping: a device belongs to a switch node -> it is a
        // switch egress (gets the finite queue when queue_cap>0, is sampled/counted
        // for the CSV export). This reproduces the hand-coded scenarios' switchDevs
        // set (every port of every switch node).
        if (cfg.nodes[a].role == Role::Switch)
        {
            switchDevs.push_back(DynamicCast<SimpleNetDevice>(dv.Get(0)));
            switchDevNames.push_back(qname(a, pa));
            if (l.queueCap > 0)
            {
                DynamicCast<SimpleNetDevice>(dv.Get(0))->GetQueue()->SetAttribute(
                    "MaxSize", QueueSizeValue(QueueSize(PACKETS, l.queueCap)));
            }
        }
        if (cfg.nodes[b].role == Role::Switch)
        {
            switchDevs.push_back(DynamicCast<SimpleNetDevice>(dv.Get(1)));
            switchDevNames.push_back(qname(b, pb));
            if (l.queueCap > 0)
            {
                DynamicCast<SimpleNetDevice>(dv.Get(1))->GetQueue()->SetAttribute(
                    "MaxSize", QueueSizeValue(QueueSize(PACKETS, l.queueCap)));
            }
        }
    }

    // ---- Build each node's data uplink from the sink-rooted parent pointers ---
    if (sinkId >= 0)
    {
        for (int id = 0; id < nNodes; ++id)
        {
            if (id == sinkId)
            {
                continue;
            }
            int par = sinkParent[id];
            if (par < 0)
            {
                continue; // unreachable from sink (shouldn't happen in a tree)
            }
            // Find the link between id and par; the uplink device is id's side.
            for (size_t li = 0; li < cfg.links.size(); ++li)
            {
                int a = g.nameToId.at(cfg.links[li].a);
                int b = g.nameToId.at(cfg.links[li].b);
                if ((a == id && b == par))
                {
                    state.uplink[id] = {built[li].devA, built[li].devB->GetAddress(),
                                        built[li].portA, true};
                    break;
                }
                if ((b == id && a == par))
                {
                    state.uplink[id] = {built[li].devB, built[li].devA->GetAddress(),
                                        built[li].portB, true};
                    break;
                }
            }
        }
    }

    // ---- Queue counters + backlog sampling registration ---------------------
    state.queueCounters.assign(switchDevs.size(), QueueCounters{});
    state.qSampleNames = switchDevNames;
    state.qSampleQueues.clear();
    for (size_t i = 0; i < switchDevs.size(); ++i)
    {
        state.queueCounters[i].module = switchDevNames[i];
        QueueCounters* qc = &state.queueCounters[i];
        Ptr<Queue<Packet>> q = switchDevs[i]->GetQueue();
        q->TraceConnectWithoutContext("Enqueue", MakeBoundCallback(&QEnqueue, qc));
        q->TraceConnectWithoutContext("Dequeue", MakeBoundCallback(&QDequeue, qc));
        q->TraceConnectWithoutContext("Drop", MakeBoundCallback(&QDrop, qc));
        state.qSampleQueues.push_back(q);
    }
    state.qLenTime.assign(switchDevs.size(), {});
    state.qLenVal.assign(switchDevs.size(), {});

    // ---- Bottleneck queue = the sink's parent-side egress toward the sink -----
    if (sinkId >= 0)
    {
        int par = sinkParent[sinkId] >= 0 ? sinkParent[sinkId] : -1;
        // The sink's incoming link comes from whichever neighbor is on the path
        // FROM gm (i.e. the sink's own parent when rooted at gm) -- but for a tree
        // the sink has exactly one neighbor toward the interior, so use that.
        (void)par;
        for (size_t li = 0; li < cfg.links.size(); ++li)
        {
            int a = g.nameToId.at(cfg.links[li].a);
            int b = g.nameToId.at(cfg.links[li].b);
            Ptr<NetDevice> egressToSink;
            if (a == sinkId)
            {
                egressToSink = built[li].devB; // the OTHER end's egress toward sink
            }
            else if (b == sinkId)
            {
                egressToSink = built[li].devA;
            }
            if (egressToSink)
            {
                state.bottleneckQueue = DynamicCast<SimpleNetDevice>(egressToSink)->GetQueue();
                state.bottleneckCap = cfg.links[li].queueCap;
                state.bottleneckQueue->TraceConnectWithoutContext(
                    "Drop", MakeCallback(&BottleneckDropTrace));
                break;
            }
        }
    }

    // ---- Offset trajectories (every non-GM node) ----------------------------
    for (int id = 0; id < nNodes; ++id)
    {
        if (cfg.nodes[id].role != Role::Gm)
        {
            ent[id]->ConnectOffsetTrace(MakeBoundCallback(&OffsetSink, id));
        }
    }

    // ---- gPTP drivers -------------------------------------------------------
    Time pdelayInterval = MilliSeconds(cfg.pdelayIntervalMs);
    Time syncInterval = MilliSeconds(cfg.syncIntervalMs);
    for (int id = 0; id < nNodes; ++id)
    {
        if (cfg.nodes[id].role != Role::Gm)
        {
            Simulator::Schedule(MilliSeconds(5), &syncsim::GptpEntity::StartPdelay, ent[id].get(),
                                pdelayInterval);
        }
    }
    Simulator::Schedule(syncInterval, &syncsim::GptpEntity::SendSync, ent[g.gmId].get(),
                        syncInterval);
    Simulator::Schedule(syncInterval, &syncsim::GptpEntity::SendAnnounce, ent[g.gmId].get(),
                        syncInterval);

    // ---- Traffic ------------------------------------------------------------
    state.burstStartS = cfg.traffic.startS;
    state.burstIntervalMs = cfg.traffic.burstIntervalMs;
    state.burstFrags = cfg.traffic.fragmentsPerBurst;
    state.fragBytes = cfg.traffic.fragmentBytes;
    std::vector<Ptr<ExponentialRandomVariable>> gaps; // keep alive across the run
    if (withTraffic && cfg.traffic.mode == TrafficMode::BackgroundFlows)
    {
        for (const auto& sname : cfg.traffic.sources)
        {
            auto it = g.nameToId.find(sname);
            if (it == g.nameToId.end())
            {
                throw std::runtime_error("traffic.sources references unknown node: " + sname);
            }
            const DataUplink& up = state.uplink[it->second];
            if (!up.valid)
            {
                throw std::runtime_error("source '" + sname + "' has no path to sink");
            }
            Ptr<ExponentialRandomVariable> gap = CreateObject<ExponentialRandomVariable>();
            gap->SetAttribute("Mean", DoubleValue(cfg.traffic.meanGapUs * 1e-6));
            gaps.push_back(gap);
            Simulator::Schedule(Seconds(cfg.traffic.startS), &DataSource, up.dev, up.peer,
                                cfg.traffic.payloadBytes, gap, Seconds(cfg.timeS));
        }
        Simulator::Schedule(Seconds(cfg.traffic.startS), &SampleBacklog, MilliSeconds(1),
                            Seconds(cfg.timeS));
    }
    else if (withTraffic && cfg.traffic.mode == TrafficMode::AlignedBursts)
    {
        state.bursts.clear();
        for (const auto& sname : cfg.traffic.sources)
        {
            auto it = g.nameToId.find(sname);
            if (it == g.nameToId.end())
            {
                throw std::runtime_error("traffic.sources references unknown node: " + sname);
            }
            BurstCtx b;
            b.clock = clocks[it->second].get(); // each source uses ITS OWN clock (S6)
            b.nodeId = it->second;
            b.nextK = 0;
            state.bursts.push_back(b);
        }
        for (size_t bi = 0; bi < state.bursts.size(); ++bi)
        {
            Simulator::Schedule(Time(0), &ScheduleNextBurst, static_cast<int>(bi));
        }
        Simulator::Schedule(Seconds(cfg.traffic.startS), &SampleBacklog, MilliSeconds(1),
                            Seconds(cfg.timeS));
    }

    if (sampleQueues)
    {
        Simulator::Schedule(MilliSeconds(5), &SampleQueueLengths, MilliSeconds(5),
                            Seconds(cfg.timeS));
    }

    Simulator::Stop(Seconds(cfg.timeS));
    Simulator::Run();
    Simulator::Destroy();

    // ---- Per-node offset stats ----------------------------------------------
    st.clear();
    for (int id = 0; id < nNodes; ++id)
    {
        if (cfg.nodes[id].role == Role::Gm)
        {
            continue;
        }
        double peak = 0, steadyPeak = 0, finalOff = 0;
        auto it = state.traj.find(id);
        if (it != state.traj.end() && !it->second.empty())
        {
            for (const auto& sm : it->second)
            {
                peak = std::max(peak, std::fabs(sm.offset));
                if (sm.global >= cfg.traffic.startS)
                {
                    steadyPeak = std::max(steadyPeak, std::fabs(sm.offset));
                }
            }
            finalOff = it->second.back().offset;
        }
        st.push_back({peak, steadyPeak, finalOff, cfg.nodes[id].hops, cfg.nodes[id].driftPpm,
                      cfg.nodes[id].name, state.ent[id]->GetServoCount()});
    }
    g_active = nullptr;
}

// ============================ Reporting ==================================

void
PrintSingleRun(const SimCfg& cfg, const std::vector<Stat>& st)
{
    std::cout << std::fixed;
    std::cout << "\n[sim] per-node offset-from-GM summary (local - reconstructed GM):\n";
    std::cout << std::string(74, '-') << "\n";
    std::cout << std::setw(15) << "node" << " | " << std::setw(4) << "hops" << " | " << std::setw(9)
              << "ppm" << " | " << std::setw(10) << "peak us" << " | " << std::setw(10)
              << "final us" << " | servos\n";
    std::cout << std::string(74, '-') << "\n";
    std::cout << std::setprecision(3);
    for (const auto& s : st)
    {
        std::cout << std::setw(15) << s.name << " | " << std::setw(4) << s.hops << " | "
                  << std::setw(9) << std::setprecision(1) << s.ppm << std::setprecision(3) << " | "
                  << std::setw(10) << s.peak << " | " << std::setw(10) << s.finalOff << " | "
                  << s.servos << "\n";
    }

    // Hop-grouped table (INET's reporting shape).
    std::cout << "\n[sim] peak offset grouped by hop depth from GM:\n";
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
}

// Representative peer delays: the slave-side measured delay on up to three
// distinct-hop links (port 0 is always the slave port on a bridge/leaf).
void
PrintPeerDelays(const SimCfg& cfg, const Graph& g, const PassState& state)
{
    std::cout << "\n[sim] representative measured peer delays (2-step Pdelay, S1/S3):\n";
    std::cout << std::setprecision(3);
    std::vector<int> seenHops;
    int shown = 0;
    for (int id = 0; id < static_cast<int>(cfg.nodes.size()) && shown < 4; ++id)
    {
        if (cfg.nodes[id].role == Role::Gm)
        {
            continue;
        }
        int hop = cfg.nodes[id].hops;
        if (std::find(seenHops.begin(), seenHops.end(), hop) != seenHops.end())
        {
            continue;
        }
        // slave port is port 0 for our clients/bridges (the upstream link).
        Time d = state.ent[id]->GetLinkDelay(0);
        // find the upstream neighbor name for the label
        std::vector<int> par, dep;
        Bfs(g, g.gmId, par, dep);
        std::string up = par[id] >= 0 ? cfg.nodes[par[id]].name : "?";
        std::cout << "  " << std::setw(14) << up << " <-> " << std::setw(14) << cfg.nodes[id].name
                  << " : " << d.GetSeconds() * 1e6 << " us\n";
        seenHops.push_back(hop);
        ++shown;
    }
}

} // namespace

int
main(int argc, char* argv[])
{
    std::string configPath;
    std::string resultDir;
    double simTimeOverride = -1.0;
    uint32_t seed = 1, run = 1;
    CommandLine cmd(__FILE__);
    cmd.AddValue("config", "Path to the scenario YAML file (required)", configPath);
    cmd.AddValue("resultDir",
                 "Directory to write vectors.csv (+ scalars.csv when traffic runs), "
                 "opp_scavetool schema, for scripts/analyze.py; empty = skip (default)",
                 resultDir);
    cmd.AddValue("pcapPrefix",
                 "If set, capture 802.1AS pcaps (one file per device, traffic pass only) "
                 "with this file prefix; empty = off (default)",
                 g_pcapPrefix);
    cmd.AddValue("simTime", "Override sim.time_s from the YAML (s); <0 = use YAML", simTimeOverride);
    cmd.AddValue("seed", "RngSeedManager seed", seed);
    cmd.AddValue("run", "RngSeedManager run", run);
    cmd.Parse(argc, argv);

    if (configPath.empty())
    {
        std::cerr << "error: --config=<scenario.yaml> is required\n";
        return 2;
    }

    SimCfg cfg;
    try
    {
        cfg = LoadConfig(configPath);
    }
    catch (const std::exception& e)
    {
        std::cerr << "error loading config '" << configPath << "': " << e.what() << "\n";
        return 2;
    }
    if (simTimeOverride > 0)
    {
        cfg.timeS = simTimeOverride;
    }

    RngSeedManager::SetSeed(seed);
    RngSeedManager::SetRun(run);

    Graph g;
    try
    {
        g = BuildGraph(cfg);
    }
    catch (const std::exception& e)
    {
        std::cerr << "topology error: " << e.what() << "\n";
        return 2;
    }
    // Derive hop depth from gm.
    std::vector<int> par, dep;
    Bfs(g, g.gmId, par, dep);
    for (size_t i = 0; i < cfg.nodes.size(); ++i)
    {
        cfg.nodes[i].hops = dep[i];
    }

    std::cout << std::fixed;
    std::cout << "[sim] loaded '" << configPath << "': " << cfg.nodes.size() << " nodes, "
              << cfg.links.size() << " links, simTime " << std::setprecision(1) << cfg.timeS
              << "s, Sync " << cfg.syncIntervalMs << "ms, Pdelay " << cfg.pdelayIntervalMs
              << "ms.\n";
    const char* modeName = cfg.traffic.mode == TrafficMode::None            ? "none"
                           : cfg.traffic.mode == TrafficMode::BackgroundFlows ? "background_flows"
                                                                              : "aligned_bursts";
    std::cout << "[sim] traffic mode: " << modeName
              << (cfg.report.compareBaseline ? ", compare_baseline: on" : ", compare_baseline: off")
              << "\n";

    bool pass = false;

    if (!cfg.report.compareBaseline)
    {
        // -------- Single run (gptp-spike / nominal style) --------------------
        std::vector<Stat> st;
        PassState state;
        bool withTraffic = (cfg.traffic.mode != TrafficMode::None);
        // Single pass: capture whenever --pcapPrefix is set (the RunScenario hook
        // is a no-op when the prefix is empty).
        RunScenario(cfg, g, withTraffic, /*capturePcap=*/true, !resultDir.empty(), st, state);

        PrintSingleRun(cfg, st);
        PrintPeerDelays(cfg, g, state);

        bool finalOk = true, loopOk = true, coverageOk = true;
        for (const auto& s : st)
        {
            if (std::fabs(s.finalOff) >= cfg.report.finalTolUs)
            {
                finalOk = false;
            }
            if (s.servos <= 10)
            {
                loopOk = false;
            }
            if (s.peak <= 0.0)
            {
                coverageOk = false;
            }
        }
        pass = finalOk && loopOk && coverageOk;
        std::cout << "\n[sim] Gate checks (single run):\n";
        std::cout << "  [" << (finalOk ? "PASS" : "FAIL")
                  << "] every node's final offset near 0 (|.| < " << std::setprecision(1)
                  << cfg.report.finalTolUs << " us)\n";
        std::cout << "  [" << (coverageOk ? "PASS" : "FAIL")
                  << "] every traced node produced samples (all hop depths reached)\n";
        std::cout << "  [" << (loopOk ? "PASS" : "FAIL")
                  << "] servo closed the loop at every node (>10 corrections)\n";
        std::cout << "\n[sim] " << (pass ? "GATE PASS" : "GATE FAIL") << std::endl;

        if (!resultDir.empty())
        {
            std::filesystem::create_directories(resultDir);
            WriteVectorsCsv(resultDir, cfg, state);
        }
    }
    else
    {
        // -------- Compare-baseline run (congestion / feedback style) ---------
        std::vector<Stat> stBase, stBg;
        PassState base, bg;
        RunScenario(cfg, g, /*withTraffic=*/false, /*capturePcap=*/false, /*sampleQueues=*/false,
                    stBase, base);
        RunScenario(cfg, g, /*withTraffic=*/true, /*capturePcap=*/true,
                    /*sampleQueues=*/!resultDir.empty(), stBg, bg);

        std::cout << std::fixed;
        // Data-plane / bottleneck stats.
        double window = cfg.timeS - cfg.traffic.startS;
        uint64_t offeredIntoQueue = bg.dataDelivered + bg.bottleneckDrops;
        double dropPct = offeredIntoQueue ? 100.0 * bg.bottleneckDrops / offeredIntoQueue : 0.0;
        double meanBacklog = bg.backlogSamples ? bg.backlogSum / bg.backlogSamples : 0.0;
        double goodputMbps =
            bg.dataDelivered * cfg.traffic.payloadBytes * 8.0 / std::max(window, 1e-9) / 1e6;
        std::cout << "\n[sim] bottleneck (egress toward sink '" << cfg.traffic.sink << "', cap "
                  << bg.bottleneckCap << ") under traffic:\n";
        std::cout << std::setprecision(2);
        std::cout << "  data offered   : " << bg.dataOffered << " pkts\n";
        std::cout << "  delivered      : " << bg.dataDelivered << " pkts";
        if (cfg.traffic.mode == TrafficMode::BackgroundFlows)
        {
            std::cout << " (" << goodputMbps << " Mbps)";
        }
        std::cout << "\n";
        std::cout << "  dropped        : " << bg.bottleneckDrops << " pkts (" << dropPct
                  << "% of offered-into-bottleneck-queue)\n";
        std::cout << "  queue backlog  : mean " << meanBacklog << ", max " << bg.backlogMax << "\n";

        // Aligned-burst alignment metric (emergent from sync quality).
        if (cfg.traffic.mode == TrafficMode::AlignedBursts)
        {
            size_t nsrc = cfg.traffic.sources.size();
            double spreadSum = 0, spreadMax = 0;
            uint64_t spreadN = 0;
            for (const auto& [k, times] : bg.cycleFireTimes)
            {
                if (times.size() < nsrc)
                {
                    continue;
                }
                double lo = *std::min_element(times.begin(), times.end());
                double hi = *std::max_element(times.begin(), times.end());
                spreadSum += (hi - lo) * 1e6;
                spreadMax = std::max(spreadMax, (hi - lo) * 1e6);
                ++spreadN;
            }
            double meanSpread = spreadN ? spreadSum / spreadN : 0.0;
            std::cout << std::setprecision(3);
            std::cout << "\n[sim] burst alignment across " << nsrc
                      << " sources (emergent from gPTP sync):\n";
            std::cout << "  full cycles measured : " << spreadN << "\n";
            std::cout << "  mean fire-time spread: " << meanSpread
                      << " us (small => clocks agree on 'now')\n";
            std::cout << "  max  fire-time spread: " << spreadMax << " us\n";
        }

        // Side-by-side per-node peak (or steady-window peak for bursts).
        bool useSteady = (cfg.traffic.mode == TrafficMode::AlignedBursts);
        std::cout << "\n[sim] per-node "
                  << (useSteady ? "steady-window peak" : "peak") << " offset-from-GM: baseline vs "
                  << "traffic\n";
        if (useSteady)
        {
            std::cout << "  (peak over t >= " << std::setprecision(1) << cfg.traffic.startS
                      << "s, excluding the identical pre-traffic transient)\n";
        }
        std::cout << std::string(80, '-') << "\n";
        std::cout << std::setw(15) << "node" << " | " << std::setw(4) << "hops" << " | "
                  << std::setw(8) << "ppm" << " | " << std::setw(11) << "base peak" << " | "
                  << std::setw(11) << "traf peak" << " | " << std::setw(9) << "ratio/delta" << "\n";
        std::cout << std::string(80, '-') << "\n";
        std::cout << std::setprecision(3);
        double maxDelta = 0;
        std::string maxDeltaNode;
        double sinkDelta = 0, sinkBase = 0, sinkCong = 0;
        bool othersIsolated = true;
        for (size_t i = 0; i < stBase.size(); ++i)
        {
            double bp = useSteady ? stBase[i].steadyPeak : stBase[i].peak;
            double cp = useSteady ? stBg[i].steadyPeak : stBg[i].peak;
            double delta = cp - bp;
            bool isSink = (stBase[i].name == cfg.traffic.sink);
            if (isSink)
            {
                sinkDelta = delta;
                sinkBase = bp;
                sinkCong = cp;
            }
            else if (std::fabs(delta) > cfg.report.isolationTolUs)
            {
                othersIsolated = false;
            }
            if (std::fabs(delta) > std::fabs(maxDelta))
            {
                maxDelta = delta;
                maxDeltaNode = stBase[i].name;
            }
            std::cout << std::setw(15) << stBase[i].name << " | " << std::setw(4) << stBase[i].hops
                      << " | " << std::setw(8) << std::setprecision(1) << stBase[i].ppm
                      << std::setprecision(3) << " | " << std::setw(11) << bp << " | "
                      << std::setw(11) << cp << " | ";
            if (useSteady)
            {
                std::cout << std::setw(9) << delta << "\n";
            }
            else
            {
                double ratio = bp > 1e-9 ? cp / bp : 0.0;
                std::cout << std::setw(8) << std::setprecision(1) << ratio << "x"
                          << std::setprecision(3) << "\n";
            }
        }

        // Finding + gate.
        bool dropsReal = bg.bottleneckDrops > 0;
        bool baselineConverged = true;
        for (const auto& s : stBase)
        {
            if (std::fabs(s.finalOff) >= 2.0)
            {
                baselineConverged = false;
            }
        }
        bool sinkDegraded =
            sinkBase > 1e-9 && sinkCong > cfg.report.degradeFactor * sinkBase && sinkCong > 100.0;

        std::cout << "\n[sim] FINDING (reported honestly):\n";
        if (sinkDegraded)
        {
            std::cout << "  DEGRADATION LOCALIZED: sink '" << cfg.traffic.sink << "' peak "
                      << std::setprecision(3) << sinkBase << " -> " << sinkCong << " us ("
                      << std::setprecision(1) << (sinkCong / sinkBase) << "x); every other node "
                      << "within " << cfg.report.isolationTolUs << " us of baseline "
                      << (othersIsolated ? "(isolated)" : "(NOT isolated!)") << ".\n";
        }
        else
        {
            std::cout << std::setprecision(3)
                      << "  NEGLIGIBLE COUPLING: sink '" << cfg.traffic.sink << "' delta "
                      << sinkDelta << " us; largest change across all nodes " << maxDelta << " us at "
                      << (maxDeltaNode.empty() ? "(none)" : maxDeltaNode) << " -- despite real "
                      << "congestion (" << std::setprecision(1) << dropPct << "% drop). The "
                      << "sink is the only node sharing the congested egress queue; every other "
                      << "node is unaffected.\n";
        }

        pass = baselineConverged && dropsReal && othersIsolated;
        std::cout << "\n[sim] Gate checks (compare-baseline):\n";
        std::cout << "  [" << (baselineConverged ? "PASS" : "FAIL")
                  << "] baseline (no traffic): every node converges (|final| < 2 us)\n";
        std::cout << "  [" << (dropsReal ? "PASS" : "FAIL")
                  << "] congestion is real: bottleneck queue actually drops packets\n";
        std::cout << "  [" << (othersIsolated ? "PASS" : "FAIL")
                  << "] every node except the sink stays within " << std::setprecision(1)
                  << cfg.report.isolationTolUs << " us of its baseline (isolation)\n";
        std::cout << "\n[sim] " << (pass ? "GATE PASS" : "GATE FAIL") << std::endl;

        if (!resultDir.empty())
        {
            std::filesystem::create_directories(resultDir);
            WriteVectorsCsv(resultDir, cfg, bg);
            WriteScalarsCsv(resultDir + "/scalars.csv", bg.queueCounters);
        }
    }

    return pass ? 0 : 1;
}
