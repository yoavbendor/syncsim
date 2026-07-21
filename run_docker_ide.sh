#!/bin/bash
# Launches the OMNeT++ IDE (syncsim:ide image) with X11 forwarding, without
# having to remember/retype the docker flags every time.
#
# Usage:
#   ./run_docker_ide.sh              # opens simulations/nominal.ini (M2:
#                                     # multi-hop bridges, sync-error growth
#                                     # vs hop count -- the flagship scenario)
#   ./run_docker_ide.sh <path/to.ini> # open a different scenario instead
#
# Files under this repo are bind-mounted straight into the container (no
# copy, no overlay) -- edits you make in the IDE write directly to your
# working tree on the host, so `git status`/`git diff`/commit work normally
# once you close the IDE.
#
# First run only: the IDE workspace starts empty, so use
# File -> Open Projects from File System... and add both:
#   /work          (this repo)
#   /opt/inet4.7   (INET, baked into the image)
# The workspace is persisted on the host (see SYNCSIM_IDE_WORKSPACE below),
# so this is a one-time step, not a per-run one.
set -e

cd "$(dirname "$0")"

IMAGE="${SYNCSIM_IMAGE:-syncsim:ide}"
SIM_FILE="${1:-simulations/nominal.ini}"
WORKSPACE_DIR="${SYNCSIM_IDE_WORKSPACE:-$HOME/.syncsim-ide-workspace}"

if [ ! -f "$SIM_FILE" ]; then
    echo "error: $SIM_FILE not found (relative to repo root)" >&2
    exit 1
fi

if [ -z "$DISPLAY" ]; then
    echo "error: \$DISPLAY not set -- run this from an X-forwarded session (ssh -X / MobaXterm)." >&2
    exit 1
fi

if ! command -v xauth >/dev/null 2>&1; then
    echo "error: xauth not found on host (needed to forward your X11 auth cookie)." >&2
    exit 1
fi

XAUTH_FILE="$(mktemp /tmp/syncsim-xauth.XXXXXX)"
trap 'rm -f "$XAUTH_FILE"' EXIT
xauth extract "$XAUTH_FILE" "$DISPLAY" 2>/dev/null
chmod 644 "$XAUTH_FILE"

mkdir -p "$WORKSPACE_DIR"

echo ">> Opening OMNeT++ IDE on $SIM_FILE (host display $DISPLAY)"
echo ">> IDE workspace persisted at: $WORKSPACE_DIR"

exec docker run --rm -it \
    --network host \
    -e X11_FORWARD=1 \
    -e DISPLAY="$DISPLAY" \
    -e XAUTHORITY=/root/.Xauthority \
    -v "$XAUTH_FILE:/root/.Xauthority:ro" \
    -v "$WORKSPACE_DIR:/root/.eclipse-workspace" \
    -v "$PWD:/work" \
    "$IMAGE" \
    omnetpp -data /root/.eclipse-workspace "/work/$SIM_FILE"
