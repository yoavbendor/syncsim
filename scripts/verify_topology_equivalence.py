#!/usr/bin/env python3
"""
Phase B de-risk check: prove scripts/gen_topology.py's generated .ned/.ini
produce a bit-for-bit identical simulation to the hand-written original --
not just a structural diff of the text, but the same deterministic run
(same seed, same network name, same module hierarchy) compared vector-for-
vector and scalar-for-scalar. If this passes, the generated files are safe
to promote to the files CI actually runs; until it does, the hand-written
.ned/.ini stay authoritative.

Usage:
    verify_topology_equivalence.py <result_dir_a> <result_dir_b>
Exits non-zero (with a diff) on any mismatch.
"""
import sys
from pathlib import Path

import pandas as pd

from simdata import export_scalars_to_csv, export_vectors_to_csv


def _load_sorted(csv_path: Path | None, value_col: str) -> pd.DataFrame | None:
    if csv_path is None or not csv_path.exists():
        return None
    df = pd.read_csv(csv_path)
    keep = [c for c in ("module", "name", value_col) if c in df.columns]
    return df[keep].sort_values(["module", "name"]).reset_index(drop=True)


def compare(dir_a: Path, dir_b: Path) -> int:
    vec_a = _load_sorted(export_vectors_to_csv(dir_a), "vecvalue")
    vec_b = _load_sorted(export_vectors_to_csv(dir_b), "vecvalue")
    sca_a = _load_sorted(export_scalars_to_csv(dir_a), "value")
    sca_b = _load_sorted(export_scalars_to_csv(dir_b), "value")

    ok = True
    for label, a, b in (("vectors", vec_a, vec_b), ("scalars", sca_a, sca_b)):
        if a is None or b is None:
            print(f"[verify] {label}: missing export in one of the two result dirs")
            ok = False
            continue
        if len(a) != len(b):
            print(f"[verify] {label}: row count differs ({len(a)} vs {len(b)})")
            ok = False
            continue
        if not a["module"].equals(b["module"]) or not a["name"].equals(b["name"]):
            print(f"[verify] {label}: module/name keys differ after sorting")
            ok = False
            continue
        val_col = "vecvalue" if label == "vectors" else "value"
        mismatches = (a[val_col].astype(str) != b[val_col].astype(str))
        if mismatches.any():
            n = int(mismatches.sum())
            print(f"[verify] {label}: {n}/{len(a)} rows differ")
            print(a.loc[mismatches].head(5))
            print(b.loc[mismatches].head(5))
            ok = False
        else:
            print(f"[verify] {label}: {len(a)} rows identical -- OK")
    return 0 if ok else 1


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    return compare(Path(sys.argv[1]), Path(sys.argv[2]))


if __name__ == "__main__":
    raise SystemExit(main())
