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

# The ROMs partition is visible from the laptop, so logging there means the log
# can be read without a shell on the device.
for d in /roms/pid351 /opt/system/Tools /var/log; do
    [ -d "$(dirname "$d")" ] && { mkdir -p "$d" 2>/dev/null; LOGDIR="$d"; break; }
done
LOG="$LOGDIR/pid351-net.log"
IPFILE="$LOGDIR/pid351-ip.txt"

exec >> "$LOG" 2>&1
echo "=== pid351-net starting, pid $$, log $LOG ==="

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

        timeout 20 dhclient -1 "$IF" 2>&1 | tail -3

        if ! ip -4 addr show "$IF" | grep -q "inet "; then
            # No DHCP server - direct cable to the laptop, most likely.
            echo "no lease on $IF, falling back to static"
            ip addr add 192.168.7.2/24 dev "$IF" 2>&1
        fi

        ip -4 addr show "$IF" | awk '/inet /{print $2}'
    done

    # Record every current address where the laptop can read it off the card.
    {
        echo "updated: $(date)"
        ip -4 addr | awk '/inet /{print $NF ": " $2}'
        echo "sshd: $(systemctl is-active ssh.service)"
    } > "$IPFILE"

    systemctl is-active --quiet ssh.service || systemctl start ssh.service
    sleep 10
done
