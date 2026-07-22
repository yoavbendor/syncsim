#!/usr/bin/env python3
"""
Per-scenario visual report for the gPTP sync sandbox.

Reads OMNeT++ results (via simdata), renders matplotlib PNGs, writes a
self-contained HTML fragment (title + plain-language verdict + Mermaid
topology-with-levers + parameter table + plots). build_site.py concatenates
the fragments into the GitHub Pages index.

Behavior dynamics are matplotlib; static structure/levers are Mermaid --
complementary, per the plan.

Usage:
    plot_results.py --result-dir results-congestion --slug m3-congestion \
        --title "M3 - Congestion" --network Nominal \
        --ini simulations/congestion.ini --site site --kind single
"""
from __future__ import annotations

import argparse
import glob
import subprocess
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import pandas as pd  # noqa: E402

from gen_mermaid import build_mermaid  # noqa: E402
from simdata import (  # noqa: E402
    export_scalars_to_csv,
    export_vectors_to_csv,
    hop_count_for,
    load_vectors,
    parse_offset_series,
    parse_series,
)

# OMNeT++'s array-gate module-name syntax. The ns-3 track (Phase 4) uses the
# equivalent module without brackets ("...eth1..."), since ns-3 has no gate
# syntax -- matching against both keeps this constant working for either
# source without needing a CLI flag; an OMNeT++ result never contains the
# bracket-free form, so this is purely additive.
BOTTLENECK = "Nominal.swCore.eth[1].macLayer.queue"
BOTTLENECK_ALIASES = {BOTTLENECK, "Nominal.swCore.eth1.macLayer.queue"}


def _offset_series(df: pd.DataFrame) -> dict[str, tuple[list[float], list[float]]]:
    """module -> (times_s, |offset|_us) for every clock.timeChanged node."""
    rows = df[(df["name"] == "timeChanged:vector") & df["module"].str.endswith(".clock")]
    out = {}
    for _, r in rows.iterrows():
        t, offsets = parse_offset_series(r.get("vectime"), r.get("vecvalue"))
        v = [abs(x) * 1e6 for x in offsets]
        if t and v:
            out[r["module"]] = (t, v)
    return out


def _resolve_bottleneck(df: pd.DataFrame) -> str:
    """Whichever BOTTLENECK_ALIASES form actually appears in this df's modules
    (OMNeT++'s bracket form or ns-3's bracket-free form); falls back to the
    OMNeT++ form (existing behavior) if neither is present."""
    present = set(df.get("module", pd.Series(dtype=str)))
    for alias in BOTTLENECK_ALIASES:
        if alias in present:
            return alias
    return BOTTLENECK


def _queue_series(df: pd.DataFrame, module: str) -> tuple[list[float], list[float]]:
    rows = df[(df["name"] == "queueLength:vector") & (df["module"] == module)]
    if rows.empty:
        return [], []
    r = rows.iloc[0]
    return parse_series(r.get("vectime")), parse_series(r.get("vecvalue"))


def plot_offsets(offsets, out_png: Path, title: str) -> str | None:
    if not offsets:
        return None
    worst = max(offsets, key=lambda m: max(offsets[m][1]))
    fig, ax = plt.subplots(figsize=(9, 4.5))
    for mod, (t, v) in offsets.items():
        if mod == worst:
            continue
        ax.plot(t, [max(x, 0.01) for x in v], color="#bbbbbb", lw=0.8, alpha=0.7)
    t, v = offsets[worst]
    ax.plot(t, [max(x, 0.01) for x in v], color="#d9534f", lw=2.0,
            label=f"{worst.split('.')[1]} (worst, peak {max(v):.0f} us)")
    ax.set_yscale("log")
    ax.set_xlabel("simulation time (s)")
    ax.set_ylabel("|offset from GM| (us, log)")
    ax.set_title(f"{title}: sync error over time ({len(offsets)} devices)")
    ax.grid(True, which="both", ls=":", alpha=0.4)
    ax.legend(loc="upper right", fontsize=9)
    fig.tight_layout()
    fig.savefig(out_png, dpi=110)
    plt.close(fig)
    return out_png.name


def plot_backlog(df, out_png: Path, title: str) -> str | None:
    t, v = _queue_series(df, _resolve_bottleneck(df))
    if not t:
        return None
    fig, ax = plt.subplots(figsize=(9, 3.2))
    ax.plot(t, v, color="#4a90d9", lw=1.0)
    ax.set_xlabel("simulation time (s)")
    ax.set_ylabel("queue backlog (packets)")
    ax.set_title(f"{title}: bottleneck queue backlog (swCore->coreClient link)")
    ax.grid(True, ls=":", alpha=0.4)
    fig.tight_layout()
    fig.savefig(out_png, dpi=110)
    plt.close(fig)
    return out_png.name


