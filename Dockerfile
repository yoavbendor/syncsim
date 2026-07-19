# Headless OMNeT++ + INET simulator image for the gPTP/TSN sync sandbox.
#
# MIGRATION SPIKE (branch claude/inet-4.7-omnetpp-6.4-migration): bumping from
# the proven OMNeT++ 6.0.3 + INET 4.5.4 pin (see claude/sync-simulation-tool-p6ade4)
# to OMNeT++ 6.4.x + INET 4.7.x. INET 4.6 shipped a backward-incompatible
# reimplementation of both gPTP (formal state machines, dedicated clock-servo
# submodule) and the clock model (rewritten arithmetic, femtosecond
# simtime-resolution needed for accurate 1ppm drift) -- exactly the two
# subsystems this sandbox studies -- so this is a real re-validation project,
# not a version bump. Exact asset filenames below are best-effort (matching
# the established omnetpp-X.Y.Z-linux-x86_64.tgz / vX.Y.Z + inet-X.Y.Z-src.tgz
# naming convention); this session's network access can't browse GitHub's
# release-asset listing directly, so the real CI build is what confirms or
# corrects them -- consistent with how every other version-sensitive detail
# in this project has been verified. First build attempt confirmed the asset
# URLs were right but failed later on a missing pkg-config (6.0.3 did not
# need it: "configure: error: pkg-config program not found"). Rather than
# keep discovering 6.4.0's other new prerequisites one slow CI round at a
# time, the rest of this list was front-loaded from OMNeT++'s own documented
# install.sh prerequisites for Ubuntu (ccache, python3-venv, doxygen,
# graphviz, xdg-utils, libdw-dev) -- this sandbox has no Docker daemon and
# this session's GitHub access can't cross-add the omnetpp/omnetpp repo to
# actually build it locally, so the official dependency list is the next
# best thing to a local repro; the real CI build still confirms it.
# The build is heavy (~20-30 min first time); CI caches it via buildx gha cache.
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential clang lld gdb bison flex perl ccache \
        python3 python3-pip python3-dev python3-venv \
        libxml2-dev zlib1g-dev ca-certificates wget xz-utils pkg-config \
        doxygen graphviz xdg-utils libdw-dev \
    && rm -rf /var/lib/apt/lists/*

# Python analysis stack (used by scripts/analyze.py) plus scipy/posix_ipc, which
# OMNeT++'s `configure` hard-requires even for a headless `make base` build
# (it checks for the IDE's Python deps unconditionally).
RUN pip3 install --no-cache-dir pandas numpy matplotlib scipy posix_ipc pyyaml

SHELL ["/bin/bash", "-c"]

# ---------------------------------------------------------------------------
# OMNeT++ 6.4.0 (headless: no Qtenv, no OSG). `make base` skips the samples.
# ---------------------------------------------------------------------------
ENV OMNETPP_ROOT=/opt/omnetpp
RUN wget -q https://github.com/omnetpp/omnetpp/releases/download/omnetpp-6.4.0/omnetpp-6.4.0-linux-x86_64.tgz -O /tmp/omnetpp.tgz \
    && mkdir -p /opt && tar xzf /tmp/omnetpp.tgz -C /opt \
    && mv /opt/omnetpp-6.4.0 "$OMNETPP_ROOT" && rm /tmp/omnetpp.tgz
ENV PATH=$OMNETPP_ROOT/bin:$PATH
# 6.4.0's configure checks for IDE-related Python modules (e.g. ipython>=7.0.0)
# unconditionally, even with WITH_QTENV=no -- "configure: error: Install the
# missing Python modules and restart the configure script." (6.0.3 did not
# check for these). Install from OMNeT++'s own bundled requirements file,
# already present after extracting the archive above, before configuring.
RUN pip3 install --no-cache-dir -r "$OMNETPP_ROOT/python/requirements.txt"
RUN cd "$OMNETPP_ROOT" \
    && source setenv \
    && ./configure WITH_QTENV=no WITH_OSG=no WITH_OSGEARTH=no \
    && make -j"$(nproc)" MODE=release base

# ---------------------------------------------------------------------------
# INET 4.7.0 (release shared library libINET.so).
# ---------------------------------------------------------------------------
ENV INET_ROOT=/opt/inet4.7
RUN wget -q https://github.com/inet-framework/inet/releases/download/v4.7.0/inet-4.7.0-src.tgz -O /tmp/inet.tgz \
    && tar xzf /tmp/inet.tgz -C /opt && rm /tmp/inet.tgz
RUN cd "$INET_ROOT" \
    && source "$OMNETPP_ROOT/setenv" \
    && source setenv \
    && make makefiles \
    && make -j"$(nproc)" MODE=release

# Runtime env so `opp_run` finds the kernel + INET lib + NED files.
ENV LD_LIBRARY_PATH=$INET_ROOT/src:$OMNETPP_ROOT/lib
ENV NEDPATH=$INET_ROOT/src
WORKDIR /work
