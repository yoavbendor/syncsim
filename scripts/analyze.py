#!/usr/bin/env python3
"""
Post-run analysis for the gPTP sync sandbox.

Reports every syncing node's gPTP-measured offset from its master (INET's own
`gptp.timeDifference` signal), grouped by hop count from the GM where the
topology is known (M2+), plus egress queue backlog and a congestion summary
(Mbps/pps/drop-ppm) derived from queue scalars.

Design note on --strict: this tool does NOT gate on offset magnitude. A large
peak offset under deliberate congestion (M3) is a real *result* to study, not
a bug -- the whole point of the sandbox is to produce numbers like that.
--strict instead asserts model-correctness sanity: the simulation produced
the expected signals, no series contains NaN/Inf, and no node's offset
diverges without bound (a "gPTP has clearly stopped functioning" ceiling, not
a sync-quality target). Fail only on evidence the simulator isn't faithfully
modeling the real protocol -- never on a scenario simply being harsh.

Usage:
    python3 scripts/analyze.py results [--divergence-ceiling-us 50000] [--strict]
"""
import argparse
import math
import sys
from pathlib import Path

import pandas as pd

from simdata import (
    HOP_MAPS,
    export_scalars_to_csv,
    export_vectors_to_csv,
    hop_count_for,
    load_vectors,
    parse_series,
)


def print_offset_report(offset_rows: pd.DataFrame) -> dict[str, list[float]]:
    """Print each node's final/peak offset and hop count. Returns module ->
    parsed sample list, for the sanity checks and hop-count summary to reuse."""
    by_hop: dict[int, list[float]] = {}
    series_by_module: dict[str, list[float]] = {}
    print("[analyze] gPTP offset-from-master per node:")
    for _, row in offset_rows.sort_values("module").iterrows():
        module = row["module"]
        values = parse_series(row.get("vecvalue"))
        series_by_module[module] = values
        if not values:
            print(f"  {module:30s} no samples")
            continue
        final = values[-1]
        peak = max(abs(v) for v in values)
        hops = hop_count_for(module)
        hop_label = f"hops={hops}" if hops is not None else "hops=?"
        print(f"  {module:30s} final={final * 1e6:+9.2f}us  peak={peak * 1e6:9.2f}us  "
              f"n={len(values):5d}  {hop_label}")
        if hops is not None:
            by_hop.setdefault(hops, []).append(peak)

    if by_hop:
        print("[analyze] peak offset by hop count from GM:")
        for hops in sorted(by_hop):
            peaks = by_hop[hops]
            mean_peak = sum(peaks) / len(peaks)
            print(f"  hops={hops}  n_nodes={len(peaks):3d}  "
                  f"mean_peak={mean_peak * 1e6:8.2f}us  max_peak={max(peaks) * 1e6:8.2f}us")

    return series_by_module


