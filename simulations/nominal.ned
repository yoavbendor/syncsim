//
// M2 multi-hop topology: GM -> core switch -> {coreClient, 3 zone switches}.
// Each zone switch fans out to 4 clients. This gives two hop depths from the
// GM in one scenario so sync error growth vs hop count can be measured:
//   coreClient        : 2 hops (gm -> swCore -> coreClient)
//   clientsA/B/C[*]    : 3 hops (gm -> swCore -> swZone -> client)
//
// Port numbering (gate++ allocates in connection-statement order):
//   swCore: eth0=gm (slave, faces GM), eth1=coreClient, eth2=swA, eth3=swB, eth4=swC (master)
//   swA/B/C: eth0=swCore (slave, faces core), eth1..eth4=clients (master)
// gPTP roles assigned in nominal.ini via gptp.masterPorts.
//
// No package declaration: run.sh passes "simulations" itself as a NED root,
// so files directly under it are unpackaged (see minimal.ned).
import inet.networks.base.TsnNetworkBase;
import inet.node.ethernet.EthernetLink;
import inet.node.tsn.TsnClock;
import inet.node.tsn.TsnDevice;
import inet.node.tsn.TsnSwitch;

network Nominal extends TsnNetworkBase
{
    parameters:
        int clientsPerZone = default(4);
    submodules:
        gm: TsnClock {
            @display("p=400,80");
        }
        swCore: TsnSwitch {
            @display("p=400,220");
        }
        coreClient: TsnDevice {
            @display("p=600,220");
        }
        swA: TsnSwitch {
            @display("p=150,360");
        }
        swB: TsnSwitch {
            @display("p=400,360");
        }
        swC: TsnSwitch {
            @display("p=650,360");
        }
        clientsA[clientsPerZone]: TsnDevice {
            @display("p=150,500");
        }
        clientsB[clientsPerZone]: TsnDevice {
            @display("p=400,500");
        }
        clientsC[clientsPerZone]: TsnDevice {
            @display("p=650,500");
        }
    connections:
        gm.ethg++ <--> EthernetLink <--> swCore.ethg++;
        swCore.ethg++ <--> EthernetLink <--> coreClient.ethg++;
        swCore.ethg++ <--> EthernetLink <--> swA.ethg++;
        swCore.ethg++ <--> EthernetLink <--> swB.ethg++;
        swCore.ethg++ <--> EthernetLink <--> swC.ethg++;
        for i=0..clientsPerZone-1 {
            swA.ethg++ <--> EthernetLink <--> clientsA[i].ethg++;
        }
        for i=0..clientsPerZone-1 {
            swB.ethg++ <--> EthernetLink <--> clientsB[i].ethg++;
        }
        for i=0..clientsPerZone-1 {
            swC.ethg++ <--> EthernetLink <--> clientsC[i].ethg++;
        }
}
