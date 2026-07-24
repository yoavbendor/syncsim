# ns-3 track — TCAM-style switch data-plane plan: ACLs, LPM routing, QoS classification

**Status: plan only. No code changed.** Companion to `NS3_PARITY_PLAN.md`. That
document closed the gap between ns-3 and OMNeT++/INET on the phenomenon
syncsim actually studies (gPTP sync ↔ congestion feedback). This document
scopes a **new capability area**, requested directly: give syncsim's switches
a real, TCAM-style match-action data plane — Access Control Lists (ACLs),
Longest Prefix Match (LPM) routing lookups for IPv4/IPv6, and QoS
classification — the way a real merchant-silicon switch ASIC's forwarding
pipeline works. This is not a parity gap; OMNeT++/INET doesn't have a
TCAM-flavored model either syncsim has ever used. It is new ground for both
tracks, being built ns-3-first per the project's now-primary-track status.

**Explicitly out of scope, per direct instruction:** BMCA (dynamic
grandmaster election — not used in automotive gPTP deployments, which this
project's use case reflects) and Qbv (time-aware shaper) / Qav
(credit-based shaper) priority *scheduling*. QoS **classification** (deciding
which traffic class a packet belongs to) is in scope; QoS **shaping/gating**
(the part Qbv/Qav do) is not — classified packets get a priority label and,
at most, a simple strict-priority multi-queue selection, not a real
IEEE 802.1Qbv gate-schedule or 802.1Qav credit engine.

---

## Why this needs a prerequisite phase most TCAM feature requests wouldn't

Read `ns3/congestion/congestion-topology.cc`, `ns3/feedback/feedback-topology.cc`,
and `ns3/sim/sim.cc` yourself and you'll find: **none of syncsim's ns-3 traffic
has an IP header today.** The background/burst data traffic used to produce
congestion is raw Ethernet — a custom ethertype (`0x88b5`) and an opaque
fixed-size payload, sent directly via `NetDevice::Send()`. There is no
`InternetStackHelper`, no `Ipv4Address` anywhere in the ns-3 track. (gPTP
traffic is *correctly* pure L2 — 802.1AS is an L2 protocol by design, per
S4's existing, deliberate choice — and stays that way; this plan does not
touch it.)

The user's own framing — "filtering... based on source/destination IPs and
ports," "LPM for IPv4 and IPv6 forwarding tables" — requires packets that
*have* IPs and ports to match on. **Phase 0 below (a real IP/UDP traffic
model for the data plane) is a genuine, sizeable prerequisite, not a
formality.** The good news: ns-3's `internet` module is already enabled in
this project's build (`Dockerfile`'s `--enable-modules=...,internet,...`), so
this needs new *code*, not a new *dependency*.

---

## Architecture: a real match-action pipeline, mirroring how switch ASICs actually work

Real TCAM-based switches evaluate packets through an ordered pipeline —
typically: **ingress ACL → L3 routing lookup (LPM) → QoS classification →
egress ACL → queue selection**. This plan builds one reusable ternary-match
primitive and three consumers of it (ACL, a longest-prefix-match structure for
routing, and QoS classification), installed per switch port, evaluated in
that pipeline order. This keeps the design honest to what "TCAM rules" means
in the real world, rather than three unrelated ad hoc filters.

### The core primitive: `TcamTable`

