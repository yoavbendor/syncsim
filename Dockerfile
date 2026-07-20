# OMNeT++ + INET simulator image for the gPTP/TSN sync sandbox, in three
# build targets sharing one Dockerfile:
#
#   headless (default) -- Cmdenv only, no Qt/X11 in the image at all. What
#                          CI builds (see .github/workflows/ci.yml's explicit
#                          `target: headless`) and what every scripts/run.sh
#                          invocation uses. Byte-for-byte the same recipe
#                          this Dockerfile has always used, on Ubuntu 22.04.
#   gui                -- adds Qtenv (WITH_QTENV=yes) plus a self-contained
#                          Xvfb/x11vnc/noVNC virtual desktop, so a running
#                          simulation is watchable from a browser tab with
#                          zero host-side X11 forwarding or VNC client setup.
#   ide                -- adds the full OMNeT++ IDE (Eclipse-based NED
#                          editor/diagrammer) on top of `gui`, for learning/
#                          editing .ned files interactively rather than just
#                          watching a run.
#
# CI never builds `gui`/`ide` -- they cost nothing in CI build time or image
# size. Build them explicitly when wanted:
#   docker build --target gui -t syncsim:gui .
#   docker build --target ide -t syncsim:ide .
#
# Pinned versions (INET 4.7.x requires OMNeT++ 6.4.x):
#   OMNeT++ 6.4.0, INET 4.7.0 -> ships the Gptp + TSN shaper + clock/oscillator
#   modules this project uses, under INET's post-4.6 reimplementation of both
#   (see README's "Why pinned to..." section for the migration history).
#
# `gui`/`ide` are on Ubuntu 24.04, not 22.04 like `headless` -- confirmed
# empirically: 22.04's `qt6-base-dev` ships Qt 6.2.4, and OMNeT++ 6.4.0's
# `configure` outright fails its Qt6 compile-check against it ("Cannot build
# Qt apps") under 22.04's clang-14, even though 6.2.4 nominally satisfies the
# ">=6.2" version requirement -- some combination of that specific Qt build
# and toolchain doesn't work, and config.log's underlying compiler error
# wasn't worth chasing once 24.04's Qt 6.4.2 was confirmed to just work
# (verified end-to-end: configure succeeds, `make base` links a Qtenv-capable
# opp_run, `ldd` shows it against Qt6). The OMNeT++ project's own `opp_env`
# package manager (Nix-based) was also tried and does solve this properly --
# its Nix expression pins a matched-known-good Qt6 -- but its flake-based
# dependency resolution shells out to the GitHub API at install time
# (`api.github.com/repos/numtide/flake-utils/...`), which is a real
# rate-limiting risk for repeated `docker build` in CI, not just a one-off
# sandbox fluke, so the smaller, already-proven Ubuntu-version bump was
# chosen over adopting Nix here.
# The build is heavy (~20-30 min first time for `headless`; `gui`/`ide` add
# more on top, plus their own ~2 min apt install). CI caches `headless` via
# buildx gha cache.

# =============================================================================
# Stage: base-deps -- apt/pip layer + OMNeT++/INET source for `headless`.
# Ubuntu 22.04 -- unchanged from the original single-target Dockerfile.
# =============================================================================
FROM ubuntu:22.04 AS base-deps

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

