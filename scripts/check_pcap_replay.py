#!/usr/bin/env python3
"""
C1 verification: confirm the pcap replay run in results-pcap-replay actually
delivered packets to coreClient's sink (not just that the modules wired up
without error). Usage: check_pcap_replay.py <result_dir>
"""
import sys
from pathlib import Path

import pandas as pd

from simdata import export_scalars_to_csv


def main() -> int:
    result_dir = Path(sys.argv[1] if len(sys.argv) > 1 else "results-pcap-replay")
    csv_path = export_scalars_to_csv(result_dir)
    if csv_path is None:
        print("[check_pcap_replay] no .sca files found", file=sys.stderr)
        return 1
    df = pd.read_csv(csv_path)
    rows = df[
        df["module"].str.contains("coreClient", na=False)
        & df["name"].str.contains("packetReceived", na=False)
    ]
    received = rows["value"].sum() if not rows.empty else 0
    print(f"[check_pcap_replay] coreClient sink received scalar sum: {received}")
    if received <= 0:
        print("[check_pcap_replay] FAIL: replay delivered zero packets to coreClient sink", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
