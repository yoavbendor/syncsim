#!/usr/bin/env bash
# ns-3 M5 parameter sweep -- the ns-3-track analog of scripts/run_sweep.sh +
# simulations/sweep.ini's `${cap = 5, 20, 80}` iteration.
#
# ns-3 has no native ${var=...} config-iteration syntax, so the "sweep" is a
# thin shell loop that runs the SAME built scenario binary once per queue-
# capacity value, driving the --queueCapacity CommandLine lever (an alias for
# --queueCap, named to mirror sweep.ini's `cap`). Each iteration writes its own
# scalars.csv, which we rename to sweep.ini's exact `<name>-cap=<value>` file
# convention (its output-scalar-file) so scripts/summarize_sweep.py's discovery
# pattern (`*-cap=*`) transfers to the ns-3 track UNCHANGED -- reuse over a
# parallel script, consistent with the rest of Phase 4.
#
# Usage: ns3/scripts/run_sweep.sh [SCENARIO] [RESULT_DIR] [CAP...]
#   NS3_ROOT=/path/to/ns-3-dev ns3/scripts/run_sweep.sh
#   NS3_ROOT=/path/to/ns-3-dev ns3/scripts/run_sweep.sh congestion-topology out 5 20 80
set -euo pipefail

SCENARIO="${1:-congestion-topology}"
RESULT_DIR="${2:-results-ns3-sweep}"
if [[ $# -gt 2 ]]; then CAPS=("${@:3}"); else CAPS=(5 20 80); fi

: "${NS3_ROOT:?NS3_ROOT must be set (the pinned ns-3 checkout root)}"

BIN=$(find "$NS3_ROOT/build/scratch" -maxdepth 2 -name "*-${SCENARIO}" -type f | head -1)
if [[ -z "$BIN" ]]; then
    echo "ERROR: no built scratch executable found for scenario '$SCENARIO'" >&2
    echo "  (looked under $NS3_ROOT/build/scratch for a file matching '*-${SCENARIO}')" >&2
    exit 1
fi

mkdir -p "$RESULT_DIR"
RESULT_DIR="$(cd "$RESULT_DIR" && pwd)"
NAME="${SCENARIO%-topology}"

echo ">> ns-3 sweep of '$SCENARIO' over queueCapacity = ${CAPS[*]} -> '$RESULT_DIR'"
for cap in "${CAPS[@]}"; do
    ITER_DIR="$RESULT_DIR/cap=$cap"
    mkdir -p "$ITER_DIR"
    echo ">> iteration cap=$cap"
    "$BIN" --queueCapacity="$cap" --resultDir="$ITER_DIR" --simTime=30 >/dev/null
    cp "$ITER_DIR/scalars.csv" "$RESULT_DIR/${NAME}-cap=${cap}.csv"
done

echo ">> Sweep finished. Per-iteration scalar CSVs (sweep.ini <name>-cap=<value> naming):"
ls -1 "$RESULT_DIR"/*-cap=*.csv
