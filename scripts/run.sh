#!/usr/bin/env bash
# Run a scenario headless with opp_run + the INET shared library.
# CONFIG is an omnetpp.ini [Config <name>] *section* name (default: General),
# not the NED network name -- those are two different things.
# Usage: scripts/run.sh [CONFIG] [INI] [RESULT_DIR]
#   scripts/run.sh General simulations/minimal.ini results
set -euo pipefail

CONFIG="${1:-General}"
INI="${2:-simulations/minimal.ini}"
RESULT_DIR="${3:-results}"

: "${INET_ROOT:?INET_ROOT must be set (provided by the Docker image)}"

mkdir -p "$RESULT_DIR"

echo ">> Running config '$CONFIG' from '$INI' -> '$RESULT_DIR'"
opp_run \
    -l "$INET_ROOT/src/INET" \
    -n "$INET_ROOT/src:simulations" \
    -u Cmdenv \
    -c "$CONFIG" \
    -r 0 \
    --result-dir="$RESULT_DIR" \
    --cmdenv-express-mode=true \
    --cmdenv-status-frequency=10s \
    "$INI"

echo ">> Simulation finished. Results in '$RESULT_DIR':"
ls -la "$RESULT_DIR"