def print_time_windowed_report(offset_rows: pd.DataFrame, sim_time_s: float, num_windows: int) -> None:
    """Peak |offset| per node broken into equal time windows across the run.
    Whole-run peak/final (print_offset_report) can hide transient onset or
    oscillation within a constant-load run -- this surfaces it without
    requiring any new scenario mechanics (phases via runtime parameter
    changes were considered and skipped: whether ActivePacketSource's
    volatile productionInterval actually re-reads after a ScenarioManager
    set-param wasn't confirmed, and this gives most of the same visibility
    on data already collected)."""
    if num_windows <= 1:
        return
    window_size = sim_time_s / num_windows
    print(f"[analyze] peak |offset| by time window ({num_windows} x {window_size:.1f}s, in us):")
    header = "  " + f"{'module':30s}" + "".join(f"  w{i:<7d}" for i in range(num_windows))
    print(header)
    for _, row in offset_rows.sort_values("module").iterrows():
        module = row["module"]
        times = parse_series(row.get("vectime"))
        values = parse_series(row.get("vecvalue"))
        if not times or not values:
            continue
        window_peaks = [0.0] * num_windows
        for t, v in zip(times, values):
            idx = min(int(t // window_size), num_windows - 1)
            window_peaks[idx] = max(window_peaks[idx], abs(v))
        row_str = "  " + f"{module:30s}" + "".join(f"  {p * 1e6:7.2f}" for p in window_peaks)
        print(row_str)


def run_sanity_checks(series_by_module: dict[str, list[float]], divergence_ceiling_s: float) -> list[str]:
    """Model-correctness checks -- NOT sync-quality checks. Returns a list of
    failure descriptions (empty if all sane)."""
    failures = []

    root_candidates = {m.split(".", 1)[0] for m in series_by_module}
    expected_root = next((r for r in root_candidates if r in HOP_MAPS), None)
    if expected_root:
        for pattern, _hops in HOP_MAPS[expected_root]:
            matches = [m for m in series_by_module if pattern.match(m) and series_by_module[m]]
            if not matches:
                failures.append(f"expected signal category missing entirely: {pattern.pattern}")

    for module, values in series_by_module.items():
        if not values:
            failures.append(f"{module}: no samples recorded")
            continue
        if any(math.isnan(v) or math.isinf(v) for v in values):
            failures.append(f"{module}: NaN/Inf found in offset series")
            continue
        peak = max(abs(v) for v in values)
        if peak > divergence_ceiling_s:
            failures.append(
                f"{module}: offset diverged to {peak * 1e3:.2f}ms, exceeding the "
                f"{divergence_ceiling_s * 1e3:.0f}ms sanity ceiling -- gPTP appears to have "
                f"stopped functioning, not merely degraded"
            )

    return failures


def print_queue_and_congestion_summary(df: pd.DataFrame, result_dir: Path, sim_time_s: float) -> None:
    """Report per-port egress queue backlog (from queueLength:vector, always
    recorded -- it's part of sync dynamics, not general data traffic) and a
    congestion summary in Mbps/pps/drop-ppm derived from queue scalars whose
    units are confirmed in INET's PacketQueue.ned (packet lengths in bits,
    packet counts in pk)."""
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
    if not {"module", "name", "value"}.issubset(sdf.columns):
        return

    def scalar(module: str, name: str) -> float:
        rows = sdf[(sdf["module"] == module) & (sdf["name"] == name)]
        return float(rows["value"].iloc[0]) if not rows.empty else 0.0

    queue_modules = sorted(sdf.loc[sdf["module"].str.contains(r"\.macLayer\.queue$", na=False), "module"].unique())
    congestion_rows = []
    for module in queue_modules:
        in_pk = scalar(module, "incomingPackets:count")
        in_bits = scalar(module, "incomingPacketLengths:sum")
        drop_pk = scalar(module, "droppedPacketsQueueOverflow:count")
        if in_pk == 0 and in_bits == 0:
            continue
        congestion_rows.append((
            module,
            in_bits / sim_time_s / 1e6,       # offered Mbps
            in_pk / sim_time_s,                # offered pps
            (drop_pk / in_pk * 1e6) if in_pk else 0.0,  # drop ppm
        ))

    if congestion_rows:
        print(f"[analyze] data-plane congestion summary (offered load, {sim_time_s:.0f}s window):")
        for module, mbps, pps, drop_ppm in congestion_rows:
            print(f"  {module:35s} {mbps:8.2f} Mbps  {pps:9.1f} pps  drop={drop_ppm:10.1f} ppm")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("result_dir")
    ap.add_argument("--divergence-ceiling-us", type=float, default=50_000.0,
                     help="sanity ceiling on |offset|: exceeding this means gPTP has "
                          "stopped functioning, not just degraded (default 50ms)")
    ap.add_argument("--sim-time", type=float, default=60.0,
                     help="sim-time-limit used by the scenario, for Mbps/pps derivation")
    ap.add_argument("--time-windows", type=int, default=4,
                     help="split the run into this many equal windows and report peak "
                          "|offset| per window per node (set 1 to disable)")
    ap.add_argument("--strict", action="store_true",
                     help="fail (exit 1) on model-correctness sanity failures "
                          "(missing signals, NaN/Inf, unbounded divergence) -- "
                          "never on sync quality/magnitude")
    args = ap.parse_args()

    result_dir = Path(args.result_dir)
    csv_path = export_vectors_to_csv(result_dir)
    if csv_path is None:
        return 1 if args.strict else 0

    df = load_vectors(csv_path)
    names = sorted(df["name"].unique()) if "name" in df else []
    print(f"[analyze] available vector names: {names}")

    offset_rows = df[(df["name"] == "timeDifference:vector") & df["module"].str.endswith(".gptp")]
    if offset_rows.empty:
        print("[analyze] no gptp.timeDifference vectors found -- inspect the names above.", file=sys.stderr)
        return 1 if args.strict else 0

    series_by_module = print_offset_report(offset_rows)
    print_time_windowed_report(offset_rows, args.sim_time, args.time_windows)

    failures = run_sanity_checks(series_by_module, args.divergence_ceiling_us / 1e6)
    if failures:
        print("[analyze] sanity check: FAIL", file=sys.stderr)
        for f in failures:
            print(f"  - {f}", file=sys.stderr)
    else:
        print("[analyze] sanity check: PASS -- gPTP produced the expected, finite, bounded signals.")

    print_queue_and_congestion_summary(df, result_dir, args.sim_time)

    return 0 if (not failures or not args.strict) else 1


if __name__ == "__main__":
    sys.exit(main())