def plot_coupling(offsets, df, out_png: Path, title: str) -> str | None:
    cc = next((m for m in offsets if m.endswith("coreClient.clock")), None)
    tq, vq = _queue_series(df, _resolve_bottleneck(df))
    if cc is None or not tq:
        return None
    to, vo = offsets[cc]
    fig, ax1 = plt.subplots(figsize=(9, 4.0))
    ax1.plot(to, [max(x, 0.01) for x in vo], color="#d9534f", lw=1.6)
    ax1.set_xlabel("simulation time (s)")
    ax1.set_ylabel("coreClient |offset| (us)", color="#d9534f")
    ax1.tick_params(axis="y", labelcolor="#d9534f")
    ax2 = ax1.twinx()
    ax2.plot(tq, vq, color="#4a90d9", lw=0.9, alpha=0.7)
    ax2.set_ylabel("bottleneck backlog (packets)", color="#4a90d9")
    ax2.tick_params(axis="y", labelcolor="#4a90d9")
    ax1.set_title(f"{title}: does congestion move sync? (offset vs backlog)")
    fig.tight_layout()
    fig.savefig(out_png, dpi=110)
    plt.close(fig)
    return out_png.name


def plot_hopbar(offsets, out_png: Path, title: str) -> str | None:
    by_hop: dict[int, list[float]] = {}
    for mod, (_t, v) in offsets.items():
        h = hop_count_for(mod)
        if h is not None:
            by_hop.setdefault(h, []).append(max(v))
    if len(by_hop) < 2:
        return None
    hops = sorted(by_hop)
    means = [sum(by_hop[h]) / len(by_hop[h]) for h in hops]
    maxes = [max(by_hop[h]) for h in hops]
    fig, ax = plt.subplots(figsize=(6, 3.6))
    x = range(len(hops))
    ax.bar([i - 0.2 for i in x], means, width=0.4, label="mean peak", color="#5cb85c")
    ax.bar([i + 0.2 for i in x], maxes, width=0.4, label="max peak", color="#f6c343")
    ax.set_xticks(list(x))
    ax.set_xticklabels([f"{h} hop{'s' if h > 1 else ''}" for h in hops])
    ax.set_ylabel("peak |offset| (us)")
    ax.set_title(f"{title}: sync error vs hops from GM")
    ax.legend(fontsize=9)
    fig.tight_layout()
    fig.savefig(out_png, dpi=110)
    plt.close(fig)
    return out_png.name


def sweep_bar(result_dir: Path, out_png: Path, title: str) -> tuple[str | None, list[tuple[str, float]]]:
    # Phase 4 additive branch, same reasoning as simdata.export_scalars_to_csv
    # and summarize_sweep.py's matching patch: prefer .sca (OMNeT++, exported
    # via opp_scavetool as before) but fall back to a pre-exported .csv sibling
    # (the ns-3 M5 sweep in ns3/scripts/run_sweep.sh writes these directly, no
    # opp_scavetool available). A genuine OMNeT++ sweep dir always has .sca
    # files, so that path is unchanged.
    sca_files = sorted(glob.glob(str(result_dir / "*-cap=*.sca")))
    is_csv = False
    if not sca_files:
        sca_files = sorted(glob.glob(str(result_dir / "*-cap=*.csv")))
        is_csv = True
    pts = []
    for f in sca_files:
        cap = Path(f).name.split("cap=")[1].split(".")[0]
        if is_csv:
            csv = Path(f)
        else:
            csv = Path(f).with_suffix(".csv")
            subprocess.run(["opp_scavetool", "export", "-T", "s", "-F", "CSV-R", "-o", str(csv), f], check=True)
        sdf = pd.read_csv(csv)
        sdf = sdf[sdf.get("type", "scalar") == "scalar"] if "type" in sdf else sdf
        mod = sdf[sdf["module"].isin(BOTTLENECK_ALIASES)]

        def sc(name):
            r = mod[mod["name"] == name]
            return float(r["value"].iloc[0]) if not r.empty else 0.0
        inp = sc("incomingPackets:count")
        drp = sc("droppedPacketsQueueOverflow:count")
        pts.append((cap, (drp / inp * 100) if inp else 0.0))
    if not pts:
        return None, []
    pts.sort(key=lambda p: int(p[0]))
    fig, ax = plt.subplots(figsize=(6, 3.6))
    ax.bar([p[0] for p in pts], [p[1] for p in pts], color="#4a90d9")
    ax.set_xlabel("queue capacity (packets)")
    ax.set_ylabel("drop rate (%)")
    ax.set_ylim(0, max(50, max(p[1] for p in pts) * 1.3))
    ax.set_title(f"{title}: drop rate vs queue capacity")
    for p in pts:
        ax.text(p[0], p[1] + 1, f"{p[1]:.1f}%", ha="center", fontsize=9)
    fig.tight_layout()
    fig.savefig(out_png, dpi=110)
    plt.close(fig)
    return out_png.name, pts


