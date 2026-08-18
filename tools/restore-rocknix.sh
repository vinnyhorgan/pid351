#!/bin/sh
# Put ROCKNIX's boot config back. The rollback, in one command.
set -e
CARD=/media/dvh/ROCKNIX
[ -f "$CARD/extlinux/extlinux.conf.rocknix" ] || {
    echo "no backup at $CARD/extlinux/extlinux.conf.rocknix"; exit 1; }
cp "$CARD/extlinux/extlinux.conf.rocknix" "$CARD/extlinux/extlinux.conf"
sync
echo "ROCKNIX boot config restored. Our files under /pid351 are left alone."
