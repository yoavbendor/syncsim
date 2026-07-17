#!/usr/bin/env python3
"""
Post-run analysis for the gPTP sync sandbox.

Reads OMNeT++ result vectors and checks the M1 convergence property: every
syncing node's gPTP-measured offset from its master (INET's own
`gptp.timeDifference` signal) settles to a small value by the end of the run.

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


def load_vectors(csv_path: Path) -> pd.DataFrame:
    """Return the long-form vector rows (module, name, vectime, vecvalue)."""
    df = pd.read_csv(csv_path)
    return df[df.get("type", "vector") == "vector"] if "type" in df else df


def parse_series(cell) -> list[float]:
    """vecvalue/vectime cells are a single string of space/comma-separated numbers."""
    if not isinstance(cell, str) or not cell:
        return []
    return [float(x) for x in _NUM_RE.findall(cell)]


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
    print(f"[analyze] M1 convergence check (|final offset| < {args.max_offset_us:.0f} us):")
    for _, row in offset_rows.sort_values("module").iterrows():
        values = parse_series(row.get("vecvalue"))
        if not values:
            print(f"  {row['module']:30s} no samples")
            all_converged = False
            continue
        final = values[-1]
        peak = max(abs(v) for v in values)
        converged = abs(final) < threshold_s
        all_converged &= converged
        status = "PASS" if converged else "FAIL"
        print(f"  {row['module']:30s} final={final * 1e6:+9.2f}us  peak={peak * 1e6:9.2f}us  "
              f"n={len(values):5d}  [{status}]")

    if all_converged:
        print("[analyze] M1 convergence: PASS -- all syncing nodes converged.")
    else:
        print("[analyze] M1 convergence: FAIL -- see nodes above.", file=sys.stderr)

    return 0 if (all_converged or not args.strict) else 1


if __name__ == "__main__":
    sys.exit(main())
