#!/usr/bin/env bash
# Run every iteration of a parameter-sweep config (an ini using OMNeT++'s
# native ${var=v1,v2,...} syntax) in one invocation. Differs from run.sh only
# by omitting -r: opp_run then executes all expanded runs of the config
# sequentially, each writing its own result files (see sweep.ini's
# output-vector-file/output-scalar-file, which embed the iteration variable
# in the filename so runs stay distinguishable).
# Usage: scripts/run_sweep.sh [CONFIG] [INI] [RESULT_DIR]
#   scripts/run_sweep.sh General simulations/sweep.ini results-sweep
set -euo pipefail

CONFIG="${1:-General}"
INI="${2:-simulations/sweep.ini}"
RESULT_DIR="${3:-results-sweep}"

: "${INET_ROOT:?INET_ROOT must be set (provided by the Docker image)}"

mkdir -p "$RESULT_DIR"
RESULT_DIR="$(cd "$RESULT_DIR" && pwd)"

echo ">> Running all iterations of config '$CONFIG' from '$INI' -> '$RESULT_DIR'"
opp_run \
    -l "$INET_ROOT/src/INET" \
    -n "$INET_ROOT/src:simulations" \
    -u Cmdenv \
    -c "$CONFIG" \
    --result-dir="$RESULT_DIR" \
    --cmdenv-express-mode=true \
    --cmdenv-status-frequency=10s \
    "$INI"

echo ">> Sweep finished. Results in '$RESULT_DIR':"
ls -la "$RESULT_DIR"