A generic, reusable ternary-match engine (new file, e.g. `ns3/sim/tcam.h`/`.cc`,
plus vendored copies wherever it's used):

```cpp
struct TcamRule {
    uint32_t priority;              // lower = evaluated first; first match wins
    // Each configured field is (value, mask); mask=0 means "don't care".
    Ipv4Address srcIpValue, srcIpMask;
    Ipv4Address dstIpValue, dstIpMask;
    uint16_t srcPortValue, srcPortMask;
    uint16_t dstPortValue, dstPortMask;
    uint8_t  protocolValue, protocolMask;   // TCP/UDP/etc
    // action is consumer-specific (see below) -- ACL/QoS/LPM each interpret
    // "what happens on a hit" differently, so action is a variant/enum the
    // caller supplies, not baked into TcamTable itself.
};

class TcamTable {
  public:
    void AddRule(TcamRule rule, /* action */);
    // Returns the highest-priority (lowest number) matching rule's action,
    // or a "no match" / default-action result. O(n) linear scan across
    // rules -- genuinely how a *software* TCAM emulation should behave
    // (real hardware TCAMs are O(1) via massively parallel comparators;
    // we are not modeling silicon timing, just match semantics -- see
    // "Non-goals" below).
    Result Lookup(const PacketFields& f) const;
};
```

IPv6 support is the same shape with 128-bit addresses (`Ipv6Address` is
already a real ns-3 type); ship IPv4 first, add IPv6 as an explicit,
separately-gated follow-up phase (see Phase 3) rather than building both at
once and risking neither being solid.

### LPM is NOT just "TcamTable with IP-prefix rules"

Real ASICs implement longest-prefix-match with dedicated hardware (a
prefix-sorted TCAM or a trie) precisely *because* LPM's matching rule is
different from ACL's: **LPM wants the most-specific (longest) matching
prefix to win, not the first-configured rule.** A generic priority-ordered
`TcamTable` used naively for routing would require the *operator* to keep
prefixes correctly priority-sorted by hand — fragile and unrealistic. This
plan builds a **separate, purpose-built `LpmTable` class** (a real prefix
trie, or a `std::map<Ipv4Address, ...>` sorted by descending prefix length
with a linear longest-match scan for the packet counts this project's
topologies need — no need for a highly-optimized trie at 18-node scale) —
correct LPM semantics by construction, not by rule-ordering discipline.
`TcamTable` (the ACL/QoS engine above) and `LpmTable` (routing) are two
related but distinct primitives, both "TCAM-style" in spirit, honestly
different in implementation because real hardware treats them differently
too.

### Non-goals (stated up front, not discovered halfway through)

- **Not modeling TCAM as a hardware timing/power resource.** No silicon
  area/power modeling, no realistic O(1)-vs-O(n) lookup latency simulation.
  This is a **behavioral** model (get the match-action semantics right),
  not a **hardware-performance** model. If lookup latency ever needs to be a
  first-class simulated quantity (e.g. to show TCAM overflow effects), that
  is new, separate scope, flag it explicitly if it becomes a real need.
- **Not ECMP / multi-path routing.** One next-hop per LPM prefix. Real ASICs
  often support ECMP; this project's topologies are trees today (a single
  path between any two nodes already), so ECMP has no test material yet.
