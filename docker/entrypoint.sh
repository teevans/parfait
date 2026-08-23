#!/usr/bin/env bash
# Brings up a throwaway PipeWire graph with virtual meeting devices, then execs "$@".
set -euo pipefail

export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/tmp/xdg}"
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"

# PipeWire and WirePlumber both want a session bus; re-exec ourselves inside one.
if [ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ]; then
    exec dbus-run-session -- "$0" "$@"
fi

LOGDIR=/tmp/pw-logs
mkdir -p "$LOGDIR"

start() {
    local name=$1
    "$name" >"$LOGDIR/$name.log" 2>&1 &
    echo "$name started (pid $!)"
}

start pipewire
start wireplumber
start pipewire-pulse

# The pulse socket appears a moment after the daemon does.
for _ in $(seq 1 100); do
    if pactl info >/dev/null 2>&1; then break; fi
    sleep 0.2
done
if ! pactl info >/dev/null 2>&1; then
    echo "error: pipewire-pulse never came up" >&2
    tail -n 40 "$LOGDIR"/*.log >&2 || true
    exit 1
fi

# "Them" = whatever is played into the meeting sink; parfait taps its monitor.
pactl load-module module-null-sink \
    sink_name=meeting \
    sink_properties=device.description=Meeting >/dev/null
pactl set-default-sink meeting

# "Me" = whatever is played into micfeed, re-exposed as a real-looking source.
pactl load-module module-null-sink \
    sink_name=micfeed \
    sink_properties=device.description=MicFeed >/dev/null
pactl load-module module-remap-source \
    master=micfeed.monitor \
    source_name=vmic \
    source_properties=device.description=VirtualMic >/dev/null
pactl set-default-source vmic

echo "--- sinks ---";   pactl list short sinks
echo "--- sources ---"; pactl list short sources
echo "--- defaults ---"
pactl info | grep -E '^Default (Sink|Source):'

pactl list short sinks   | grep -q -w meeting || { echo "error: meeting sink missing" >&2; exit 1; }
pactl list short sources | grep -q -w vmic    || { echo "error: vmic source missing" >&2; exit 1; }

exec "$@"
