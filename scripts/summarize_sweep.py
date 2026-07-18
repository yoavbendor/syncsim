#!/usr/bin/env python3
"""
Summarize a parameter sweep: one result set per iteration value (see
sweep.ini's output-scalar-file, which embeds the swept variable in the
filename, e.g. General-cap=5.sca). Prints the bottleneck's drop rate for
each iteration value side by side -- the actual "lever" comparison M5 is
about.

Usage:
    python3 scripts/summarize_sweep.py results-sweep --var cap \
        --bottleneck-module Nominal.swCore.eth[1].macLayer.queue
"""
import argparse
import glob
import re
import subprocess
import sys
from pathlib import Path

import pandas as pd


def export_scalars(sca_file: Path) -> pd.DataFrame:
    csv_path = sca_file.with_suffix(".csv")
    cmd = ["opp_scavetool", "export", "-T", "s", "-F", "CSV-R", "-o", str(csv_path), str(sca_file)]
    subprocess.run(cmd, check=True)
    df = pd.read_csv(csv_path)
    return df[df.get("type", "scalar") == "scalar"] if "type" in df else df


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("result_dir")
    ap.add_argument("--var", default="cap", help="name of the swept iteration variable")
    ap.add_argument("--bottleneck-module", default="Nominal.swCore.eth[1].macLayer.queue",
                     help="module to report offered/dropped packets for")
    args = ap.parse_args()

    result_dir = Path(args.result_dir)
    sca_files = sorted(glob.glob(str(result_dir / f"*-{args.var}=*.sca")))
    if not sca_files:
        print(f"[sweep] no *-{args.var}=*.sca files in {result_dir}", file=sys.stderr)
        return 1

    var_re = re.compile(rf"{re.escape(args.var)}=([^.]+)")
    rows = []
    for sca_file in sca_files:
        m = var_re.search(Path(sca_file).name)
        var_value = m.group(1) if m else "?"
        df = export_scalars(Path(sca_file))
        mod = df[df["module"] == args.bottleneck_module]

        def scalar(name: str) -> float:
            r = mod[mod["name"] == name]
            return float(r["value"].iloc[0]) if not r.empty else 0.0

        in_pk = scalar("incomingPackets:count")
        drop_pk = scalar("droppedPacketsQueueOverflow:count")
        drop_ppm = (drop_pk / in_pk * 1e6) if in_pk else 0.0
        rows.append((var_value, in_pk, drop_pk, drop_ppm))

    print(f"[sweep] bottleneck ({args.bottleneck_module}) vs {args.var}:")
    print(f"  {args.var:>8s}  {'offered pk':>12s}  {'dropped pk':>12s}  {'drop ppm':>10s}")
    for var_value, in_pk, drop_pk, drop_ppm in rows:
        print(f"  {var_value:>8s}  {in_pk:12.0f}  {drop_pk:12.0f}  {drop_ppm:10.1f}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
