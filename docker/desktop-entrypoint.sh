#!/bin/bash
# Entrypoint for the `gui`/`ide` Docker targets. Two modes:
#
# noVNC (default) -- starts a throwaway virtual desktop (Xvfb + fluxbox +
#   x11vnc + noVNC) inside the container so Qtenv (and, in the `ide` image,
#   the OMNeT++ IDE) are watchable/usable from any browser at
#   http://localhost:6080/vnc.html -- no host-side setup needed, but every
#   frame is VNC-encoded and round-tripped over the network, which is
#   noticeably laggier for an interactive IDE than a local window.
#
# X11 forwarding (X11_FORWARD=1) -- skips Xvfb/fluxbox/x11vnc/websockify
#   entirely and draws straight to the host's own X server via a forwarded
#   DISPLAY + X11 socket, giving native window responsiveness. Requires a
#   host X server reachable from the container:
#
#     # Linux host:
#     xhost +local:docker
#     docker run --rm -e X11_FORWARD=1 -e DISPLAY=$DISPLAY \
#         -v /tmp/.X11-unix:/tmp/.X11-unix:ro -v "$PWD:/work" syncsim:ide
#
#     # macOS host: install XQuartz, enable "Allow connections from network
#     # clients" in its preferences, then `xhost +localhost` and
#     # DISPLAY=host.docker.internal:0 in place of the two lines above.
#
#     # Windows host (WSL2 with WSLg): DISPLAY is already set by WSLg: pass
#     # -e DISPLAY=$DISPLAY -v /tmp/.X11-unix:/tmp/.X11-unix:ro, no xhost step.
#
# The `headless` image never includes this script.
set -e

if [ "$X11_FORWARD" = "1" ]; then
    # DISPLAY is "[host]:display[.screen]" (e.g. "localhost:110.0" under SSH
    # X11 forwarding) but the socket file is just "X<display>", no screen
    # suffix -- strip both the host prefix and the ".screen" part.
    DISPLAY_NUM="${DISPLAY#*:}"
    DISPLAY_NUM="${DISPLAY_NUM%%.*}"
    if [ -z "$DISPLAY" ] || [ ! -S "/tmp/.X11-unix/X${DISPLAY_NUM}" ]; then
        echo ">> X11_FORWARD=1 but no usable \$DISPLAY / /tmp/.X11-unix socket found." >&2
        echo ">> Pass -e DISPLAY=\$DISPLAY -v /tmp/.X11-unix:/tmp/.X11-unix:ro (see" >&2
        echo ">> this script's header comment for the host-specific setup)." >&2
        exit 1
    fi
    echo ">> X11 forwarding: drawing to host display $DISPLAY"

    if [ -n "$AUTOSTART_APP" ]; then
        "$OMNETPP_ROOT/bin/$AUTOSTART_APP" &
    fi

    if [ "$#" -gt 0 ] && [ "$1" != "bash" ]; then
        exec "$@"
    fi
    exec bash
fi

DISPLAY=":99"
export DISPLAY
GEOMETRY="${VNC_GEOMETRY:-1600x900x24}"

Xvfb "$DISPLAY" -screen 0 "$GEOMETRY" -nolisten tcp &
XVFB_PID=$!

for _ in $(seq 1 50); do
    xdpyinfo -display "$DISPLAY" >/dev/null 2>&1 && break
    sleep 0.2
done

fluxbox >/tmp/fluxbox.log 2>&1 &
x11vnc -display "$DISPLAY" -forever -shared -nopw -rfbport 5900 -quiet >/tmp/x11vnc.log 2>&1 &
websockify --web=/usr/share/novnc 6080 localhost:5900 >/tmp/websockify.log 2>&1 &

echo ">> noVNC ready: open http://localhost:6080/vnc.html in a browser."
echo ">> (or point a VNC client at localhost:5900 if that port is published too)"
echo ">> Laggy? Re-run with X11_FORWARD=1 instead (see this script's header)."

xterm -geometry 100x30+20+20 &

# AUTOSTART_APP is unset in the `gui` image (just a terminal) and set to
# "omnetpp" in the `ide` image (launches the IDE alongside the terminal).
if [ -n "$AUTOSTART_APP" ]; then
    "$OMNETPP_ROOT/bin/$AUTOSTART_APP" &
fi

# A real command (e.g. `opp_run -u Qtenv ...`) replaces the idle wait below
# and runs in the foreground, inside the desktop that's already up.
if [ "$#" -gt 0 ] && [ "$1" != "bash" ]; then
    exec "$@"
fi

# Default CMD is "bash": keep the container alive for as long as the virtual
# desktop is up, rather than exec-ing an interactive shell that would exit
# (and take the container down with it) the moment its TTY disconnects.
wait "$XVFB_PID"