# Extraction is name-agnostic: 4.5.4's archive top-level directory happened to
# match the "inetX.Y" guess, but 4.7.0's does not ("No such file or
# directory" -- confirmed empirically in CI), and a major feature release is
# exactly where that convention was most likely to change. Extract into a
# scratch directory and move whatever single top-level directory tar
# produced, instead of assuming its name.
ENV INET_ROOT=/opt/inet4.7
RUN mkdir -p /opt/inet_extract && wget -q https://github.com/inet-framework/inet/releases/download/v4.7.0/inet-4.7.0-src.tgz -O /tmp/inet.tgz \
    && tar xzf /tmp/inet.tgz -C /opt/inet_extract \
    && mv /opt/inet_extract/*/ "$INET_ROOT" \
    && rm -rf /opt/inet_extract /tmp/inet.tgz

# =============================================================================
# Stage: headless (default target) -- what CI builds and every scripts/run.sh
# invocation uses. No Qtenv, no OSG, no X11 anywhere in this image.
# =============================================================================
FROM base-deps AS headless

RUN cd "$OMNETPP_ROOT" \
    && source setenv \
    && ./configure WITH_QTENV=no WITH_OSG=no WITH_OSGEARTH=no \
    && make -j"$(nproc)" MODE=release base

RUN cd "$INET_ROOT" \
    && source "$OMNETPP_ROOT/setenv" \
    && source setenv \
    && make makefiles \
    && make -j"$(nproc)" MODE=release

# Runtime env so `opp_run` finds the kernel + INET lib + NED files.
ENV LD_LIBRARY_PATH=$INET_ROOT/src:$OMNETPP_ROOT/lib
ENV NEDPATH=$INET_ROOT/src
WORKDIR /work

# =============================================================================
# Stage: gui-deps -- apt/pip layer + OMNeT++/INET source for `gui`/`ide`.
# Ubuntu 24.04, not 22.04 -- see the top-of-file note on the Qt6 version
# incompatibility this sidesteps. Duplicates base-deps' download/apt steps
# rather than extending it, since the two stages are now on different base
# images; the extra download cost is small next to the compile time either
# way, and correctness (an image that actually builds Qtenv) matters more
# than reusing a shared layer here.
# =============================================================================
FROM ubuntu:24.04 AS gui-deps

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential clang lld gdb bison flex perl ccache \
        python3 python3-pip python3-dev python3-venv \
        libxml2-dev zlib1g-dev ca-certificates wget xz-utils pkg-config \
        doxygen graphviz xdg-utils libdw-dev \
        qt6-base-dev qt6-base-dev-tools libgl1-mesa-dev libglu1-mesa-dev \
        xvfb x11vnc novnc websockify fluxbox xterm x11-utils \
    && rm -rf /var/lib/apt/lists/*

# --ignore-installed: novnc pulls in apt's python3-numpy (a debian-packaged
# dist-packages install with no RECORD file), which plain pip install then
# fails to uninstall ("RECORD file not found") when it tries to upgrade it --
# confirmed empirically on this image. --ignore-installed skips that
# uninstall step and just shadows it with pip's own copy.
RUN pip3 install --no-cache-dir --break-system-packages --ignore-installed \
        pandas numpy matplotlib scipy posix_ipc pyyaml

SHELL ["/bin/bash", "-c"]

ENV OMNETPP_ROOT=/opt/omnetpp
RUN wget -q https://github.com/omnetpp/omnetpp/releases/download/omnetpp-6.4.0/omnetpp-6.4.0-linux-x86_64.tgz -O /tmp/omnetpp.tgz \
    && mkdir -p /opt && tar xzf /tmp/omnetpp.tgz -C /opt \
    && mv /opt/omnetpp-6.4.0 "$OMNETPP_ROOT" && rm /tmp/omnetpp.tgz
ENV PATH=$OMNETPP_ROOT/bin:$PATH
RUN pip3 install --no-cache-dir --break-system-packages --ignore-installed -r "$OMNETPP_ROOT/python/requirements.txt"

ENV INET_ROOT=/opt/inet4.7
RUN mkdir -p /opt/inet_extract && wget -q https://github.com/inet-framework/inet/releases/download/v4.7.0/inet-4.7.0-src.tgz -O /tmp/inet.tgz \
    && tar xzf /tmp/inet.tgz -C /opt/inet_extract \
    && mv /opt/inet_extract/*/ "$INET_ROOT" \
    && rm -rf /opt/inet_extract /tmp/inet.tgz

