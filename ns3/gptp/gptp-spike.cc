// SPDX-License-Identifier: Apache-2.0
//
// syncsim ns-3 migration POC -- Phase 2 (R-GPTP) proof scenario / Gate 2.
//
// Reproduces syncsim's M1 ("Minimal") gPTP scenario on ns-3: a grandmaster, one
// time-aware bridge that is simultaneously slave (toward the GM) and master
// (toward two clients), and two end stations -- each node an independent
// syncsim::Clock (Phase 1) at M1's baseline drift rates. A minimal but real
// 802.1AS mechanism (peer-delay + Sync/Follow_Up with residence-time correction
// + a servo, all in gptp.{h,cc}) closes the loop and drives every node's
// offset-from-GM toward ~0.
//
// Topology (identical shape to simulations/minimal.ned and Phase 0's
// smoke-topology.cc -- reusing that exact CsmaHelper per-link construction):
//
//     gm(0ppm) --- sw(80ppm) --- client1(200ppm)
//                    \--------- client2(-350ppm)
//
//   sw.port0 <-> gm       : SLAVE  (faces the GM)
//   sw.port1 <-> client1  : MASTER
//   sw.port2 <-> client2  : MASTER
//
// Unlike Phase 0 there is NO BridgeNetDevice here (simplification S4 in gptp.h):
// gPTP frames are terminated per-port and the bridge *regenerates* Sync
// downstream with a fresh correction field, so a transparent L2 bridge would be
// wrong. We keep Phase 0's link construction and attach our own per-device
// receive callbacks instead.
//
// WHAT GATE 2 GATES ON (per NS3_MIGRATION_POC_PLAN.md, echoing analyze.py's
// "gate on model-correctness, never on result magnitude"): the MECHANISM --
//   (a) every node's offset converges toward ~0, and
//   (b) peak offset scales with each node's configured drift magnitude in the
//       right relative order (client2 350ppm > client1 200ppm > sw 80ppm),
//       roughly proportional to |ppm|.
// NOT on bit-matching INET's exact microsecond digits.
//
// INET M1 baseline being reproduced (from the POC plan / README): sw peak
// ~10.00us, client1 ~25.01us, client2 ~43.76us, all final ~0.00us. Those peaks
// are essentially |drift| * syncInterval (INET's default gPTP syncInterval is
// 0.125 s: 80ppm*0.125s = 10us, 200ppm*0.125s = 25us, 350ppm*0.125s = 43.75us).
// This spike uses the same 0.125 s interval, so a faithful mechanism lands on
// the same peaks -- but the gate is the scaling, not the digits.
//
// Design notes / dead ends (following the clock-spike / smoke-topology
// convention of writing down what was tried):
//   - The correction-field mechanism makes the downstream propagation
//     INDEPENDENT of how well the bridge itself is synced: sw forwards only
//     durations (its measured peer delay + its residence time) plus the GM's
//     origin timestamp, so client offset reconstruction never depends on sw's
//     absolute clock offset. This dissolves what looked at first like an
//     "unresolvable ordering problem" from sw's dual slave/master role -- sw can
//     servo its own clock at any point relative to the relay without corrupting
//     the clients. We still apply sw's servo strictly AFTER the downstream relay
//     so the phase step cannot corrupt the residence duration read off sw's
//     local clock.
//   - Servo: a phase step (deadbeat) each Sync PLUS an integral frequency
//     correction (gain 0.6) so the offset converges to ~0 rather than sitting as
//     a pure |drift|*interval sawtooth. A pure phase-only servo was tried first;
//     it reproduces the peak scaling but leaves a persistent per-cycle sawtooth,
//     so "final ~0" only held at the instants right after a correction. Adding
//     the frequency loop makes final ~0 hold structurally (the residual drift is
//     cancelled), matching INET's final ~0.00us.
//   - No RNG anywhere -> deterministic by construction (confirmed by running the
//     binary twice: byte-identical stdout). Seed/run pinned for parity.

#include "gptp.h"

#include "ns3/core-module.h"
#include "ns3/csma-module.h"
#include "ns3/network-module.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <vector>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("SyncsimGptpSpike");

