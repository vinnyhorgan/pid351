#!/bin/bash
#
# Installs the probe onto a ROCKNIX card from the laptop. Run as root.
#
# Finds the STORAGE partition by label rather than by device node, because
# the card lands on a different node depending on which port of the dock
# it went into.

set -u
[ "$(id -u)" = 0 ] || { echo "must run as root"; exit 1; }

HERE=$(cd "$(dirname "$0")" && pwd)

DEV=$(blkid -L STORAGE 2>/dev/null)
[ -n "$DEV" ] || { echo "no partition labelled STORAGE found; is the card in?"; exit 1; }
echo "STORAGE partition: $DEV"

MNT=$(findmnt -n -o TARGET --source "$DEV" | head -1)
OURS=0
if [ -z "$MNT" ]; then
    MNT=/mnt/rocknix-storage
    mkdir -p "$MNT"
    mount "$DEV" "$MNT" || { echo "mount failed"; exit 1; }
    OURS=1
    echo "mounted at $MNT"
else
    echo "already mounted at $MNT"
fi

fail() { echo "ERROR: $*"; [ "$OURS" = 1 ] && umount "$MNT"; exit 1; }

[ -d "$MNT/.config" ] || fail "$MNT/.config missing - is this really the ROCKNIX storage partition?"

mkdir -p "$MNT/pid351"
install -m 755 "$HERE/probe.sh" "$MNT/pid351/probe.sh" || fail "copy probe.sh"

# ROCKNIX walks /storage/.config/autostart as a DIRECTORY (see
# /usr/bin/autostart in the SYSTEM squashfs). LibreELEC's single
# autostart.sh file is not read at all on this distro.
rm -f "$MNT/.config/autostart.sh"
mkdir -p "$MNT/.config/autostart"
install -m 755 "$HERE/autostart-entry.sh" "$MNT/.config/autostart/pid351-probe" \
    || fail "copy autostart entry"

# Same script as a Ports entry, so it can be re-run from the menu without
# a reboot once we start iterating on device tree changes.
mkdir -p "$MNT/roms/ports"
for P in "$MNT/roms/ports" "$MNT/roms/tools"; do
    [ -d "$P" ] || continue
    cat > "$P/pid351-probe.sh" <<'INNER'
#!/bin/sh
/storage/pid351/probe.sh
INNER
    chmod 755 "$P/pid351-probe.sh"
    echo "ports entry: $P/pid351-probe.sh"
done

echo
echo "installed:"
ls -l "$MNT/pid351/" "$MNT/.config/autostart/"
sync

if [ "$OURS" = 1 ]; then
    umount "$MNT" && echo "unmounted, card safe to remove"
else
    echo "NOTE: $MNT was already mounted; unmount it before pulling the card"
fi
