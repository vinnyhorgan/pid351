#!/bin/sh
#
# Runs pid351 on the panel, from /storage/.config/autostart/ .
#
# ROCKNIX's autostart walks that directory as root and only starts the UI
# afterwards, so nothing holds DRM master while this runs - which is exactly
# the window pid351 needs, and avoids having to stop EmulationStation.
#
# Those scripts are synchronous, so a hang here stalls the boot. Hence the
# hard time limit: worst case the console is 90 seconds late to its menu,
# rather than needing the card pulled and edited.

BIN=/storage/pid351/pid351
LOG=/storage/pid351/first-light.log

[ -x "$BIN" ] || exit 0

{
    echo "=== pid351 first light ==="
    uname -a
    echo "uptime: $(cat /proc/uptime)"
    echo
} > "$LOG" 2>&1

if command -v timeout >/dev/null 2>&1; then
    timeout -s TERM -k 5 90 "$BIN" >> "$LOG" 2>&1
    rc=$?
else
    "$BIN" >> "$LOG" 2>&1 &
    pid=$!
    ( sleep 90; kill -TERM "$pid" 2>/dev/null ) &
    wait "$pid"
    rc=$?
fi
echo "exit status: $rc" >> "$LOG"

# Mirror onto the vfat partition, which the laptop can read without root.
if mount -o remount,rw /flash 2>/dev/null; then
    cp "$LOG" /flash/ 2>/dev/null
    sync
    mount -o remount,ro /flash 2>/dev/null
fi