namespace
{

// Per-node offset trajectory: offset-from-GM (us) recorded at every Sync the
// node's servo processes -- the exact analog of simdata.py's derived series.
struct OffsetSample
{
    double global; // seconds
    double offset; // microseconds (local - reconstructed GM)
};

std::map<int, std::vector<OffsetSample>> g_traj; // id -> samples
const char* g_names[] = {"sw   (80ppm)", "client1 (200ppm)", "client2(-350ppm)"};

void
OffsetSink(int id, Time global, double offsetSec)
{
    g_traj[id].push_back({global.GetSeconds(), offsetSec * 1e6});
}

// Receive trampoline: ns-3 ReceiveCallback signature -> GptpEntity::OnDeviceReceive.
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

} // namespace

int
main(int argc, char* argv[])
{
    double simTime = 20.0;
    double syncIntervalMs = 125.0;  // INET default gPTP syncInterval
    double pdelayIntervalMs = 50.0; // peer-delay refresh
    double finalTolUs = 2.0;        // "final offset near 0" tolerance
    CommandLine cmd(__FILE__);
    cmd.AddValue("simTime", "Simulation duration (s)", simTime);
    cmd.AddValue("syncInterval", "gPTP Sync interval (ms)", syncIntervalMs);
    cmd.AddValue("pdelayInterval", "Peer-delay interval (ms)", pdelayIntervalMs);
    cmd.AddValue("finalTol", "Final-offset tolerance (us)", finalTolUs);
    cmd.Parse(argc, argv);

    // Deterministic; this spike touches no RNG (see header). Pinned for parity.
    RngSeedManager::SetSeed(1);
    RngSeedManager::SetRun(1);

    // ---- Topology: Phase 0's exact CsmaHelper per-link construction --------
    NodeContainer nodes;
    nodes.Create(4); // 0=gm, 1=sw, 2=client1, 3=client2
    Ptr<Node> gm = nodes.Get(0);
    Ptr<Node> sw = nodes.Get(1);
    Ptr<Node> client1 = nodes.Get(2);
    Ptr<Node> client2 = nodes.Get(3);

    CsmaHelper csma;
    csma.SetChannelAttribute("DataRate", StringValue("100Mbps"));
    csma.SetChannelAttribute("Delay", TimeValue(MicroSeconds(1)));

    NetDeviceContainer gmSwLink = csma.Install(NodeContainer(gm, sw));
    NetDeviceContainer swC1Link = csma.Install(NodeContainer(sw, client1));
    NetDeviceContainer swC2Link = csma.Install(NodeContainer(sw, client2));

    // ---- Per-node local clocks at M1's baseline drift rates (Phase 1) ------
    // GM is the reference: 0 ppm, so its local time == global sim time.
    syncsim::Clock gmClock(0.0);
    syncsim::Clock swClock(80.0);
    syncsim::Clock c1Clock(200.0);
    syncsim::Clock c2Clock(-350.0);

    // ---- gPTP entities -----------------------------------------------------
    syncsim::GptpEntity gmE("gm", &gmClock, /*isGm=*/true);
    syncsim::GptpEntity swE("sw", &swClock, false);
    syncsim::GptpEntity c1E("client1", &c1Clock, false);
    syncsim::GptpEntity c2E("client2", &c2Clock, false);

    // GM: one master port toward sw.
    uint32_t gmP0 = gmE.AddPort(gmSwLink.Get(0), gmSwLink.Get(1)->GetAddress(), /*master=*/true);
    // sw: slave toward gm (port0), masters toward client1 (port1), client2 (port2).
    uint32_t swP0 = swE.AddPort(gmSwLink.Get(1), gmSwLink.Get(0)->GetAddress(), false);
    uint32_t swP1 = swE.AddPort(swC1Link.Get(0), swC1Link.Get(1)->GetAddress(), true);
    uint32_t swP2 = swE.AddPort(swC2Link.Get(0), swC2Link.Get(1)->GetAddress(), true);
    // clients: single slave port toward sw.
    uint32_t c1P0 = c1E.AddPort(swC1Link.Get(1), swC1Link.Get(0)->GetAddress(), false);
    uint32_t c2P0 = c2E.AddPort(swC2Link.Get(1), swC2Link.Get(0)->GetAddress(), false);

    // Wire device receive callbacks to the owning entity + port index.
    gmSwLink.Get(0)->SetReceiveCallback(MakeBoundCallback(&RxTrampoline, &gmE, gmP0));
    gmSwLink.Get(1)->SetReceiveCallback(MakeBoundCallback(&RxTrampoline, &swE, swP0));
    swC1Link.Get(0)->SetReceiveCallback(MakeBoundCallback(&RxTrampoline, &swE, swP1));
    swC2Link.Get(0)->SetReceiveCallback(MakeBoundCallback(&RxTrampoline, &swE, swP2));
    swC1Link.Get(1)->SetReceiveCallback(MakeBoundCallback(&RxTrampoline, &c1E, c1P0));
    swC2Link.Get(1)->SetReceiveCallback(MakeBoundCallback(&RxTrampoline, &c2E, c2P0));

    // Offset trajectories (sw=0, client1=1, client2=2).
    swE.ConnectOffsetTrace(MakeBoundCallback(&OffsetSink, 0));
    c1E.ConnectOffsetTrace(MakeBoundCallback(&OffsetSink, 1));
    c2E.ConnectOffsetTrace(MakeBoundCallback(&OffsetSink, 2));

    // ---- Drivers -----------------------------------------------------------
    Time pdelayInterval = MilliSeconds(pdelayIntervalMs);
    Time syncInterval = MilliSeconds(syncIntervalMs);
    // Peer-delay first, so link delays are measured before the first Sync.
    Simulator::Schedule(MilliSeconds(5), &syncsim::GptpEntity::StartPdelay, &swE, pdelayInterval);
    Simulator::Schedule(MilliSeconds(5), &syncsim::GptpEntity::StartPdelay, &c1E, pdelayInterval);
    Simulator::Schedule(MilliSeconds(5), &syncsim::GptpEntity::StartPdelay, &c2E, pdelayInterval);
    // GM sources Sync every syncInterval starting at t = syncInterval.
    Simulator::Schedule(syncInterval, &syncsim::GptpEntity::SendSync, &gmE, syncInterval);

    Simulator::Stop(Seconds(simTime));
    Simulator::Run();
    Simulator::Destroy();

    // ======================= Evidence reporting =============================
    std::cout << std::fixed;

    // Measured peer delays (must be small + positive -- mechanism sanity).
    Time dGmSw = swE.GetLinkDelay(swP0);
    Time dSwC1 = c1E.GetLinkDelay(c1P0);
    Time dSwC2 = c2E.GetLinkDelay(c2P0);
    std::cout << "\n[gptp-spike] measured peer delays (per-link, 1-step Pdelay, S1/S3):\n";
    std::cout << std::setprecision(3);
    std::cout << "  gm <-> sw      : " << dGmSw.GetSeconds() * 1e6 << " us\n";
    std::cout << "  sw <-> client1 : " << dSwC1.GetSeconds() * 1e6 << " us\n";
    std::cout << "  sw <-> client2 : " << dSwC2.GetSeconds() * 1e6 << " us\n";
    std::cout << "  (channel Delay=1us + one 100Mbps frame serialization; small,"
                 " positive, stable)\n";

    // Sampled offset trajectory over time (downsampled table).
    std::cout << "\n[gptp-spike] offset-from-GM trajectory (local - reconstructed GM, us)\n";
    std::cout << "  gated on: converges toward ~0, peak scales with |drift|\n";
    std::cout << std::string(72, '-') << "\n";
    std::cout << std::setw(9) << "global(s)" << " | " << std::setw(14) << g_names[0] << " | "
              << std::setw(16) << g_names[1] << " | " << std::setw(16) << g_names[2] << "\n";
    std::cout << std::string(72, '-') << "\n";
    size_t n = g_traj[0].size();
    for (size_t i = 1; i < g_traj[1].size(); ++i)
    {
        n = std::min(n, g_traj[1].size());
    }
    n = std::min({g_traj[0].size(), g_traj[1].size(), g_traj[2].size()});
    // Show the first 6 cycles (the transient/peak) then ~every 2 s.
    Time syncI = MilliSeconds(syncIntervalMs);
    double showEvery = 2.0;
    double nextShow = 0.0;
    for (size_t i = 0; i < n; ++i)
    {
        double t = g_traj[0][i].global;
        bool early = (i < 6);
        bool periodic = (t >= nextShow);
        if (!early && !periodic)
        {
            continue;
        }
        if (periodic)
        {
            nextShow = t + showEvery;
        }
        std::cout << std::setprecision(4) << std::setw(9) << t << " | " << std::setprecision(4)
                  << std::setw(14) << g_traj[0][i].offset << " | " << std::setw(16)
                  << g_traj[1][i].offset << " | " << std::setw(16) << g_traj[2][i].offset << "\n";
    }
    (void)syncI;
    (void)gmP0;
    (void)swP1;
    (void)swP2;

    // Per-node peak (max |offset|) and final offset.
    struct Stat
    {
        double peak;
        double finalOff;
        double ppmMag;
    };
    Stat st[3];
    const double ppmMag[3] = {80.0, 200.0, 350.0};
    for (int id = 0; id < 3; ++id)
    {
        double peak = 0;
        for (const auto& s : g_traj[id])
        {
            peak = std::max(peak, std::fabs(s.offset));
        }
        st[id] = {peak, g_traj[id].back().offset, ppmMag[id]};
    }

    std::cout << "\n[gptp-spike] per-node summary:\n";
    std::cout << std::setprecision(3);
    std::cout << std::setw(18) << "node" << " | " << std::setw(10) << "peak us" << " | "
              << std::setw(11) << "final us" << " | " << std::setw(14) << "peak/|ppm|"
              << " | servos\n";
    std::cout << std::string(72, '-') << "\n";
    const uint32_t servos[3] = {swE.GetServoCount(), c1E.GetServoCount(), c2E.GetServoCount()};
    for (int id = 0; id < 3; ++id)
    {
        std::cout << std::setw(18) << g_names[id] << " | " << std::setw(10) << st[id].peak << " | "
                  << std::setw(11) << st[id].finalOff << " | " << std::setw(14)
                  << (st[id].peak / st[id].ppmMag) << " | " << servos[id] << "\n";
    }
    std::cout << "  INET M1 baseline peaks: sw ~10.00, client1 ~25.01, client2 ~43.76 us"
                 " (all final ~0.00)\n";

    // ---- Gate 2 checks -----------------------------------------------------
    bool finalOk = std::fabs(st[0].finalOff) < finalTolUs &&
                   std::fabs(st[1].finalOff) < finalTolUs && std::fabs(st[2].finalOff) < finalTolUs;
    // Ordering: client2 > client1 > sw.
    bool orderOk = st[2].peak > st[1].peak && st[1].peak > st[0].peak;
    // Roughly proportional: normalized peak/|ppm| within a tight band.
    double nrm[3] = {st[0].peak / st[0].ppmMag, st[1].peak / st[1].ppmMag, st[2].peak / st[2].ppmMag};
    double nmax = std::max({nrm[0], nrm[1], nrm[2]});
    double nmin = std::min({nrm[0], nrm[1], nrm[2]});
    bool propOk = (nmax / nmin) < 1.3; // proportional to within 30%
    // Peer delays sane: positive and small (< 100 us).
    auto sane = [](Time d) { return d.GetSeconds() > 0 && d.GetSeconds() < 100e-6; };
    bool delayOk = sane(dGmSw) && sane(dSwC1) && sane(dSwC2);
    // Closed loop actually ran (many servo corrections at each node).
    bool loopOk = servos[0] > 10 && servos[1] > 10 && servos[2] > 10;

    bool pass = finalOk && orderOk && propOk && delayOk && loopOk;

    std::cout << "\n[gptp-spike] Gate 2 checks:\n";
    std::cout << "  [" << (finalOk ? "PASS" : "FAIL") << "] every node's final offset near 0 (|.| < "
              << finalTolUs << " us)\n";
    std::cout << "  [" << (orderOk ? "PASS" : "FAIL")
              << "] peak offset order client2 > client1 > sw\n";
    std::cout << "  [" << (propOk ? "PASS" : "FAIL")
              << "] peak roughly proportional to |drift| (peak/|ppm| within 30%)\n";
    std::cout << "  [" << (delayOk ? "PASS" : "FAIL")
              << "] peer delays measured, positive and small\n";
    std::cout << "  [" << (loopOk ? "PASS" : "FAIL")
              << "] servo closed the loop (>10 corrections per node)\n";
    std::cout << "\n[gptp-spike] "
              << (pass ? "GATE 2 PASS: minimal 802.1AS reproduces M1's signature on ns-3.45"
                       : "GATE 2 FAIL")
              << std::endl;

    return pass ? 0 : 1;
}
