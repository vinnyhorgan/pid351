#!/bin/bash
# pid351 - bring up networking and ssh at boot, unconditionally
#
# Installed onto ArkOS's rootfs as a systemd service. Exists because ArkOS's
# own "Enable Remote Services" only starts sshd if a default route is present
# and does not survive a reboot, and because EmulationStation runs tools
# scripts without stdin so nothing interactive is reliable.
#
# Polls rather than reacting to udev, so a dongle plugged in at any point after
# boot still gets configured. Cheap enough at a 10 second interval.

# Always log somewhere that cannot fail. /roms is the ROMs partition and is
# only useful once actually mounted - writing there too early would put the
# file underneath the mountpoint, where it silently disappears.
LOG=/var/log/pid351-net.log
exec >> "$LOG" 2>&1
echo "=== pid351-net starting, pid $$ ==="

# Mirror onto the card when, and only when, the partition is really mounted,
# retried every pass so it appears as soon as it can be read from the laptop.
publish() {
    mountpoint -q /roms || return
    mkdir -p /roms/pid351 2>/dev/null || return
    {
        echo "updated: $(date)"
        ip -4 addr | awk '/inet /{print $NF ": " $2}'
        echo "sshd: $(systemctl is-active ssh.service)"
    } > /roms/pid351/pid351-ip.txt
    cp -f "$LOG" /roms/pid351/pid351-net.log 2>/dev/null
    sync
}

systemctl start ssh.service
echo "sshd: $(systemctl is-active ssh.service)"

while true; do
    for n in /sys/class/net/*; do
        [ -d "$n" ] || continue
        IF=$(basename "$n")
        [ "$IF" = "lo" ] && continue

        # Already has an address? Nothing to do.
        if ip -4 addr show "$IF" 2>/dev/null | grep -q "inet "; then
            continue
        fi

        echo "--- $(date) configuring $IF ---"
        ip link set "$IF" up
        sleep 3
        echo "carrier: $(cat "$n/carrier" 2>/dev/null)"

        if command -v dhclient >/dev/null; then
            timeout 20 dhclient -1 "$IF" 2>&1 | tail -3
        elif command -v udhcpc >/dev/null; then
            timeout 20 udhcpc -i "$IF" -n -q 2>&1 | tail -3
        else
            echo "no dhcp client found"
        fi

        if ! ip -4 addr show "$IF" | grep -q "inet "; then
            # No DHCP server - direct cable to the laptop, most likely.
            echo "no lease on $IF, falling back to static"
            ip addr add 192.168.7.2/24 dev "$IF" 2>&1
        fi

        ip -4 addr show "$IF" | awk '/inet /{print $2}'
    done

    publish

    systemctl is-active --quiet ssh.service || systemctl start ssh.service
    sleep 10
done
