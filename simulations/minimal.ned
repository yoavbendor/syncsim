//
// M1 minimal topology: 1 grandmaster clock + 1 TSN bridge + 2 end stations.
// Modeled on INET's OneMasterClockGptpShowcase idiom (v4.5.4).
//
// Port/gate numbering follows connection order below:
//   sw.ethg[0] <-> gm       => switch eth0 is the SLAVE port (faces the GM)
//   sw.ethg[1] <-> client1  => switch eth1 is a MASTER port
//   sw.ethg[2] <-> client2  => switch eth2 is a MASTER port
// gPTP roles are assigned in minimal.ini via gptp.masterPorts.
//
package syncsim.simulations;

import inet.networks.base.TsnNetworkBase;
import inet.node.ethernet.EthernetLink;
import inet.node.tsn.TsnClock;
import inet.node.tsn.TsnDevice;
import inet.node.tsn.TsnSwitch;

network Minimal extends TsnNetworkBase
{
    submodules:
        gm: TsnClock {
            @display("p=300,100");
        }
        sw: TsnSwitch {
            @display("p=300,250");
        }
        client1: TsnDevice {
            @display("p=200,400");
        }
        client2: TsnDevice {
            @display("p=400,400");
        }
    connections:
        gm.ethg++ <--> EthernetLink <--> sw.ethg++;      // sw eth0 (slave, faces GM)
        sw.ethg++ <--> EthernetLink <--> client1.ethg++; // sw eth1 (master)
        sw.ethg++ <--> EthernetLink <--> client2.ethg++; // sw eth2 (master)
}
