#!/bin/bash
# pid351 - install the boot-time network service onto a mounted ArkOS rootfs.
#
#   sudo bash tools/install-bootnet.sh /mnt/arkos
#
# Additive and reversible: it adds two files and two symlinks and modifies
# nothing that ArkOS already ships. To undo, run with --uninstall.

set -e
R="${1:?usage: install-bootnet.sh <mounted-arkos-rootfs> [--uninstall]}"
HERE="$(cd "$(dirname "$0")" && pwd)"

[ -d "$R/etc/systemd/system" ] || { echo "error: $R does not look like an ArkOS rootfs"; exit 1; }
touch "$R/.pid351-write-test" 2>/dev/null || { echo "error: $R is mounted read-only - remount rw"; exit 1; }
rm -f "$R/.pid351-write-test"

WANTS="$R/etc/systemd/system/multi-user.target.wants"

if [ "$2" = "--uninstall" ]; then
    rm -f "$WANTS/pid351-net.service" "$WANTS/ssh.service"
    rm -f "$R/etc/systemd/system/pid351-net.service" "$R/usr/local/sbin/pid351-net.sh"
    echo "removed. ArkOS is back to how it shipped."
    exit 0
fi

install -D -m 0755 "$HERE/bootnet/pid351-net.sh"      "$R/usr/local/sbin/pid351-net.sh"
install -D -m 0644 "$HERE/bootnet/pid351-net.service" "$R/etc/systemd/system/pid351-net.service"

mkdir -p "$WANTS"
ln -sf ../pid351-net.service        "$WANTS/pid351-net.service"
ln -sf /lib/systemd/system/ssh.service "$WANTS/ssh.service"

sync
echo "installed:"
echo "  $R/usr/local/sbin/pid351-net.sh"
echo "  $R/etc/systemd/system/pid351-net.service"
echo "  $WANTS/pid351-net.service  -> enables it at boot"
echo "  $WANTS/ssh.service         -> makes sshd persistent"
echo
echo "On next boot the device will bring up any ethernet interface, fall back"
echo "to 192.168.7.2/24 without DHCP, start sshd, and write its address to"
echo "pid351-ip.txt on the ROMs partition."
