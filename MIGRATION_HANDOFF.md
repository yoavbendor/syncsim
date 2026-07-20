# OMNeT++ 6.4 / INET 4.7 migration — handoff to sandbox-driven iteration

You have a local sandbox with OMNeT++ 6.4.0 + INET 4.7.0 already built. This
doc hands you the lead on finishing the migration spike referenced in
README's "Why pinned to OMNeT++ 6.0.3 + INET 4.5.4" section. Read this fully
before touching anything — the git branch already has real progress, more
than a from-scratch spike would assume.

## Branch: use `claude/inet-4.7-omnetpp-6.4-migration`, not a new one

That's the branch of record for this work (tracked in the original plan as
"C2"). If you or a prior session started `claude/omnetpp-install-6topxg` or
anything else, treat its *findings* as validation (see below — they converged
almost exactly with this branch's independent fixes) but do your git work on
`claude/inet-4.7-omnetpp-6.4-migration`. Fetch and check it out before
starting:

```
git fetch origin claude/inet-4.7-omnetpp-6.4-migration
git checkout claude/inet-4.7-omnetpp-6.4-migration
git pull
```

**Check CI before you do anything else.** As of this handoff, commit
`68de868` just pushed and its CI run (workflow `sim-ci`, triggered by that
push) was still `in_progress`. Check its outcome first — it may already tell
you whether M2-M5 pass or where the next break is, saving you a redundant
local repro:

```
# via gh/GitHub MCP tools, or just open in a browser:
https://github.com/yoavbendor/syncsim/actions?query=branch%3Aclaude%2Finet-4.7-omnetpp-6.4-migration
```

## What's already done on this branch (committed, pushed, CI-verified)

The `Dockerfile` on this branch already pins OMNeT++ 6.4.0 + INET 4.7.0 and
**builds successfully in real CI** (confirmed: ~25min cold build, then cached
via buildx gha cache on later runs, ~1min). You do not need to rebuild the
Docker recipe — your sandbox's manually-built `/opt/omnetpp-6.4.0` +
`/opt/inet-4.7` should already match it (same versions, same
`WITH_QTENV=no WITH_OSG=no WITH_OSGEARTH=no` headless recipe, same INET
`make makefiles && make MODE=release`).

Five real, CI-confirmed INET 4.6+ breaking changes have been found and fixed,
all as ini-only or Python-script changes — **zero NED changes needed so far**:

1. **`ConstantDriftOscillator.oscillator.nominalTickLength`** is now a
   mandatory parameter (its base class, `DriftingOscillatorBase`, dropped the
   default). Fixed: `**.clock.oscillator.nominalTickLength = 10ns` added to
   all five scenario ini files, matching INET's own
   `showcases/tsn/timesynchronization/gptp/omnetpp.ini` reference config,
   which pairs it with `simtime-resolution = fs`.
2. **`simtime-resolution = fs`** added alongside it (same reference config;
   INET 4.6+'s peer-delay measurement needs femtosecond precision).
3. **Gptp's reimplementation requires streaming-mode PHY signals.** It now
   times frames off `receptionStarted`/`transmissionStarted` signals, which
   only `StreamingTransmitter`/`DestreamingReceiver` emit (INET 4.7's default
   `PacketTransmitter`/`PacketReceiver` don't). Symptom was: `Invalid process
   state, the network interface module must emit receptionStarted signal`.
   Fixed: `**.transmitter.typename = "StreamingTransmitter"` and
   `**.receiver.typename = "DestreamingReceiver"` added to all five ini
   files (again matching INET's own reference config verbatim).
4. **Ipv4 now uses `checksumMode`, not `crcMode`** (the old ini's
   `**.crcMode = "computed"` silently never applied to Ipv4 at all). Fixed in
   `pcap_capture.ini`: added `**.checksumMode = "computed"`.
5. **`Gptp` no longer emits a `timeDifference` signal at all** (confirmed
   against INET 4.7's `Gptp.ned`: only `gmRateRatioChanged`,
   `neighborRateRatioChanged`, `pdelayChanged`, `packetDropped` remain).
   Offset-from-GM has to be derived from `ClockBase`'s `timeChanged` signal
   instead (each clock's own absolute time, not an offset) — valid because
   every scenario's GM has `driftRate=0ppm`, so GM's clock time is always
   exactly simulation time, and `offset_from_GM = clock_time - event_time`
   for any other node. Fixed in `scripts/simdata.py` (new
   `parse_offset_series` helper + `HOP_MAPS` patterns changed from `.gptp` to
   `.clock` module suffix), `scripts/analyze.py`, and `scripts/plot_results.py`.

Also fixed, not INET-version-related but discovered along the way:
`PcapRecorder.packetFilter` is an `object`-typed parameter, and its `expr(...)`
value must be passed **unquoted** in ini (a quoted `"expr(...)"` string
parses as a literal cMatchExpression pattern instead) — fixed in
`pcap_capture.ini`'s `[Config SyncOnly]`.

**Confirmed green in real CI so far:** M1 (`minimal.ini`), the pcap
capture/replay round-trip (`pcap_capture.ini`/`pcap_replay.ini`, General
config), and the SyncOnly gPTP-filtered capture. All three ran to completion
with real, sane numbers (not just "no crash" — e.g. capture file sizes,
packet counts).

## What's NOT yet confirmed (this is your primary job)

- **M2 (`nominal.ini`, 17-node multi-hop)** — the 5 fixes above are already
  applied to this file too, but no CI run has yet gotten far enough to prove
  it passes (see cascade note below). This is the highest-risk step per the
  original plan: multi-hop bridging is where INET 4.7's most structurally
  different gPTP machinery would surface first.
- **M3 (`congestion.ini`)**, **M4 (`feedback.ini`)**, **M5 (`sweep.ini`)** —
  same story: fixes already applied, unconfirmed whether they're sufficient.
- **Phase B's YAML→NED/ini generator (`scripts/gen_topology.py`)** — not
  checked against INET 4.7 at all. A prior sandbox session flagged that
  `TsnClock` may now `extend GptpMaster` directly rather than toggling a
  generic `gptp` submodule via `hasTimeSynchronization` — worth verifying
  before assuming the generator's output still matches hand-written NED.
  If the generated networks diverge, the CI step "Verify generated networks
  are bit-for-bit equivalent to hand-written" will fail loudly and tell you
  exactly what changed.
- **Why M2-M5 haven't been proven yet even though the fixes are pushed:**
  every earlier CI run failed on an *upstream* step (M1's analyze step, or a
  pcap step), and `ci.yml`'s M2-M5 steps don't have `if: always()` — a
  failure anywhere before them causes GitHub Actions to skip (not fail) all
  of M2 through the site-build step. Once M1's `Export + analyze telemetry`
  step passes clean (which the fix-5 above should finally achieve), M2-M5
  should actually execute for the first time. **Check the in-flight run
  (commit `68de868`) first** — it may have already answered this.

## How to work: sandbox-first, CI as the authority

You have what the CI runner doesn't: instant iteration. Use it that way —
don't round-trip every change through a 25-minute-cached CI run to find out
if an ini tweak works.

1. **Mirror the Dockerfile's env in your shell**, don't diverge from it:
   ```
   source /opt/omnetpp-6.4.0/setenv   # or wherever your build lives
   export LD_LIBRARY_PATH="$INET_ROOT/src:$OMNETPP_ROOT/lib"
   export NEDPATH="$INET_ROOT/src"
   ```
   Run scenarios directly with `opp_run` (or `scripts/run.sh`, adjusted for
   your local paths instead of the Docker `/work` mount) against
   `simulations/nominal.ini` etc. — this is exactly what `scripts/run.sh`
   does inside the container, just without `docker run` wrapping it.
2. **Iterate locally**: run M2, read the actual error (if any), fix the ini
   or script, rerun — seconds, not minutes. Use
   `python3 scripts/analyze.py <resultdir> --strict` locally the same way CI
   does, since that's the actual gate.
3. **Once a scenario passes locally**, commit the fix with a clear message
   (see this branch's existing commits for the expected level of detail —
   root-cause the *why*, not just the *what*, since that's this project's
   standard) and push to `claude/inet-4.7-omnetpp-6.4-migration`.
4. **Let CI be the final word.** Local runs prove it *can* work; CI (same
   Dockerfile, clean container, no locally-cached state) proves it reproduces
   for real. Don't consider a milestone done until CI shows it green.
5. **Once all of M1-M5 are green in real CI**, check Phase B's generated-network
   verification step too — it's already wired into `ci.yml` and will tell you
   immediately if the YAML DSL generator needs updating.
6. **Only after all of that is green**: this is the point to revisit
   README's "Why pinned to..." paragraph and decide whether to retire the
   OMNeT++ 6.0.3/INET 4.5.4 pin in favor of this branch, per the original
   plan's explicit decision gate (quoted below).

## The decision gate (user's explicit instruction, still standing)

> if this reaches a green M1-M5 rerun without disproportionate effort,
> propose merging it (retiring the 4.5.4 pin). If it stalls (deep NED/API
> breakage, unclear new-servo config surface, time sink with no end in
> sight), stop, document why in that branch's README/commit, and fall back
> to the current pin — wait for a later INET release rather than force it.

Every issue hit so far has been a well-understood, one-file config or script
fix (not a NED rewrite) — the spike is still clearly on the "proceed" side of
that gate. If M2's multi-hop bridging turns out to need real NED changes
(new bridge module types, restructured `masterPorts`/domain config), that's
the point to seriously weigh cost vs. the gate above, not before.

## Reference: files touched so far

- `Dockerfile` — OMNeT++ 6.4.0 + INET 4.7.0 pins, apt prerequisites
  (`pkg-config`, `ccache`, `python3-venv`, `doxygen`, `graphviz`, `xdg-utils`,
  `libdw-dev`), OMNeT++'s own `python/requirements.txt` installed before
  `configure`, name-agnostic INET archive extraction.
- `simulations/minimal.ini`, `nominal.ini`, `congestion.ini`, `feedback.ini`,
  `sweep.ini` — all five got `simtime-resolution = fs`,
  `**.clock.oscillator.nominalTickLength = 10ns`,
  `**.transmitter.typename = "StreamingTransmitter"`,
  `**.receiver.typename = "DestreamingReceiver"`.
- `simulations/pcap_capture.ini` — additionally `**.checksumMode =
  "computed"` and the unquoted `expr(...)` packetFilter fix.
- `scripts/simdata.py` — `parse_offset_series` helper, `HOP_MAPS` module
  suffix `.gptp` → `.clock`.
- `scripts/analyze.py`, `scripts/plot_results.py` — updated to use
  `parse_offset_series` against `timeChanged:vector` instead of the removed
  `timeDifference:vector`.
- `.github/workflows/ci.yml` — split into `build-and-smoke` +
  `deploy-pages` jobs (a job-level `environment:` gates the *entire* job's
  start on non-Pages branches — unrelated to INET, a general CI fix cherry-
  picked from the main branch).

## One environment gotcha worth relaying (from a prior sandbox session)

This CI-facing session's own network proxy blocks `github.com` release-asset
downloads and `add_repo` across GitHub owners outside the session's scope —
irrelevant to your sandbox if it already has OMNeT++/INET built, but if you
ever need to rebuild from scratch and hit the same wall, plain `git clone
--branch <tag> https://github.com/...` reportedly works even when the
release-tarball URL 403s.
