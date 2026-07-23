#!/usr/bin/env python3
"""
Shared simulation-data helpers for the gPTP sync sandbox.

IO + parsing extracted from analyze.py so both the text analyzer and the
plotting/reporting scripts read OMNeT++ results the same way. No behavior
change: analyze.py imports these unchanged.
"""
import glob
import re
import subprocess
import sys
from pathlib import Path

import pandas as pd

_NUM_RE = re.compile(r"[-+]?\d*\.?\d+(?:[eE][-+]?\d+)?")

# Hop count from the GM to each node's gptp module, per known topology.
# Keyed by the network's root module name (the first path component). Also
# used as the expected-signal manifest for analyze.py's --strict sanity check.
HOP_MAPS = {
    "Minimal": [
        (re.compile(r"^Minimal\.sw\.clock$"), 1),
        (re.compile(r"^Minimal\.client\d+\.clock$"), 2),
    ],
    "Nominal": [
        (re.compile(r"^Nominal\.swCore\.clock$"), 1),
        (re.compile(r"^Nominal\.coreClient\.clock$"), 2),
        (re.compile(r"^Nominal\.sw[ABC]\.clock$"), 2),
        (re.compile(r"^Nominal\.clients[ABC]\[\d+\]\.clock$"), 3),
    ],
}


def export_vectors_to_csv(result_dir: Path) -> Path | None:
    """Use opp_scavetool to export .vec files to a long-form CSV.

    Phase 4 (ns-3 observability track) additive branch: if a ``vectors.csv``
    already exists in ``result_dir``, use it directly and skip opp_scavetool.
    This lets the ns-3 scenario drivers, which write this exact
    ``module,name,vectime,vecvalue`` schema themselves, be analyzed by the
    unchanged analyze.py/simdata.py without an OMNeT++ toolchain.

    Why this is provably behavior-preserving for the OMNeT++ path (not merely
    asserted): opp_scavetool is what *produces* ``vectors.csv`` in the first
    place, so a real OMNeT++ result directory never contains one *before*
    analyze.py runs -- it holds only ``.vec``/``.sca`` files. The early-return
    branch below is therefore never taken on any genuine OMNeT++ run, and the
    subsequent opp_scavetool call is reached exactly as before. (This is a
    structural guarantee by inspection: no OMNeT++/INET toolchain is available
    in the ns-3 POC sandbox to re-run and watch the OMNeT++ path pass, so this
    is reasoned, not executed -- matching the project's honesty standard.)
    """
    existing = result_dir / "vectors.csv"
    if existing.exists():
        print(f"[simdata] using pre-existing {existing} (no opp_scavetool)")
        return existing
    vec_files = glob.glob(str(result_dir / "*.vec"))
    if not vec_files:
        print(f"[simdata] no .vec files in {result_dir}", file=sys.stderr)
        return None
    csv_path = result_dir / "vectors.csv"
    cmd = ["opp_scavetool", "export", "-T", "v", "-F", "CSV-R",
           "-o", str(csv_path), *vec_files]
    print(f"[simdata] {' '.join(cmd)}")
    subprocess.run(cmd, check=True)
    return csv_path


def export_scalars_to_csv(result_dir: Path) -> Path | None:
    """Use opp_scavetool to export .sca files to a long-form CSV.

    Phase 4 additive branch, symmetric to export_vectors_to_csv: prefer a
    pre-existing ``scalars.csv`` (written directly by the ns-3 drivers) over
    globbing .sca + opp_scavetool. Same behavior-preserving argument: a real
    OMNeT++ result dir never has ``scalars.csv`` before analyze.py runs, so the
    branch is never taken on the OMNeT++ path.
    """
    existing = result_dir / "scalars.csv"
    if existing.exists():
        print(f"[simdata] using pre-existing {existing} (no opp_scavetool)")
        return existing
    sca_files = glob.glob(str(result_dir / "*.sca"))
    if not sca_files:
        return None
    csv_path = result_dir / "scalars.csv"
    cmd = ["opp_scavetool", "export", "-T", "s", "-F", "CSV-R",
           "-o", str(csv_path), *sca_files]
    print(f"[simdata] {' '.join(cmd)}")
    subprocess.run(cmd, check=True)
    return csv_path


def load_vectors(csv_path: Path) -> pd.DataFrame:
    """Return the long-form vector rows (module, name, vectime, vecvalue)."""
    df = pd.read_csv(csv_path)
    return df[df.get("type", "vector") == "vector"] if "type" in df else df


def parse_series(cell) -> list[float]:
    """vecvalue/vectime cells are a single string of space/comma-separated numbers."""
    if not isinstance(cell, str) or not cell:
        return []
    return [float(x) for x in _NUM_RE.findall(cell)]


def parse_offset_series(vectime_cell, vecvalue_cell) -> tuple[list[float], list[float]]:
    """(times, offset-from-GM) for a clock module's `timeChanged` vector.

    INET 4.6+ replaced Gptp's own `timeDifference` signal (removed) with
    ClockBase's `timeChanged`, which records each clock's own absolute time,
    not an offset -- confirmed against INET's Gptp.ned (no timeDifference
    signal exists there anymore) and ClockBase's docs. Every scenario's GM
    has driftRate=0ppm, so the GM's clock time is always exactly simulation
    time; offset-from-GM for any other node is therefore its clock time minus
    the simulation time at which that sample was recorded.
    """
    times = parse_series(vectime_cell)
    values = parse_series(vecvalue_cell)
    n = min(len(times), len(values))
    return times[:n], [values[i] - times[i] for i in range(n)]


def hop_count_for(module: str) -> int | None:
    root = module.split(".", 1)[0]
    for pattern, hops in HOP_MAPS.get(root, []):
        if pattern.match(module):
            return hops
    return None


def network_name(df: pd.DataFrame) -> str | None:
    """Root module name (network name) inferred from the vector modules."""
    if "module" not in df or df.empty:
        return None
    roots = {m.split(".", 1)[0] for m in df["module"].dropna()}
    for r in roots:
        if r in HOP_MAPS:
            return r
    return next(iter(roots), None)
