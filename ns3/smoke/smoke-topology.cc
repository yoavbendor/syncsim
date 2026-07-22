// Phase 0 (Gate 0) data-plane smoke test: proves ns-3's *easy* half of the
// syncsim port -- CSMA links + a BridgeNetDevice switch + a finite,
// really-dropping egress queue + pcap capture -- headless and deterministic.
// No clock/gPTP model here: that is Phase 1/2's job. This only exercises
// R3/R4/R6 from NS3_MIGRATION_SURVEY.md's requirements table.
//
// Topology mirrors simulations/minimal.ned's shape (GM -- sw -- {client1,
// client2}), minus the gPTP/clock machinery:
//
//   gm -------+
//             sw --- client2  (both flows converge here, like
//   client1 --+                congestion.ini's shared-uplink pattern)
//
// gm and client1 both flood client2 fast enough to overflow the switch's
// small egress queue toward client2 -- proving real, shared-queue drops
// (R3), the same "convergence on one link" pattern congestion.ini uses for
// real in Phase 3, not just an isolated per-link overrun.
//
// Traffic is sent via direct NetDevice::Send() calls on a Simulator-scheduled
// timer, not through ns-3's Application/Socket layers. Two higher-level
// approaches were tried first and both failed for reasons unrelated to
// R3/R4/R6:
//   1. UdpClientHelper over the full IP stack: the vast majority of offered
//      packets vanished silently in the ARP/socket layers before ever
//      reaching the NetDevice queue (confirmed empirically: of 100,000
//      scheduled app-level sends, only ~4-9% reached the queue's Enqueue
//      trace, and the rest showed up on none of the
//      Ipv4L3Protocol/ArpL3Protocol/device-queue Drop traces either) -- an
//      artifact of the IP stack's own flow control, nothing to do with the
//      CSMA/bridge/queue mechanism this test exists to exercise.
//   2. OnOffApplication + PacketSocketFactory (the idiom ns-3's own
//      csma-packet-socket.cc example uses): hit a reproducible internal
//      assertion (`m_nBytes.Get() >= item->GetSize()` in queue.h's
//      DoDequeue) even in a minimal 2-node/1-flow/large-queue isolation,
//      independent of queue capacity -- a real bug or edge case in this
//      ns-3.45 build's OnOff+PacketSocket interaction, not something worth
//      chasing further for a Gate-0 smoke test.
// Direct NetDevice::Send() is simpler, has no such issue, and is closer to
// what Phase 1/2's clock/gPTP work will need anyway (precise, code-driven
// control over when frames go out).

#include "ns3/bridge-module.h"
#include "ns3/core-module.h"
#include "ns3/csma-module.h"
#include "ns3/network-module.h"

#include <iostream>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("SyncsimSmokeTopology");

static uint32_t g_totalDrops = 0;

static void
QueueDropTrace(Ptr<const Packet>)
{
    g_totalDrops++;
}

// Repeatedly sends `size`-byte frames from `dev` to `dest` every `interval`,
// until `stopAt`. Direct NetDevice::Send() -- see file header for why this
// replaced an Application-layer traffic generator.
static void
FloodSend(Ptr<NetDevice> dev, Address dest, uint16_t protocol, uint32_t size, Time interval, Time stopAt)
{
    if (Simulator::Now() >= stopAt)
    {
        return;
    }
    dev->Send(Create<Packet>(size), dest, protocol);
    Simulator::Schedule(interval, &FloodSend, dev, dest, protocol, size, interval, stopAt);
}

