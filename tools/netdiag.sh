#!/bin/bash
# pid351 - USB ethernet diagnosis and forced bring-up
#
# ArkOS's own "Enable Remote Services" refuses to do anything unless a default
# gateway already exists. We do not need a gateway - we need an address the
# laptop can reach - so this bypasses that check entirely: it reports why the
# dongle is or is not working, brings the link up by whatever means available,
# and starts sshd regardless.

OUT="$(dirname "$0")/pid351-netdiag.txt"

sec() { echo; echo "===== $* ====="; }
run() { echo "--- \$ $* ---"; "$@" 2>&1 || echo "(failed or absent)"; }

{
sec "usb devices enumerated"
# lsusb is not installed on ArkOS, so read the bus out of sysfs instead.
for d in /sys/bus/usb/devices/*; do
    [ -r "$d/idVendor" ] || continue
    printf "%-12s %s:%s  %s %s\n" "$(basename "$d")" \
        "$(cat "$d/idVendor")" "$(cat "$d/idProduct")" \
        "$(cat "$d/manufacturer" 2>/dev/null)" "$(cat "$d/product" 2>/dev/null)"
done

sec "network interfaces"
for n in /sys/class/net/*; do
    [ -d "$n" ] || continue
    echo "$(basename "$n"): operstate=$(cat "$n/operstate" 2>/dev/null) carrier=$(cat "$n/carrier" 2>/dev/null) mac=$(cat "$n/address" 2>/dev/null)"
done
run ip addr
run ip route

sec "usb role"
run cat /sys/class/udc/ff300000.usb/state
for e in /sys/class/extcon/*; do
    [ -d "$e" ] && { echo "--- $e ---"; cat "$e/state" 2>/dev/null; }
done

sec "drivers"
run lsmod
run dmesg

sec "forced bring-up"
# Any interface that is not loopback gets brought up and asked for a lease.
for n in /sys/class/net/*; do
    IF=$(basename "$n")
    [ "$IF" = "lo" ] && continue
    echo "--- trying $IF ---"
    sudo ip link set "$IF" up 2>&1
    sleep 2
    echo "carrier after up: $(cat "$n/carrier" 2>/dev/null)"

    sudo dhclient -v "$IF" 2>&1 | tail -5 || sudo udhcpc -i "$IF" -n -q 2>&1 | tail -5

    ADDR=$(ip -4 addr show "$IF" | awk '/inet /{print $2}')
    if [ -z "$ADDR" ]; then
        # No DHCP server, or a direct cable to the laptop. Take a fixed address
        # so there is still something to ssh to.
        echo "no lease on $IF, assigning static 192.168.7.2/24"
        sudo ip addr add 192.168.7.2/24 dev "$IF" 2>&1
        ADDR="192.168.7.2/24 (static)"
    fi
    echo "$IF address: $ADDR"
done

sec "sshd"
sudo systemctl start ssh.service 2>&1
run systemctl is-active ssh.service

sec "RESULT"
ip -4 addr | awk '/inet /{print "  " $NF ": " $2}'

} > "$OUT" 2>&1

sync
clear
echo "=============================================="
echo " pid351 network diagnosis"
echo "=============================================="
echo
echo "Addresses now configured:"
ip -4 addr | awk '/inet /{print "   " $NF ": " $2}'
echo
echo "sshd: $(systemctl is-active ssh.service 2>/dev/null)"
echo
echo "Full report written to:"
echo "   $OUT"
echo
echo "Press any key or wait 60s..."
read -n 1 -t 60
