#!/bin/bash
# Entrypoint for the `gui`/`ide` Docker targets: starts a throwaway virtual
# desktop (Xvfb + fluxbox + x11vnc + noVNC) inside the container so Qtenv
# (and, in the `ide` image, the OMNeT++ IDE) are watchable/usable from any
# browser at http://localhost:6080/vnc.html -- no host-side X11 forwarding or
# VNC client needed. The `headless` image never includes this script.
set -e

DISPLAY_NUM="${DISPLAY#:}"
GEOMETRY="${VNC_GEOMETRY:-1600x900x24}"

Xvfb ":$DISPLAY_NUM" -screen 0 "$GEOMETRY" -nolisten tcp &
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
