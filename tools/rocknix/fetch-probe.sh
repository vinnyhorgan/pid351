#!/bin/bash
#
# Mounts the ROCKNIX storage partition, reports whether the probe is
# installed and whether it ran, copies any reports into the repo, and
# unmounts. Run as root.

set -u
[ "$(id -u)" = 0 ] || { echo "must run as root"; exit 1; }

DEST=/home/dvh/pid351/docs
OWNER=dvh

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
echo

echo "--- is the probe installed? ---"
ls -l "$MNT/pid351/" 2>&1
echo
echo "--- autostart.sh ---"
if [ -e "$MNT/.config/autostart.sh" ]; then
    ls -l "$MNT/.config/autostart.sh"
    echo "contents:"; sed 's/^/    /' "$MNT/.config/autostart.sh"
else
    echo "NOT PRESENT - the installer never ran"
fi
echo
echo "--- ports/tools entries ---"
ls -l "$MNT/roms/ports" "$MNT/roms/tools" 2>&1 | head -20
echo
echo "--- roms tree (top level) ---"
ls "$MNT/roms" 2>&1 | head -30
echo

n=$(ls "$MNT"/pid351/probe-*.txt 2>/dev/null | wc -l)
if [ "$n" -gt 0 ]; then
    cp "$MNT"/pid351/probe-*.txt "$DEST"/ && chown "$OWNER": "$DEST"/probe-*.txt
    echo "copied $n report(s) into $DEST"
else
    echo "no probe reports found - it has not run yet"
fi

sync
if [ "$OURS" = 1 ]; then
    umount "$MNT" && echo "unmounted, card safe to remove"
else
    echo "NOTE: $MNT was already mounted; unmount before pulling the card"
fi
