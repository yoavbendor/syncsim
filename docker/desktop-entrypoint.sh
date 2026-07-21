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
#   DISPLAY, giving native window responsiveness. Requires a host X server
#   reachable from the container:
#
#     # Linux host (local X server, unix socket):
#     xhost +local:docker
#     docker run --rm -e X11_FORWARD=1 -e DISPLAY=$DISPLAY \
#         -v /tmp/.X11-unix:/tmp/.X11-unix:ro -v "$PWD:/work" syncsim:ide
#
#     # Linux host over SSH X11 forwarding (ssh -X): sshd's X11 proxy is
#     # TCP-only on the host's loopback (DISPLAY like "localhost:110.0",
#     # no matching /tmp/.X11-unix socket), so the container needs to share
#     # the host's network namespace instead of a socket bind-mount. Rootless
#     # podman/docker also can't read your real ~/.Xauthority (owned by your
#     # host UID, mode 600) from inside the container's remapped UID, so
#     # extract just this display's cookie to a throwaway world-readable file:
#     xauth extract /tmp/x11.xauth $DISPLAY && chmod 644 /tmp/x11.xauth
#     docker run --rm --network host -e X11_FORWARD=1 -e DISPLAY=$DISPLAY \
#         -e XAUTHORITY=/root/.Xauthority -v /tmp/x11.xauth:/root/.Xauthority:ro \
#         -v "$PWD:/work" syncsim:ide
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
    # Ask libX11 (via xdpyinfo) whether DISPLAY is actually reachable,
    # rather than looking for a /tmp/.X11-unix socket file -- SSH X11
    # forwarding (DISPLAY like "localhost:110.0") is TCP-only and never
    # creates one, so a socket-existence check false-negatives on it.
    if [ -z "$DISPLAY" ] || ! xdpyinfo -display "$DISPLAY" >/dev/null 2>&1; then
        echo ">> X11_FORWARD=1 but \$DISPLAY ($DISPLAY) is not reachable." >&2
        echo ">> Local X server: pass -e DISPLAY=\$DISPLAY -v /tmp/.X11-unix:/tmp/.X11-unix:ro" >&2
        echo ">> SSH-forwarded X (DISPLAY=localhost:N.S): pass --network host instead of" >&2
        echo ">> the socket mount (see this script's header comment)." >&2
        exit 1
    fi
    echo ">> X11 forwarding: drawing to host display $DISPLAY"

    # Only autostart AUTOSTART_APP when nothing else was asked for -- an
    # explicit command (e.g. our own `omnetpp -data ... foo.ini`) already
    # launches the IDE itself, and running both at once fights over the
    # same Eclipse workspace lock and crashes both.
    if [ "$#" -gt 0 ] && [ "$1" != "bash" ]; then
        exec "$@"
    fi

    if [ -n "$AUTOSTART_APP" ]; then
        "$OMNETPP_ROOT/bin/$AUTOSTART_APP" &
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

# A real command (e.g. `opp_run -u Qtenv ...`) replaces the idle wait below
# and runs in the foreground, inside the desktop that's already up. Only
# autostart AUTOSTART_APP when nothing else was asked for -- an explicit
# command that itself launches the IDE would otherwise collide with it
# over the same Eclipse workspace lock.
if [ "$#" -gt 0 ] && [ "$1" != "bash" ]; then
    exec "$@"
fi

# AUTOSTART_APP is unset in the `gui` image (just a terminal) and set to
# "omnetpp" in the `ide` image (launches the IDE alongside the terminal).
if [ -n "$AUTOSTART_APP" ]; then
    "$OMNETPP_ROOT/bin/$AUTOSTART_APP" &
fi

# Default CMD is "bash": keep the container alive for as long as the virtual
# desktop is up, rather than exec-ing an interactive shell that would exit
# (and take the container down with it) the moment its TTY disconnects.
wait "$XVFB_PID"
