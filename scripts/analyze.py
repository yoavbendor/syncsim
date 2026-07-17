#!/usr/bin/env python3
"""
Post-run analysis for the gPTP sync sandbox.

Reads OMNeT++ result vectors and checks the convergence property: every
syncing node's gPTP-measured offset from its master (INET's own
`gptp.timeDifference` signal) settles to a small value by the end of the run.

For known multi-hop topologies (M2+), also groups nodes by hop count from the
GM and reports how peak sync error grows with hop depth -- this is the
hop-by-hop peer-delay / residence-time accumulation effect (IEEE 802.1AS
transparent/boundary-clock behavior).

Usage:
    python3 scripts/analyze.py results [--max-offset-us 1000] [--strict]
"""
import argparse
import glob
import re
import subprocess
import sys
from pathlib import Path

import pandas as pd

_NUM_RE = re.compile(r"[-+]?\d*\.?\d+(?:[eE][-+]?\d+)?")

# Hop count from the GM to each node's gptp module, per known topology.
# Keyed by the network's root module name (the first path component).
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


def export_vectors_to_csv(result_dir: Path) -> Path:
    """Use opp_scavetool to export .vec files to a long-form CSV."""
    vec_files = glob.glob(str(result_dir / "*.vec"))
    if not vec_files:
        print(f"[analyze] no .vec files in {result_dir}", file=sys.stderr)
        return None
    csv_path = result_dir / "vectors.csv"
    cmd = ["opp_scavetool", "export", "-T", "v", "-F", "CSV-R",
           "-o", str(csv_path), *vec_files]
    print(f"[analyze] {' '.join(cmd)}")
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
    print(f"[analyze] {' '.join(cmd)}")
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


def print_queue_summary(df: pd.DataFrame, result_dir: Path) -> None:
    """Report per-port egress queue backlog (from queueLength:vector) and, if
    present, packet drop counts (from the .sca scalars) -- makes real TSN
    switch congestion (M3: finite DropTailQueue under offered load) directly
    observable rather than only inferred from timing effects."""
    queue_rows = df[(df["name"] == "queueLength:vector") & df["module"].str.contains(r"\.macLayer\.queue$")]
    if not queue_rows.empty:
        print("[analyze] egress queue backlog (packets):")
        for _, row in queue_rows.sort_values("module").iterrows():
            values = parse_series(row.get("vecvalue"))
            if not values:
                continue
            print(f"  {row['module']:35s} max={max(values):5.0f}  mean={sum(values) / len(values):6.2f}")

    scalars_csv = export_scalars_to_csv(result_dir)
    if scalars_csv is None:
        return
    sdf = pd.read_csv(scalars_csv)
    sdf = sdf[sdf.get("type", "scalar") == "scalar"] if "type" in sdf else sdf
    if "name" not in sdf:
        return
    dropped = sdf[sdf["name"].str.contains("dropped", case=False, na=False) & (sdf.get("value", 0) > 0)]
    if not dropped.empty:
        print("[analyze] packet drops (nonzero scalars):")
        for _, row in dropped.sort_values("module").iterrows():
            print(f"  {row['module']:35s} {row['name']:35s} {row['value']:.0f}")
    else:
        print("[analyze] no nonzero drop scalars found.")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("result_dir")
    ap.add_argument("--max-offset-us", type=float, default=1000.0,
                     help="convergence bound on |final gptp offset| in microseconds")
    ap.add_argument("--strict", action="store_true",
                     help="fail (exit 1) if signals are missing or a node doesn't converge")
    args = ap.parse_args()

    result_dir = Path(args.result_dir)
    csv_path = export_vectors_to_csv(result_dir)
    if csv_path is None:
        return 1 if args.strict else 0

    df = load_vectors(csv_path)
    names = sorted(df["name"].unique()) if "name" in df else []
    print(f"[analyze] available vector names: {names}")

    # INET's Gptp module records the measured offset-from-master as
    # `timeDifference:vector` on each syncing node's `<node>.gptp` submodule.
    offset_rows = df[(df["name"] == "timeDifference:vector") & df["module"].str.endswith(".gptp")]
    if offset_rows.empty:
        print("[analyze] no gptp.timeDifference vectors found -- inspect the names above.", file=sys.stderr)
        return 1 if args.strict else 0

    threshold_s = args.max_offset_us / 1e6
    all_converged = True
    by_hop: dict[int, list[float]] = {}
    print(f"[analyze] convergence check (|final offset| < {args.max_offset_us:.0f} us):")
    for _, row in offset_rows.sort_values("module").iterrows():
        module = row["module"]
        values = parse_series(row.get("vecvalue"))
        if not values:
            print(f"  {module:30s} no samples")
            all_converged = False
            continue
        final = values[-1]
        peak = max(abs(v) for v in values)
        converged = abs(final) < threshold_s
        all_converged &= converged
        status = "PASS" if converged else "FAIL"
        hops = hop_count_for(module)
        hop_label = f"hops={hops}" if hops is not None else "hops=?"
        print(f"  {module:30s} final={final * 1e6:+9.2f}us  peak={peak * 1e6:9.2f}us  "
              f"n={len(values):5d}  {hop_label}  [{status}]")
        if hops is not None:
            by_hop.setdefault(hops, []).append(peak)

    if by_hop:
        print("[analyze] peak offset by hop count from GM:")
        for hops in sorted(by_hop):
            peaks = by_hop[hops]
            mean_peak = sum(peaks) / len(peaks)
            print(f"  hops={hops}  n_nodes={len(peaks):3d}  "
                  f"mean_peak={mean_peak * 1e6:8.2f}us  max_peak={max(peaks) * 1e6:8.2f}us")

    if all_converged:
        print("[analyze] convergence: PASS -- all syncing nodes converged.")
    else:
        print("[analyze] convergence: FAIL -- see nodes above.", file=sys.stderr)

    print_queue_summary(df, result_dir)

    return 0 if (all_converged or not args.strict) else 1


if __name__ == "__main__":
    sys.exit(main())
