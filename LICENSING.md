# Licensing

This document maps out, in one place, what license governs which part of this
repository — the code syncsim itself authored, versus the simulators it
drives, which are fetched fresh at Docker build time and never vendored into
this repository's own history.

## syncsim's own code

Everything originally authored in this repository — `simulations/*.ned`/
`*.ini`, `configs/`, `scripts/*.py`, the `Dockerfile`, `docker/`,
`.github/workflows/`, and the `ns3/` tree — is licensed under the
**[Apache License 2.0](LICENSE)**. `ns3/` additionally carries its own copy
at [`ns3/LICENSE`](ns3/LICENSE) (identical text), so that subtree reads as
self-contained/independently reusable and its license is unambiguous to
anyone who clones or extracts just that directory.

This is syncsim's own choice as the author of these files, and it does not
depend on which simulator a given file happens to configure or drive.

## What's fetched at build time — not part of this repository

The Dockerfile downloads three external simulators; none of their source is
vendored into this repo, and none of their licenses apply to syncsim's own
files above. Each governs only itself and how the resulting *combined,
distributed binary* may be used:

| Dependency | Fetched from | License | Commercial use |
|---|---|---|---|
| **OMNeT++ kernel** (`headless`/`gui`/`ide` Dockerfile stages) | `github.com/omnetpp/omnetpp` release tarball | [Academic Public License](https://omnetpp.org/intro/license.html) | **Requires a paid OMNEST license** from the OMNeT++ project for any commercial/for-profit use. Free only for academic, non-profit-research, teaching, and personal non-commercial use. |
| **INET Framework** | `github.com/inet-framework/inet` release tarball | LGPL | Commercial use is fine; LGPL permits linking from proprietary/commercial code. INET was never the restriction — the OMNeT++ kernel it runs on top of is. |
| **ns-3** (`ns3` Dockerfile stage) | `gitlab.com/nsnam/ns-3-dev` (pinned `ns-3.45`) | GPLv2 | Commercial use is permitted; GPLv2's copyleft means modifications to ns-3 *itself* must be published if the combined binary is distributed. |

**The practical bottom line:** reorganizing or relicensing syncsim's own
files cannot change any of the above — running the OMNeT++ *kernel* for a
commercial purpose requires OMNEST regardless of how this repository is laid
out. The table exists purely so that constraint, and its actual scope
(the kernel, not INET, not syncsim's own `.ned`/`.ini`/scripts), is written
down and unambiguous rather than left as tribal knowledge.

## The ns-3 track's own honest caveat

`ns3/*.cc`/`*.h` are syncsim's clean-room, Apache-2.0-licensed original work
(see each file's SPDX header and the per-milestone `ns3/*/README.md` for the
full provenance statements). Because they link against ns-3 core at build
time, **the combined, distributed `ns3` Docker image / binary is governed by
GPLv2**, same as any ns-3 application. The Apache-2.0 header on syncsim's own
files means: syncsim owns that copyright and may reuse or relicense *those
specific files* elsewhere; it does not make the linked, combined artifact
permissive. This is stated in detail in `ns3/clock/README.md`,
`ns3/gptp/README.md`, and every subsequent `ns3/*/README.md` — repeated here
because it's the crux of why "the ns-3 track is Apache-2.0" is a claim about
authorship, not about the shape of the thing you'd actually ship.

## Current decision: ns-3 is the primary track; OMNeT++/INET is kept as a reference cross-check

**Superseded Phase 5 decision, revisited.** `NS3_MIGRATION_POC_PLAN.md`'s
original Phase 5 gate chose dual-track with OMNeT++/INET authoritative,
reasoning that the ns-3 track still carried real, disclosed gaps (pcap
capture/replay, IEEE-dissectable wire format, a trustworthy servo under
congestion, real hop-by-hop data forwarding). Tiers 1–3 of
`NS3_PARITY_PLAN.md` closed every one of those in turn — P1a's servo
hardening, P2c/P3c's pcap capture (now byte-exact IEEE 802.1AS, tshark-
dissectable), and P3a's real full-duplex hop-by-hop forwarding — so the
reasoning behind the original Phase 5 choice no longer holds.

**As of the P3c wire-format milestone, this repository's primary track is
`ns3/` (GPLv2, commercially usable, no Academic Public License restriction).
OMNeT++/INET is kept as a reference implementation for cross-checking new
work, not the default place development happens.** New scenario/feature work
targets the ns-3 track first; OMNeT++/INET stays green and gated in CI as the
independent, highest-fidelity cross-check it has been from the start, but is
no longer where "the real answer" is assumed to live by default.

If your own use case specifically needs to avoid the OMNeT++ kernel's
Academic Public License (e.g. any commercial use), the `ns3` Docker target is
now the primary, recommended path rather than a fallback — see `ns3/README.md`.
`NS3_MIGRATION_SURVEY.md`, `NS3_MIGRATION_POC_PLAN.md`, and `NS3_PARITY_PLAN.md`
are the full record of how this decision was reached and revisited.
</content>
