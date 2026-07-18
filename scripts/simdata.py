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
        (re.compile(r"^Minimal\.sw\.gptp$"), 1),
        (re.compile(r"^Minimal\.client\d+\.gptp$"), 2),
    ],
    "Nominal": [
        (re.compile(r"^Nominal\.swCore\.gptp$"), 1),
        (re.compile(r"^Nominal\.coreClient\.gptp$"), 2),
        (re.compile(r"^Nominal\.sw[ABC]\.gptp$"), 2),
        (re.compile(r"^Nominal\.clients[ABC]\[\d+\]\.gptp$"), 3),
    ],
}


def export_vectors_to_csv(result_dir: Path) -> Path | None:
    """Use opp_scavetool to export .vec files to a long-form CSV."""
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
    """Use opp_scavetool to export .sca files to a long-form CSV."""
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
