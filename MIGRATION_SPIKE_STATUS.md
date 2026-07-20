# OMNeT++ 6.4 / INET 4.7 migration spike — status & handoff

Tracks the "isolated migration spike to OMNeT++ 6.4 + INET 4.7" mentioned in
README's "Why pinned to OMNeT++ 6.0.3 + INET 4.5.4" section. Goal: replace the
current pin (for 6.4's NED-editing/diagramming IDE tooling) without regressing
the M1-M5 results that pin currently guarantees. Branch: `claude/omnetpp-install-6topxg`.

**Nothing in the repo has changed.** Everything below happened in the sandbox
container (`/opt`, `/tmp`) and disappears when it's reclaimed. This file exists
to make the work reproducible, not to record a completed migration.

## Environment built so far (sandbox-local, not in Dockerfile)

- **OMNeT++ 6.4.0**, built from source at `/opt/omnetpp-6.4.0`: `git clone
  --branch omnetpp-6.4.0 https://github.com/omnetpp/omnetpp.git`, then
  `cp configure.user.dist configure.user && source setenv && ./configure
  WITH_QTENV=no WITH_OSG=no WITH_OSGEARTH=no && make MODE=release base` —
  same headless recipe the current Dockerfile uses for 6.0.3, still applies
  unchanged. Verified via `opp_run -v` and by building+running the bundled
  `fifo` sample to completion (721,549 events).
- **INET 4.7.0** (tag `v4.7.0`), built from source at `/opt/inet-4.7` against
  the above: `git clone --branch v4.7.0 https://github.com/inet-framework/inet.git`,
  then `source <omnetpp>/setenv && source setenv && make makefiles && make
  MODE=release`. Clean build, `libINET.so` produced.
- **Gotcha for whoever continues this:** this sandbox's proxy blocks
  `github.com` web/release-download endpoints and `add_repo` for orgs outside
  the session's owner scope (403 "GitHub access ... not enabled for this
  session") — that killed the prebuilt-tarball route for both OMNeT++ and
  INET. Plain `git clone https://github.com/...` over HTTPS worked fine for
  both despite that, and is what actually got this done. If the next session
  hits the same 403 on a release download, don't fight it — just `git clone`
  the tag instead.

## What's verified: M1 (`minimal.ini`) runs on INET 4.7

INET 4.7 rewrote `Gptp` (formal state machines, dedicated clock-servo
submodule, different message-timestamping points — this is the exact gap
README's decision record already flagged). Running `minimal.ini` against it
needed **3 ini-only changes, zero NED changes**:

1. `simtime-resolution = fs` — new peer-delay measurement needs femtosecond
   resolution; previously unset (defaulted to ps).
2. `**.transmitter.typename = "StreamingTransmitter"` and
   `**.receiver.typename = "DestreamingReceiver"` — the new `Gptp` timestamps
   off streaming-mode PHY signals; `LayeredEthernetInterface` (INET 4.7's
   default Ethernet interface, used by `TsnClock`/`TsnSwitch`/`TsnDevice`)
   doesn't default to these.
3. `**.clock.oscillator.nominalTickLength = 10ns` — `ConstantDriftOscillator`'s
   base class (`DriftingOscillatorBase`) made this parameter mandatory, no
   default anymore. `10ns` is the value INET's own examples/showcases use.

Everything else — the `Minimal` NED topology, `gptp.masterPorts`,
`hasTimeSynchronization`, per-node `driftRate`, `referenceClock` — carried
over unchanged from the current repo's `minimal.ini`.

**Result — matches the documented M1 baseline almost exactly:**

| node (drift) | INET 4.5.4 (README baseline) | INET 4.7 (this spike) |
|---|---|---|
| sw (80ppm) | final=0.00us peak=10.00us | final=0.00us peak=**10.00us** |
| client1 (200ppm) | final=0.00us peak=25.01us | final=0.00us peak=**25.00us** |
| client2 (-350ppm) | final=0.00us peak=43.76us | final=0.00us peak=**43.76us** |

(Computed manually from raw `clock.timeChanged` vectors via `opp_scavetool`
+ pandas — `scripts/analyze.py` could NOT be used as-is; see below. Scratch
ini lived at `/tmp/inet-build/minimal-4.7.ini`, not committed — reconstruct
from `simulations/minimal.ini` + the 3 changes above.)

## What's broken and needs fixing before this can gate anything

- **`scripts/analyze.py` / `scripts/simdata.py`**: hardcode the signal name
  `gptp.timeDifference`, which doesn't exist in INET 4.7. The equivalent
  signal is `clock.timeChanged`, recorded on the node's `clock` submodule
  directly rather than on the `gptp` submodule. `--strict` sanity gating is
  not usable against 4.7 output until this is updated.

## Not attempted yet (remaining scope of the spike)

- **M2** (`nominal.ini`, 17-node multi-hop) — unverified. Highest-risk step:
  bridges use `GptpBridge`/the multi-domain machinery, INET 4.7's most
  structurally different piece vs. the old monolithic `Gptp`, so this is
  where something version-specific is most likely to surface.
- **M3** (`congestion.ini`, `DropTailQueue` + background UDP) — unverified.
  Queueing/traffic modules are gPTP-independent so likely fine, but not run.
- **M4** (`feedback.ini`, clock-driven bursts) — depends on M2+M3, not run.
- **M5** (`sweep.ini`, time-windowed report + parameter sweep) — not run.
- **`scripts/gen_topology.py`** (YAML → NED/ini) — not checked against 4.7's
  NED changes (e.g. `TsnClock` now `extends GptpMaster` directly instead of
  toggling a generic `gptp` submodule via `hasTimeSynchronization`); unknown
  whether the generator's output still matches hand-written NED under 4.7.
- **Dockerfile** — still pins 6.0.3/4.5.4, completely untouched this session.
- **CI** — not exercised against 4.7 at all.

## Suggested transition plan

1. Fix `analyze.py`/`simdata.py`'s signal name (`gptp.timeDifference` →
   `clock.timeChanged`, module path `**.gptp` → `**.clock`) as a standalone
   change, since every later step needs `--strict` working to mean anything.
2. Bump the Dockerfile to 6.4.0 + INET 4.7.0 using the build recipe above —
   it's a near-verbatim port of the current 6.0.3 recipe (`WITH_QTENV=no`
   etc. all still apply), on its own branch/PR so it can be reverted cleanly
   if a later milestone doesn't pan out.
3. Port the 3 `minimal.ini` changes above into the repo's actual
   `simulations/minimal.ini`, and check whether `nominal.ini`/`congestion.ini`/
   `feedback.ini`/`sweep.ini` need the same 3 (they build on the same
   `TsnClock`/`TsnSwitch`/`TsnDevice` primitives, so probably yes).
4. Rerun M1 in real CI, confirm the table above holds outside this sandbox.
5. Work M2 → M3 → M4 → M5 in that order (same dependency order as the
   original build). Treat M2 as the real test of whether this migration is
   viable at all, per the risk note above.
6. Only once all five are green under 4.7: retire the old pin, update
   README's "Why pinned to..." decision record to close out this note, and
   delete this file.
7. If any milestone's numbers diverge non-trivially from its documented
   baseline, that's a legitimate finding to document (per this project's own
   "honest finding" standard) — not automatically a blocker, but not
   something to silently paper over either.

## Reproduction budget

For whoever picks this up in a fresh sandbox: OMNeT++ clone (~1 min) + build
(~5-10 min on 4 cores) + INET clone (~1 min) + build (~15-25 min, INET is
much larger than OMNeT++ itself) before any scenario work can start. Plan
for roughly 25-40 min of unattended build time up front.
