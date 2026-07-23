# Phase 4 (M5 / Observability) — porting the analysis/report to ns-3

Companion to `ns3/README.md`. This documents **Gate 4**: making the real
OMNeT++-side analysis tool (`scripts/analyze.py`, via `scripts/simdata.py`) and
the sweep summarizer (`scripts/summarize_sweep.py`) run **genuinely against ns-3
output**, not a parallel reimplementation. Per the POC plan, "only the *input
parsing* changes; the offset-from-GM trick ports directly" — and it did: the
reporting, sanity-check, hop-grouping, time-windowing, and congestion-summary
logic in `analyze.py` are **untouched**.

## What changed

- **`nominal/congestion/feedback-topology.cc`** — purely additive CSV export
  behind a new `--resultDir` CommandLine arg (default empty = skip; existing
  stdout reports and the M2/M3/M4 gate checks are byte-for-byte unchanged when
  it's unset). `gptp.{h,cc}` and `clock.{h,cc}` stay byte-identical across every
  scenario dir (md5sum-confirmed) — this phase only *exports* data the scenarios
  already compute.
- **`scripts/simdata.py`** — one surgical, backward-compatible early-return in
  `export_vectors_to_csv` / `export_scalars_to_csv`.
- **`scripts/summarize_sweep.py`** — the symmetric touch, plus a `*-cap=*.csv`
  discovery fallback.
- **`ns3/scripts/run_sweep.sh`** (new) — the ns-3 analog of
  `scripts/run_sweep.sh` + `simulations/sweep.ini`'s `${cap = 5, 20, 80}`.

## CSV schema / module-naming convention

The drivers write exactly the long-form CSV `opp_scavetool` produces, so
`analyze.py`'s existing filters match unmodified.

**`vectors.csv`** — columns `module,name,vectime,vecvalue`, one row per traced
clock:

| field | value |
|---|---|
| `module` | `Nominal.<node>.clock` (e.g. `Nominal.swCore.clock`, `Nominal.clientsA[0].clock`) |
| `name` | `timeChanged:vector` |
| `vectime` | space-separated global sim-time (s) of each sample |
| `vecvalue` | space-separated **local clock time** (s) of each sample = `global + offset` |

- The module form matches `simdata.HOP_MAPS["Nominal"]`'s regexes verbatim, so
  hop grouping works with **zero** `simdata.py` changes.
- The name matches `analyze.py`'s filter
  `df[(df["name"] == "timeChanged:vector") & df["module"].str.endswith(".clock")]`.
- `simdata.parse_offset_series` recovers `offset = vecvalue − vectime`. Because
  the GM is drift-free, a node's local clock time minus the global sim time at
  that sample **is** its offset-from-GM — the exact trick the INET path uses.

**`vectors.csv` also carries `queueLength:vector` rows (P1b), one per traced
switch-egress queue:**

| field | value |
|---|---|
| `module` | `Nominal.<node>.eth<port>.macLayer.queue` (e.g. `Nominal.swCore.eth1.macLayer.queue`, the bottleneck) |
| `name` | `queueLength:vector` |
| `vectime` | space-separated global sim-time (s) of each backlog sample (5 ms cadence) |
| `vecvalue` | space-separated queue backlog (packets) at each sample |

- The module form is the bracket-free ns-3 shape `plot_results.py`'s
  `BOTTLENECK`/`_resolve_bottleneck` already match, so its backlog and
  offset-vs-backlog-coupling plots render with **zero** `plot_results.py` changes.
- Sampling is read-only (`Queue::GetNPackets`) and scheduled only when
  `--resultDir` is set, so it is purely additive — stdout and every gate are
  byte-identical when unset.

**`scalars.csv`** (congestion/feedback only) — columns `module,name,value`, six
rows per switch-egress queue, module `Nominal.<node>.eth<port>.macLayer.queue`
(port index = the gPTP port index, which matches `nominal.ned`'s port order:
`swCore.eth1` faces `coreClient`, the bottleneck). Stat names match INET's
`PacketQueue` exactly so `analyze.py`'s congestion summary
(regex `\.macLayer\.queue$`) reads them:

- `incomingPackets:count`, `outgoingPackets:count`,
  `droppedPacketsQueueOverflow:count`
- `incomingPacketLengths:sum`, `outgoingPacketLengths:sum`,
  `droppedPacketLengths:sum` (lengths in **bits**, INET's convention)

Counters come from the ns-3 `Queue` `Enqueue`/`Dequeue`/`Drop` traces:
`incoming = enqueued + dropped` (everything that arrived), `outgoing = dequeued`,
`dropped = overflow drops`. The congested/bursts pass is exported.

## Why the `simdata.py` / `summarize_sweep.py` patches are behavior-preserving for the OMNeT++ path

Both patches are a pure early-return: "if `vectors.csv` / `scalars.csv` already
exists in the result dir, use it and skip `opp_scavetool`." The guarantee is
**structural, by inspection** (not something we could run and watch, because
this sandbox has **no OMNeT++/INET build** — only ns-3):

> `opp_scavetool` is what *produces* `vectors.csv` / `scalars.csv` in the first
> place. A genuine OMNeT++ result directory therefore **never** contains one
> *before* `analyze.py` runs — it holds only `.vec` / `.sca` files. The new
> branch is thus never taken on any real OMNeT++ run, and the `opp_scavetool`
> call is reached exactly as before.

This reconciles with the POC plan's non-destructive constraint (whose intent is
protecting the OMNeT++ path's *behavior*, which is preserved) and with Phase 4's
own text, which explicitly calls for this change. Honest caveat: the ns-3 path
below **was** run and verified; the OMNeT++ path's invariance is reasoned, not
re-executed.

---

## Gate 4 proof — real `analyze.py --strict` output against ns-3 data

Command (run against the CSVs the ns-3 binaries just wrote):

```
python3 scripts/analyze.py <ns3-result-dir> --strict --sim-time 30 --time-windows 4
```

### nominal (M2 topology, no traffic) — exit 0, PASS

The per-node offset table, hop grouping, and time windows below are the
`analyze.py` output (P1a-hardened servo); the hop peaks (hops=1 7.98; hops=2 mean
15.69 / max 24.08; hops=3 mean 13.01 / max 28.09 µs) are **identical** to the C++
driver's own in-binary report — cross-tool agreement, off the exported CSV.

```
[simdata] using pre-existing .../res-nominal/vectors.csv (no opp_scavetool)
[analyze] available vector names: ['queueLength:vector', 'timeChanged:vector']
[analyze] gPTP offset-from-master per node:
  Nominal.clientsA[0].clock      final=    +0.00us  peak=    20.13us  n=  239  hops=3
  ... (all 17 non-GM nodes) ...
  Nominal.coreClient.clock       final=    +0.00us  peak=    24.08us  n=  239  hops=2
  Nominal.swCore.clock           final=    +0.00us  peak=     7.98us  n=  239  hops=1
[analyze] peak offset by hop count from GM:
  hops=1  n_nodes=  1  mean_peak=    7.98us  max_peak=    7.98us
  hops=2  n_nodes=  4  mean_peak=   15.69us  max_peak=   24.08us
  hops=3  n_nodes= 12  mean_peak=   13.01us  max_peak=   28.09us
[analyze] peak |offset| by time window (4 x 7.5s, in us):
  ... per-node w0..w3 (all convergence in w0, then 0.00) ...
[analyze] egress queue backlog (packets):
  ... all switch queues max=0 mean=0.00 (nominal has no background traffic) ...
[analyze] sanity check: PASS -- gPTP produced the expected, finite, bounded signals.
```

### congestion (M3) — exit 0, PASS

`coreClient` degrades (peak **510.47 µs** after P1a's servo/peer-delay
hardening — was 46,281 µs), while all 16 other nodes hold their baseline — the
isolation signature, visible in the offset table, the new backlog vector, and the
queue summary. The bottleneck `Nominal.swCore.eth1.macLayer.queue` shows 139.86
Mbps offered, 326,020 ppm (32.6 %) drop, and a `queueLength:vector` backlog that
sits at max 10 / mean 8.50 packets (P1b); every other queue carries only gPTP,
drops nothing, and stays near-empty.

```
  Nominal.coreClient.clock       final=   -85.11us  peak=   510.47us  n=  170  hops=2
  (all other nodes: peak 1.72 .. 28.09us, unchanged from nominal baseline)
[analyze] peak |offset| by time window (4 x 7.5s, in us):
  Nominal.coreClient.clock          510.47   327.57   377.58   216.50
  (every other node: convergence in w0, then 0.00)
[analyze] sanity check: PASS -- gPTP produced the expected, finite, bounded signals.
[analyze] egress queue backlog (packets):
  Nominal.swCore.eth1.macLayer.queue  max=   10  mean=  8.50
[analyze] data-plane congestion summary (offered load, 30s window):
  Nominal.swCore.eth1.macLayer.queue    139.86 Mbps    18161.9 pps  drop=  326020.1 ppm
  Nominal.swCore.eth0.macLayer.queue      0.01 Mbps       20.0 pps  drop=       0.0 ppm
  (... all other egress queues: ~0.01 Mbps, 0.0 ppm drop ...)
```

### feedback (M4) — exit 0, PASS

Offsets sit at baseline except `coreClient`, whose steady-window peak shows the
sub-µs, single-node coupling P1a surfaced (`0.694 µs` delta; all 16 others exactly
0 — see `feedback/README.md`); the bottleneck sees 87.0 % drop (870,441 ppm) from
the aligned microbursts (matching the C++ report's ~88 %) and a `queueLength:vector`
backlog spiking to its 20-packet cap (mean 0.97, the bursty regime).

```
  Nominal.coreClient.clock       final=    +0.00us  peak=    24.08us  n=  181  hops=2
  (all other nodes: peak == nominal baseline)
[analyze] sanity check: PASS -- gPTP produced the expected, finite, bounded signals.
[analyze] egress queue backlog (packets):
  Nominal.swCore.eth1.macLayer.queue  max=   20  mean=  0.97
  Nominal.swCore.eth1.macLayer.queue     18.84 Mbps     1774.0 pps  drop=  870441.0 ppm
```

**Time-windowed reporting** (`--time-windows`) came for free — no new code; it
operates on the same exported vectors (shown inline above).

---

## Parameter sweep — real drop-rate-vs-capacity table

```
NS3_ROOT=<ns-3-dev> ns3/scripts/run_sweep.sh congestion-topology <out> 5 20 80
python3 scripts/summarize_sweep.py <out> --var cap \
    --bottleneck-module Nominal.swCore.eth1.macLayer.queue
```

```
[sweep] bottleneck (Nominal.swCore.eth1.macLayer.queue) vs cap:
       cap    offered pk    dropped pk    drop ppm
         5        544857        178915    328370.6
        20        544857        177618    325990.1
        80        544856        177579    325919.1
```

**Honest finding:** drop rate is ~32.6 % and nearly **flat** across queue
capacity. This is the physically correct result for a *persistently* ~150-into-100
Mbps oversubscribed egress — steady-state drop rate is set by
`(offered − service) / offered`, which a bigger buffer cannot change; buffer
depth affects latency/jitter and burst absorption, not sustained-overload drop
rate (cap=5 is slightly higher from less burst absorption). `simulations/sweep.ini`
drives the same sustained `exponential(160us)` source, so it exhibits the same
weak dependence. The M5 point — the sweep *mechanism* (one lever, N runs, one
comparison table) — works.

---

## Mermaid / Pages pipeline — stretch goal: verified working (the one gap is now closed by P1b)

Not required for Gate 4 (the plan's Gate 4 is "a `--strict`-equivalent sanity
gate green," not "appears on the Pages site"), but attempted and it works:
`scripts/plot_results.py` and `scripts/build_site.py` route their input parsing
through the same `simdata.py` functions patched above, so they consume the ns-3
CSVs with **zero further code changes**. Verified in the sandbox:

```
python3 scripts/plot_results.py --result-dir res-nominal --slug ns3-nominal \
    --title "ns-3 M2 (Nominal)" --network Nominal --ini simulations/nominal.ini --site ns3-site
python3 scripts/plot_results.py --result-dir res-congestion --slug ns3-congestion ... 
python3 scripts/plot_results.py --result-dir sweep-out --slug ns3-sweep --kind sweep ...
python3 scripts/build_site.py --site ns3-site      # -> index.html from 3 fragment(s)
```

This produced real per-scenario fragments + PNGs (sync-error-over-time,
sync-error-vs-hop-count, the sweep drop-rate bar) and assembled a full
`index.html` — the mermaid diagram (sourced from the shared
`simulations/<scenario>.ini`, which the ns-3 scenarios reproduce), narrative,
and levers all render. Using the OMNeT++ `.ini` for the diagram is faithful
because the ns-3 nominal/congestion topologies *are* reproductions of those
scenarios.

**Known gap — CLOSED (P1b).** The *backlog* and *offset-vs-backlog coupling*
plots need a `queueLength:vector` time-series in `vectors.csv`. As of P1b the
drivers export exactly that: `nominal`/`congestion`/`feedback-topology.cc` sample
every traced switch-egress queue's backlog periodically (5 ms cadence, over the
whole run) and write one `queueLength:vector` row per queue —
`module = "Nominal.<node>.eth<port>.macLayer.queue"` (the bracket-free ns-3 form
`plot_results.py`'s `BOTTLENECK`/`_resolve_bottleneck` already match),
`vectime` = global sample time (s), `vecvalue` = backlog (packets). The
sampling is read-only and scheduled only when `--resultDir` is set, so the
existing stdout/gate behavior is byte-identical when it is unset (verified).
Verified end-to-end: `plot_results.py` against fresh `congestion`/`feedback`
output now renders **non-empty** backlog and coupling PNGs — the congestion
bottleneck series reaches its 10-packet cap (mean ≈ 8.5), the feedback series
spikes to its 20-packet cap under the aligned microbursts, and the coupling plot
overlays `coreClient`'s offset on that backlog. All other plots, the mermaid, the
narrative, the sweep bar, and the full-site assembly work today. This slug is
**additive** — a separate `ns3-*` set, alongside the existing M1–M5 pages, not
replacing anything. The generated site is not committed (it is build output, like
the OMNeT++ site).
