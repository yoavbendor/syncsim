#!/usr/bin/env python3
"""
Generate a Mermaid topology-with-levers diagram for a scenario.

Phase B: if configs/topology/<network>.yaml exists, node/edge structure and
per-node drift come from that single-source-of-truth model (via
gen_topology.build_mermaid) instead of the hardcoded TOPOLOGIES map below --
the YAML file is now the ground truth for structure. Queue capacity and
sender detection stay scenario-specific, so they are still parsed from the
scenario's .ini either way. TOPOLOGIES remains as a fallback for any network
that hasn't been transcribed to YAML yet.

Returns both the Mermaid source (for the page) and a parsed-levers dict (for a
companion parameter table), so the picture and the numbers stay in sync.
"""
from __future__ import annotations

import sys
from pathlib import Path

_REPO_ROOT = Path(__file__).resolve().parent.parent

# node id -> role; edges as (parent, child), GM-rootward parent first.
TOPOLOGIES: dict[str, dict] = {
    "Minimal": {
        "nodes": [("gm", "gm"), ("sw", "switch"),
                  ("client1", "client"), ("client2", "client")],
        "edges": [("gm", "sw"), ("sw", "client1"), ("sw", "client2")],
    },
    "Nominal": {
        "nodes": (
            [("gm", "gm"), ("swCore", "switch"), ("coreClient", "client"),
             ("swA", "switch"), ("swB", "switch"), ("swC", "switch")]
            + [(f"clients{z}[{i}]", "client") for z in "ABC" for i in range(4)]
        ),
        "edges": (
            [("gm", "swCore"), ("swCore", "coreClient"),
             ("swCore", "swA"), ("swCore", "swB"), ("swCore", "swC")]
            + [(f"sw{z}", f"clients{z}[{i}]") for z in "ABC" for i in range(4)]
        ),
    },
}


def parse_ini(ini_path: Path) -> list[tuple[str, str]]:
    params = []
    for raw in Path(ini_path).read_text().splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line or line.startswith("[") or "=" not in line:
            continue
        k, v = line.split("=", 1)
        params.append((k.strip(), v.strip()))
    return params


def lookup(params: list[tuple[str, str]], *needles: str) -> str | None:
    """Last param value whose key contains all needles (later lines win)."""
    for k, v in reversed(params):
        if all(n in k for n in needles):
            return v
    return None


def _family_key(node: str) -> str:
    """clientsA[0] -> clientsA[* (matches the ini wildcard key fragment)."""
    if "[" in node:
        return node.split("[", 1)[0] + "[*"
    return node


def drift_for(params, node: str) -> str | None:
    for key in (f".{node}.", f".{_family_key(node)}"):
        v = lookup(params, key, "driftRate")
        if v:
            return v.replace("uniform(", "").replace(")", "").replace(", ", "..")
    return None


def is_sender(params, node: str) -> bool:
    for key in (f".{node}.app", f".{_family_key(node)}].app", f".{_family_key(node)}.app"):
        v = lookup(params, key, "typename")
        if v and "Source" in v:
            return True
    return False


def queue_cap(params) -> str:
    v = lookup(params, ".macLayer.queue.packetCapacity")
    if v is None:
        return "unbounded (default PacketQueue)"
    if v.startswith("${"):
        inner = v.strip("${} ").split("=", 1)[-1]
        return f"swept {{{inner}}}"
    return v


def link_speed(params) -> str:
    return lookup(params, ".eth[*].bitrate") or "100Mbps"


def build_mermaid(network: str, ini_path: Path) -> tuple[str, dict]:
    yaml_path = _REPO_ROOT / "configs" / "topology" / f"{network.lower()}.yaml"
    params = parse_ini(ini_path)
    if yaml_path.exists():
        import gen_topology
        model = gen_topology.load_model(yaml_path)
        sender_names = set()
        for n in model["nodes"]:
            is_vec = "count" in n or "count_param" in n
            rep = f"{n['name']}[0]" if is_vec else n["name"]
            if is_sender(params, rep):
                sender_names.add(n["name"])
        mermaid, levers = gen_topology.build_mermaid(model, sender_names)
        levers["switch queue capacity"] = queue_cap(params)
        levers["senders"] = (
            [f"{n['name']}[*]" if ("count" in n or "count_param" in n) else n["name"]
             for n in model["nodes"] if n["name"] in sender_names]
            or ["none"]
        )
        return mermaid, levers

    topo = TOPOLOGIES[network]
    speed = link_speed(params)
    cap = queue_cap(params)

    def label(node: str, role: str) -> str:
        parts = [node]
        if role == "gm":
            parts = [f"{node} ⏱ GM"]
        drift = drift_for(params, node)
        if drift and drift not in ("0ppm",):
            parts.append(f"drift {drift}")
        if is_sender(params, node):
            parts.append("📤 sender")
        return "<br/>".join(parts)

    def nid(node: str) -> str:
        return node.replace("[", "_").replace("]", "")

    lines = ["graph TD"]
    for node, role in topo["nodes"]:
        lines.append(f'  {nid(node)}["{label(node, role)}"]:::{role}')
    for a, b in topo["edges"]:
        lines.append(f"  {nid(a)} --- {nid(b)}")
    lines += [
        "  classDef gm fill:#f6c343,stroke:#b8860b,color:#222,stroke-width:2px;",
        "  classDef switch fill:#4a90d9,stroke:#26537a,color:#fff;",
        "  classDef client fill:#5cb85c,stroke:#2f6f2f,color:#fff;",
    ]
    mermaid = "\n".join(lines)

    levers = {
        "network": network,
        "link speed": speed,
        "switch queue capacity": cap,
        "grandmaster": next((n for n, r in topo["nodes"] if r == "gm"), "?"),
        "nodes": len(topo["nodes"]),
        "senders": [n for n, _ in topo["nodes"] if is_sender(params, n)] or ["none"],
    }
    return mermaid, levers


if __name__ == "__main__":
    net, ini = sys.argv[1], Path(sys.argv[2])
    m, lv = build_mermaid(net, ini)
    print(m)
    print("\n-- levers --")
    for k, v in lv.items():
        print(f"{k}: {v}")