def narrative_single(offsets, df) -> list[str]:
    if not offsets:
        return ["No sync signals were recorded for this scenario."]
    peaks = {m: max(v) for m, (_t, v) in offsets.items()}
    worst, worst_peak = max(peaks.items(), key=lambda kv: kv[1])
    others = [p for m, p in peaks.items() if m != worst]
    typical = (sum(others) / len(others)) if others else worst_peak
    lines = [f"{len(offsets)} devices synchronize to a single grandmaster over gPTP."]
    if worst_peak < 100:
        lines.append(
            f"All of them stay tightly synced -- worst-case error only {worst_peak:.0f} microseconds "
            f"(a microsecond is a millionth of a second). The network is healthy.")
    else:
        nm = worst.split(".")[1]
        lines.append(
            f"Most devices stay tightly synced (around {typical:.0f} microseconds), but one -- "
            f"{nm} -- degrades to {worst_peak:.0f} microseconds, roughly {worst_peak / max(typical, 1):.0f}x worse, "
            f"because it shares a congested link with heavy data traffic.")
    t, v = _queue_series(df, _resolve_bottleneck(df))
    if v and max(v) > 1:
        lines.append(
            f"The shared bottleneck queue fills up (peak {max(v):.0f} packets), the visible cause of the strain.")
    return lines


def render_fragment(slug, title, narrative, mermaid, levers, images, extra_note="") -> str:
    imgs = "\n".join(
        f'<figure><img src="{img}" alt="{cap}"><figcaption>{cap}</figcaption></figure>'
        for cap, img in images if img)
    lever_rows = "\n".join(
        f"<tr><th>{k}</th><td>{', '.join(v) if isinstance(v, list) else v}</td></tr>"
        for k, v in levers.items())
    narr = "".join(f"<p>{s}</p>" for s in narrative)
    note = f'<p class="note">{extra_note}</p>' if extra_note else ""
    return f"""
<section id="{slug}">
  <h2>{title}</h2>
  <div class="verdict">{narr}{note}</div>
  <div class="two-col">
    <div class="diagram">
      <h3>Network &amp; levers</h3>
      <pre class="mermaid">{mermaid}</pre>
      <table class="levers">{lever_rows}</table>
    </div>
    <div class="plots">{imgs}</div>
  </div>
</section>
"""


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--result-dir", required=True)
    ap.add_argument("--slug", required=True)
    ap.add_argument("--title", required=True)
    ap.add_argument("--network", required=True)
    ap.add_argument("--ini", required=True)
    ap.add_argument("--site", required=True)
    ap.add_argument("--kind", choices=["single", "sweep"], default="single")
    args = ap.parse_args()

    result_dir = Path(args.result_dir)
    site = Path(args.site)
    site.mkdir(parents=True, exist_ok=True)

    mermaid, levers = build_mermaid(args.network, Path(args.ini))

    if args.kind == "sweep":
        img, pts = sweep_bar(result_dir, site / f"{args.slug}-sweep.png", args.title)
        flat = pts and (max(p[1] for p in pts) - min(p[1] for p in pts) < 2)
        narrative = [
            "This run sweeps the switch buffer size across several values to see how it changes packet loss."]
        if flat:
            narrative.append(
                "The drop rate barely moves (" +
                ", ".join(f"cap {c}: {d:.1f}%" for c, d in pts) +
                "). Under sustained overload, a bigger buffer only adds delay -- it does not reduce loss.")
        images = [("Drop rate vs queue capacity", img)]
        frag = render_fragment(args.slug, args.title, narrative, mermaid, levers, images)
    else:
        vcsv = export_vectors_to_csv(result_dir)
        if vcsv is None:
            (site / f"{args.slug}.frag.html").write_text(
                render_fragment(args.slug, args.title,
                                ["No vector results were produced for this scenario."],
                                mermaid, levers, []))
            return 0
        df = load_vectors(vcsv)
        export_scalars_to_csv(result_dir)
        offsets = _offset_series(df)
        images = [
            ("Sync error over time", plot_offsets(offsets, site / f"{args.slug}-offset.png", args.title)),
            ("Bottleneck queue backlog", plot_backlog(df, site / f"{args.slug}-backlog.png", args.title)),
            ("Offset vs backlog (coupling)", plot_coupling(offsets, df, site / f"{args.slug}-coupling.png", args.title)),
            ("Sync error vs hop count", plot_hopbar(offsets, site / f"{args.slug}-hops.png", args.title)),
        ]
        narrative = narrative_single(offsets, df)
        frag = render_fragment(args.slug, args.title, narrative, mermaid, levers, images)

    (site / f"{args.slug}.frag.html").write_text(frag)
    print(f"[plot] wrote {args.slug}.frag.html and PNGs into {site}/")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
