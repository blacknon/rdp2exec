#!/usr/bin/env bash
set -euo pipefail

export DISPLAY="${DISPLAY:-:99}"
export VNC_PASSWORD="${VNC_PASSWORD:-testpass123}"

if ! xdpyinfo -display "$DISPLAY" >/dev/null 2>&1; then
  Xvfb "$DISPLAY" -screen 0 1280x900x24 -ac +extension RANDR >/tmp/xvfb.log 2>&1 &
  sleep 1
fi

if [[ "${ENABLE_VNC:-1}" == "1" ]]; then
  mkdir -p /root/.vnc
  x11vnc -storepasswd "$VNC_PASSWORD" /root/.vnc/passwd >/dev/null 2>&1
  pkill x11vnc >/dev/null 2>&1 || true
  x11vnc -display "$DISPLAY" -forever -rfbauth /root/.vnc/passwd -listen 0.0.0.0 -rfbport 5900 >/tmp/x11vnc.log 2>&1 &
  sleep 1
fi

exec "$@"