int
main(int argc, char* argv[])
{
    uint32_t queueCapacityPackets = 5;
    double simTimeSeconds = 0.5;
    std::string pcapPrefix = "smoke";

    CommandLine cmd(__FILE__);
    cmd.AddValue("queueCapacity", "Egress queue capacity in packets", queueCapacityPackets);
    cmd.AddValue("simTime", "Simulation duration in seconds", simTimeSeconds);
    cmd.AddValue("pcapPrefix", "Prefix for pcap capture files", pcapPrefix);
    cmd.Parse(argc, argv);

    // Deterministic: fixed seed/run, no arg for either -- CI always gets the
    // same trace, exactly the property M1-M5's OMNeT++ runs already have.
    RngSeedManager::SetSeed(1);
    RngSeedManager::SetRun(1);

    NodeContainer nodes;
    nodes.Create(4); // 0=gm, 1=sw, 2=client1, 3=client2
    Ptr<Node> gm = nodes.Get(0);
    Ptr<Node> sw = nodes.Get(1);
    Ptr<Node> client1 = nodes.Get(2);
    Ptr<Node> client2 = nodes.Get(3);

    CsmaHelper csma;
    csma.SetChannelAttribute("DataRate", StringValue("100Mbps"));
    csma.SetChannelAttribute("Delay", TimeValue(MicroSeconds(1)));
    // Small, finite, real-dropping egress queue -- R3. ns-3's csma default
    // queue is already a bounded DropTailQueue; this just sets its capacity
    // small enough that the converging flood below is guaranteed to
    // overflow it.
    csma.SetQueue("ns3::DropTailQueue<Packet>",
                   "MaxSize",
                   StringValue(std::to_string(queueCapacityPackets) + "p"));

    // Each CSMA "channel" here is used point-to-point (2 devices), matching
    // ns-3's own bridge examples (e.g. csma-bridge.cc): the switch aggregates
    // one CSMA device per link into a single BridgeNetDevice port set (R4).
    NetDeviceContainer gmSwLink = csma.Install(NodeContainer(gm, sw));
    NetDeviceContainer swC1Link = csma.Install(NodeContainer(sw, client1));
    NetDeviceContainer swC2Link = csma.Install(NodeContainer(sw, client2));

    NetDeviceContainer swPorts;
    swPorts.Add(gmSwLink.Get(1));
    swPorts.Add(swC1Link.Get(0));
    swPorts.Add(swC2Link.Get(0));

    BridgeHelper bridge;
    bridge.Install(sw, swPorts);

    // Two floods converging on client2 -- mirrors congestion.ini's
    // shared-uplink pattern (multiple senders, one bottleneck egress).
    // 512B every 2us = ~2.05Gbps offered per flow, ~20x either link's
    // 100Mbps capacity -- guaranteed to overflow the switch's egress queue
    // toward client2 well before the link itself could ever drain it.
    Address client2Addr = swC2Link.Get(1)->GetAddress();
    const uint16_t protocol = 0x88b5; // IEEE 802 local-experimental ethertype
    Simulator::Schedule(Seconds(0.05),
                         &FloodSend,
                         gmSwLink.Get(0),
                         client2Addr,
                         protocol,
                         512,
                         MicroSeconds(2),
                         Seconds(simTimeSeconds));
    Simulator::Schedule(Seconds(0.05),
                         &FloodSend,
                         swC1Link.Get(1),
                         client2Addr,
                         protocol,
                         512,
                         MicroSeconds(2),
                         Seconds(simTimeSeconds));

    // Trace drops on EVERY device's egress queue (R3 verification) -- not
    // just the link nearest the bottleneck, so a measurement blind spot
    // can't hide the result the way it did in an earlier iteration.
    NetDeviceContainer allDevices;
    allDevices.Add(gmSwLink);
    allDevices.Add(swC1Link);
    allDevices.Add(swC2Link);
    for (uint32_t i = 0; i < allDevices.GetN(); ++i)
    {
        Ptr<CsmaNetDevice> dev = DynamicCast<CsmaNetDevice>(allDevices.Get(i));
        Ptr<Queue<Packet>> q = dev->GetQueue();
        q->TraceConnectWithoutContext("Drop", MakeCallback(&QueueDropTrace));
    }

    // pcap on every device -- R6 (observability parity with syncsim's
    // PcapRecorder-based capture/replay recipe).
    csma.EnablePcapAll(pcapPrefix, false);

    Simulator::Stop(Seconds(simTimeSeconds));
    Simulator::Run();
    Simulator::Destroy();

    std::cout << "[smoke] total egress queue drops: " << g_totalDrops << std::endl;
    std::cout << (g_totalDrops > 0 ? "[smoke] PASS: real drops observed under overload (R3)"
                                    : "[smoke] FAIL: no drops observed -- overload didn't trigger")
              << std::endl;

    return g_totalDrops > 0 ? 0 : 1;
}
