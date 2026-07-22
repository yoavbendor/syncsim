#!/usr/bin/env bash
# Run a built ns-3 scratch scenario from the syncsim ns3/ track.
# Mirrors scripts/run.sh's role for the OMNeT++ track: a thin wrapper, not a
# reimplementation -- the scenario binary itself takes CommandLine args.
#
# Usage: ns3/scripts/run.sh <scenario-stem> [-- SCENARIO_ARGS...]
#   ns3/scripts/run.sh smoke-topology
#   ns3/scripts/run.sh smoke-topology -- --queueCapacity=10 --simTime=1.0
set -euo pipefail

SCENARIO="${1:?Usage: run.sh <scenario-stem> [-- SCENARIO_ARGS...]}"
shift || true
if [[ "${1:-}" == "--" ]]; then shift; fi

: "${NS3_ROOT:?NS3_ROOT must be set (provided by the Docker ns3 stage)}"

BIN=$(find "$NS3_ROOT/build/scratch" -maxdepth 2 -name "*-${SCENARIO}" -type f | head -1)
if [[ -z "$BIN" ]]; then
    echo "ERROR: no built scratch executable found for scenario '$SCENARIO'" >&2
    echo "  (looked under $NS3_ROOT/build/scratch for a file matching '*-${SCENARIO}')" >&2
    exit 1
fi

echo ">> Running ns-3 scenario '$SCENARIO' ($BIN)"
"$BIN" "$@"