- **Not dynamic routing protocols** (OSPF/BGP/etc.) populating the LPM table.
  Routes are configured (YAML) or derived from the topology at startup, the
  same way gPTP forwarding paths are today (P3b's BFS derivation) — static,
  not a routing-protocol simulation.
- **Not Qbv/Qav**, per direct instruction (see above).

---

## Phased plan

### Phase 0 — Real IP/UDP traffic model *(prerequisite; genuinely sizeable, 1–2 weeks)*

- Install `InternetStackHelper` on every node (already-enabled `internet`
  module — no new dependency). Assign IPv4 addresses per link/subnet;
  decide addressing scheme (likely `/30` per point-to-point link, matching
  how a real router/switch fabric is addressed, or a flatter scheme if
  simpler serves the test scenarios equally well).
- Convert the existing `DataSource`/burst-generator functions (currently raw
  `NetDevice::Send()` with a custom ethertype) to real `Socket`-based UDP
  flows (`UdpSocket`/`Ipv4RawSocket` or the `OnOffApplication`/
  `UdpEchoClient`-style helpers, whichever proves cleanest against this
  project's already-established "direct `NetDevice::Send()`, not
  Application-layer" finding from Phase 0 of the original migration — **that
  finding was specifically about a reproducible internal ns-3.45 assertion
  bug in `OnOffApplication`+`PacketSocketFactory`; re-verify whether a real
  `UdpSocket` (not `PacketSocketFactory`) hits the same issue before
  assuming it does or doesn't** — carrying real, distinguishable
  source/destination IPs and ports (varying per simulated "flow" so ACL/LPM
  rules have real material to differentiate on, not one flat 5-tuple).
- **gPTP traffic is explicitly NOT touched** — stays pure L2, no IP, exactly
  as 802.1AS specifies and as this project has always modeled it.
- **Switches change role**: today a `switch`-role node in `ns3/sim/`'s YAML
  engine does a single hard-coded thing — forward non-gPTP frames toward one
  fixed `sink` via a BFS-derived L2 path. Once traffic is real IP, a switch
  needs an actual per-packet forwarding *decision* (which Phase 3's LPM table
  provides) rather than one fixed destination. This is the real structural
  change Phase 0 sets up for later phases — plan it with that end state in
  mind, don't build a throwaway IP layer that Phase 3 has to rip out.
- **Gate 0:** existing M2/M3/M4-equivalent scenarios still reproduce their
  established findings (isolation shape, coupling non-finding) with the new
  IP-based traffic model, before any ACL/LPM/QoS code is added — i.e., prove
  the traffic-model swap alone doesn't change the underlying phenomenon,
  the same "verify the mechanism transfers" discipline every prior phase used.

### Phase 1 — `TcamTable` core primitive *(~3–5 days)*

- Build and unit-test the generic ternary-match engine described above, IPv4
  only initially, standalone (a small throwaway test program in the same
  spirit as the P3a spike — prove the matching semantics are right in
  isolation before wiring it into any real topology).
- **Gate 1:** a battery of match/no-match/priority-ordering test cases
  (overlapping rules, wildcard masks, first-match-wins tie-breaking) pass
  deterministically.

### Phase 2 — ACL (ingress + egress) *(~1 week)*

- Per-switch-port ingress and egress `TcamTable` instances; permit/deny
  actions; hit counters per rule (feeds observability, see Phase 5).
- YAML schema extension (`ns3/sim/`'s engine): an `acl:` block per switch or
  per link, ordered rule list.
- **Gate 2:** a new test scenario config with an explicit deny rule proves
  the denied flow's packets never arrive at the sink, while a permitted flow
  on the same link is unaffected; hit counters match the actual packet counts.

### Phase 3 — LPM routing (IPv4 first, IPv6 as an explicit follow-up) *(~1–2 weeks)*

- `LpmTable` per switch; auto-derive routes from topology at startup (like
  P3b's BFS-derived L2 forwarding today) as the default, with explicit YAML
  override for deliberately non-default routing scenarios.
- Switches now make a REAL per-packet forwarding decision via LPM lookup,
  replacing the single-fixed-sink BFS shortcut Phase 0 flagged.
- IPv6: same `LpmTable` shape, 128-bit prefixes — **separately gated**, only
  start once IPv4 is fully verified, per this plan's own "don't build two
  new things at once and risk neither being solid" principle.
- **Gate 3:** a multi-prefix scenario proves traffic to different destination
  prefixes takes the expected, distinct next-hops; the existing M2/M3/M4-style
  isolation/coupling findings still reproduce once real LPM-driven forwarding
  replaces the Phase-0-era single-sink placeholder.

### Phase 4 — QoS classification (classification + priority-queue selection only) *(~1 week)*

- Reuses `TcamTable` (5-tuple / DSCP match → traffic-class assignment).
- Classified packets select one of N egress sub-queues by priority — **a
  simple strict-priority multi-queue, not Qbv/Qav** (explicitly out of scope,
  restated here so this phase doesn't quietly grow into gate-scheduling).
- **Gate 4:** under contention, a scenario shows the high-priority class
  experiencing measurably lower drop/latency than best-effort on the same
  congested egress — the honest, minimal thing "classification matters" can
  mean without a real shaper.

### Phase 5 — Observability *(~2–3 days, folds into whichever phase is convenient)*

- Extend `scalars.csv`/CSV export with ACL hit counts, LPM route-selection
  counts, QoS class distribution — the same `opp_scavetool`-schema pattern
  every prior ns-3 CSV export has used, so `analyze.py`/`plot_results.py`
  need minimal or no changes.

### Phase 6 — Full regression *(ongoing discipline, not a separate phase)*

- Every phase above must NOT perturb the existing gPTP-sync-focused
  scenarios unless a scenario's YAML opts into ACL/LPM/QoS features — purely
  additive, same discipline as every phase in `NS3_PARITY_PLAN.md`. Full
  Gates 0-2 + M2-M5-equivalent regression (now across old binaries AND the
  `ns3/sim/` YAML configs) after each phase, same standard as S1's fix.

---

## Effort & risk summary

| Phase | Effort | Risk | Notes |
|---|---|---|---|
| 0 — real IP traffic model | 1–2 wk | Medium — re-verify the `OnOffApplication` assertion-bug finding doesn't also apply to plain `UdpSocket` before assuming either way | Genuine prerequisite, not optional |
| 1 — `TcamTable` core | 3–5 days | Low — small, standalone, unit-testable | |
| 2 — ACL | ~1 wk | Low–medium | |
| 3 — LPM (IPv4) | ~1–1.5 wk | Medium — switches' forwarding role changes structurally | |
| 3b — LPM (IPv6) | ~3–5 days | Low, once IPv4 is solid | Explicit follow-up, not bundled |
| 4 — QoS classification | ~1 wk | Low–medium — must resist scope creep into Qbv/Qav | |
| 5 — Observability | 2–3 days | Low | |

**Total, if pursued end to end: roughly 5–7 weeks.** Per this project's own
established rhythm, each phase — arguably Phase 0 on its own, given its size
and the structural switch-role change it sets up — should get its own
explicit execution approval before starting, the same way every tier and
phase in `NS3_PARITY_PLAN.md` and the original POC plan was approved
individually. **This document is the plan only; approving it does not start
Phase 0.**
