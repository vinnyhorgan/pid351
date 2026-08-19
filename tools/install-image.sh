#!/bin/sh
# Install the pid351 image onto the card, next to ROCKNIX rather than over it.
#
# Nothing here needs root: the card is mounted by udisks with uid=1000, and the
# initramfs got its root ownership from a gen_init_cpio file list at build time.
#
# ROCKNIX's own files are never touched. The only thing that changes is
# /extlinux/extlinux.conf, which is backed up once and restored by
# tools/restore-rocknix.sh. That is the entire rollback: one file, from the
# laptop, with the card in a reader. On a machine with one SD card, no serial
# port and no maskrom button, keeping that path intact is not sentimentality
# about ROCKNIX - it is the only way back if our kernel does not come up.
set -e

CARD=/media/dvh/ROCKNIX
HERE=$(cd "$(dirname "$0")" && pwd)
OUT="$HERE/../image/out"

[ -d "$CARD" ] || { echo "card not mounted at $CARD"; exit 1; }
[ -f "$OUT/Image" ] || { echo "no image built - run image/build.sh"; exit 1; }

echo "==> backing up ROCKNIX's boot config"
if [ -f "$CARD/extlinux/extlinux.conf" ] && \
   [ ! -f "$CARD/extlinux/extlinux.conf.rocknix" ]; then
    cp "$CARD/extlinux/extlinux.conf" "$CARD/extlinux/extlinux.conf.rocknix"
    echo "    saved extlinux.conf.rocknix"
else
    echo "    backup already present, left alone"
fi

echo "==> installing"
mkdir -p "$CARD/pid351"
cp "$OUT/Image"                          "$CARD/pid351/Image"
cp "$OUT/pid351-rg351p.dtb"              "$CARD/pid351/"
cp "$OUT/extlinux.conf"                  "$CARD/extlinux/extlinux.conf"

# Clear the logs so the next boot's are unambiguous - but archive them first.
# Deleting the only copy of a boot log to make room for the next one is a
# spectacularly bad trade, and it has already happened once.
for f in pid351-boot.log pid351-fail.log; do
    [ -f "$CARD/$f" ] || continue
    n=1
    while [ -e "$HERE/../docs/logs/$n-$f" ]; do n=$((n + 1)); done
    mkdir -p "$HERE/../docs/logs"
    mv "$CARD/$f" "$HERE/../docs/logs/$n-$f"
    echo "    archived $f -> docs/logs/$n-$f"
done

# ROMs, from roms/ in the tree. They live on the card rather than inside the
# initramfs so that changing the game is a file copy instead of a kernel
# rebuild. Savestates land beside them and are never touched here: the state
# is the only record this machine keeps of a game, so the installer must not
# be a thing that can eat one.
if [ -d "$HERE/../roms" ]; then
    mkdir -p "$CARD/pid351/roms"
    n=0
    for f in "$HERE/../roms"/*; do
        [ -f "$f" ] || continue
        cp "$f" "$CARD/pid351/roms/"
        n=$((n + 1))
    done
    echo "==> staged $n ROM(s)"
    ls -la "$CARD/pid351/roms"
fi

# The mainline dtb we shipped before our own board file existed.
rm -f "$CARD/pid351/rk3326-anbernic-rg351m.dtb"

sync
echo
echo "installed:"
ls -la "$CARD/pid351"
echo
echo "boot config now:"
cat "$CARD/extlinux/extlinux.conf"
echo
# Unmount, always, as the last act. The card gets pulled the moment this
# script finishes, and leaving that to whoever is holding it is how a FAT
# gets half-written. udisksctl first because udisks owns this mount; plain
# umount as a fallback for a mount made some other way.
echo
echo "==> unmounting"
DEV=$(findmnt -no SOURCE "$CARD" 2>/dev/null)
if [ -n "$DEV" ] && command -v udisksctl >/dev/null 2>&1; then
    udisksctl unmount -b "$DEV"
elif [ -n "$DEV" ]; then
    umount "$CARD"
fi

if findmnt -no SOURCE "$CARD" >/dev/null 2>&1; then
    echo "STILL MOUNTED - do not pull the card yet."
    exit 1
fi

echo
echo "safe to pull. put it in the console and power on."
echo "to go back to ROCKNIX: tools/restore-rocknix.sh"
