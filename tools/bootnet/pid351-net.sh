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

# Gadget mode turns the handheld into a USB device so it appears as an
# ethernet adapter to the laptop. It is gated on a marker file living on the
# ROMs partition, so the laptop can switch it on and off by creating or
# deleting one file - no menus, no shell on the device.
#
# It is gated rather than unconditional because the internal gamepad is a USB
# device on this same dwc2 controller, which holds one role at a time. Gadget
# mode may well cost the controls until reboot.
setup_gadget() {
    [ -f /roms/pid351/gadget-mode ] || return
    lsmod | grep -q "^g_ether" && return

    echo "--- $(date) gadget-mode marker present, loading g_ether ---"
    modprobe libcomposite 2>&1
    # Pin both MACs so the interface name on the laptop stays stable across
    # reboots instead of changing every time.
    modprobe g_ether dev_addr=02:51:03:51:00:02 host_addr=02:51:03:51:00:01 2>&1
    sleep 2
    echo "udc state: $(cat /sys/class/udc/ff300000.usb/state 2>/dev/null)"
    echo "interfaces now: $(ls /sys/class/net/ | tr '\n' ' ')"
}

systemctl start ssh.service
echo "sshd: $(systemctl is-active ssh.service)"
setup_gadget

while true; do
    setup_gadget   # retried, in case /roms mounted after we first looked

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
