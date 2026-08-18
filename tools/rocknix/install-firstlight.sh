#!/bin/bash
#
# Puts the device build of pid351 on a ROCKNIX card and arranges for it to
# run at boot, before EmulationStation. Run as root.

set -u
[ "$(id -u)" = 0 ] || { echo "must run as root"; exit 1; }

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
BIN="$REPO/build/device/pid351"

[ -x "$BIN" ] || { echo "no device build - run 'make device' first"; exit 1; }
file "$BIN" | grep -q 'ARM aarch64' || { echo "$BIN is not an aarch64 binary"; exit 1; }

DEV=$(blkid -L STORAGE 2>/dev/null)
[ -n "$DEV" ] || { echo "no partition labelled STORAGE; is the card in?"; exit 1; }

MNT=$(findmnt -n -o TARGET --source "$DEV" | head -1)
OURS=0
if [ -z "$MNT" ]; then
    MNT=/mnt/rocknix-storage
    mkdir -p "$MNT"
    mount "$DEV" "$MNT" || { echo "mount failed"; exit 1; }
    OURS=1
fi
echo "STORAGE ($DEV) at $MNT"

mkdir -p "$MNT/pid351" "$MNT/.config/autostart"
install -m 755 "$BIN"                  "$MNT/pid351/pid351"
install -m 755 "$HERE/first-light.sh"  "$MNT/pid351/first-light.sh"
# Sorts before pid351-probe, so the panel test runs before the probe starts
# poking at USB.
install -m 755 "$HERE/first-light.sh"  "$MNT/.config/autostart/pid351-first-light"

# A stale log would be indistinguishable from a run that produced no output.
rm -f "$MNT/pid351/first-light.log"

echo
ls -l "$MNT/pid351/" "$MNT/.config/autostart/"
sync

if [ "$OURS" = 1 ]; then
    umount "$MNT" && echo "unmounted, card safe to remove"
else
    echo "NOTE: $MNT was already mounted; unmount before pulling the card"
fi