# =============================================================================
# Stage: gui -- Qtenv (WITH_QTENV=yes) + a throwaway Xvfb/x11vnc/noVNC desktop.
# Local/interactive use only; never built by CI.
# =============================================================================
FROM gui-deps AS gui

# Same `make base` recipe as the headless stage, not the full `make`: with
# WITH_QTENV=yes, `base`'s target list picks up `qtenv` automatically (see
# OMNeT++'s top-level Makefile: `ifeq "$(WITH_QTENV)" "yes" ... BASE+= qtenv`),
# so `make base` alone already links a Qtenv-capable opp_run -- confirmed
# empirically by building both ways: plain `make` (no target) additionally
# compiles all 20 bundled samples (osg-earth, petrinets, tictoc, ...) this
# image has no use for and gains nothing from, while `make base` here costs
# the same incremental time as the headless build's `make base` plus Qtenv
# itself (~1 more compile unit, `layout`).
RUN cd "$OMNETPP_ROOT" \
    && source setenv \
    && ./configure WITH_QTENV=yes WITH_OSG=no WITH_OSGEARTH=no \
    && make -j"$(nproc)" MODE=release base

RUN cd "$INET_ROOT" \
    && source "$OMNETPP_ROOT/setenv" \
    && source setenv \
    && make makefiles \
    && make -j"$(nproc)" MODE=release

ENV LD_LIBRARY_PATH=$INET_ROOT/src:$OMNETPP_ROOT/lib
ENV NEDPATH=$INET_ROOT/src

COPY docker/desktop-entrypoint.sh /usr/local/bin/desktop-entrypoint.sh
RUN chmod +x /usr/local/bin/desktop-entrypoint.sh

# Matches the Xvfb display the entrypoint starts. AUTOSTART_APP is unset here
# (plain terminal only); the `ide` stage below overrides it.
ENV DISPLAY=:99
ENV AUTOSTART_APP=

EXPOSE 6080
WORKDIR /work
ENTRYPOINT ["/usr/local/bin/desktop-entrypoint.sh"]
CMD ["bash"]

# =============================================================================
# Stage: ide -- the full OMNeT++ IDE (Eclipse-based NED editor/diagrammer) on
# top of `gui`'s Qtenv + virtual desktop, auto-started in that desktop.
# =============================================================================
FROM gui AS ide

# The IDE's Eclipse RCP bundle should carry its own embedded JRE (standard
# for OMNeT++'s prebuilt IDE downloads), but this Dockerfile isn't itself
# CI-exercised for this target -- a headless system JRE is cheap insurance
# against that assumption being wrong for this specific release, rather than
# a whole broken tier discovered only when someone actually tries it.
RUN apt-get update && apt-get install -y --no-install-recommends \
        default-jre-headless \
    && rm -rf /var/lib/apt/lists/*

# The generic Linux release tarball extracted in gui-deps ships a prebuilt
# IDE (Eclipse RCP with its own bundled JRE) at $OMNETPP_ROOT/ide -- building
# it from source instead means `make ide`, which shells out to
# releng/build-omnetpp-ide-linux (a Maven/Tycho build against Eclipse's p2
# update sites): heavy, network-fragile, and not something this project's
# CI-verified-assumptions style should gamble on for a Dockerfile that isn't
# CI-exercised itself. Fail loudly here rather than silently shipping a
# broken "ide" tier if that assumption turns out wrong for this release.
RUN test -d "$OMNETPP_ROOT/ide" || { \
        echo "ERROR: \$OMNETPP_ROOT/ide not found in the generic release tarball."; \
        echo "Fallback: build it from source with 'make ide' (needs a JDK + Maven"; \
        echo "+ network access to Eclipse's p2 update sites) inside the omnetpp"; \
        echo "source tree at \$OMNETPP_ROOT, then retry this stage."; \
        exit 1; \
    }

ENV AUTOSTART_APP=omnetpp
