#!/bin/bash
# pid351 - USB ethernet diagnosis
#
# EmulationStation runs tools scripts with no stdin, so every prompt must be a
# timed countdown rather than a keypress. Nothing in here may block on input.

OUT="$(dirname "$0")/pid351-netdiag.txt"
TMP=/tmp/pid351-diag
mkdir -p $TMP

sec() { echo; echo "===== $* ====="; }
run() { echo "--- \$ $* ---"; "$@" 2>&1 || echo "(failed or absent)"; }

countdown() {
    local n=$1 msg=$2
    while [ "$n" -gt 0 ]; do
        printf "\r   %s ... %2ds  " "$msg" "$n"
        sleep 1
        n=$((n - 1))
    done
    printf "\r%-70s\r" " "
}

usb_tree() {
    for d in /sys/bus/usb/devices/*; do
        [ -r "$d/idVendor" ] || continue
        printf "%-12s %s:%s maxchild=%s  %s %s\n" "$(basename "$d")" \
            "$(cat "$d/idVendor")" "$(cat "$d/idProduct")" \
            "$(cat "$d/maxchild" 2>/dev/null)" \
            "$(cat "$d/manufacturer" 2>/dev/null)" \
            "$(cat "$d/product" 2>/dev/null)"
    done
}
net_ifs() { ls /sys/class/net/ 2>/dev/null | tr '\n' ' '; }

clear
echo
echo "  ============================================"
echo "   pid351 USB ethernet diagnosis"
echo "  ============================================"
echo
echo "   No keypresses needed - just follow the timer."
echo
sleep 4

echo "   STEP 1 of 2"
echo "   >>> UNPLUG the ethernet dongle now. <<<"
echo
countdown 15 "capturing baseline in"

usb_tree > $TMP/usb.before
net_ifs   > $TMP/net.before
dmesg | wc -l > $TMP/dmesg.mark
echo "   Baseline: $(wc -l < $TMP/usb.before) usb devices, interfaces: $(cat $TMP/net.before)"
echo
sleep 2

echo "   STEP 2 of 2"
echo "   >>> PLUG the dongle into the OTG port NOW. <<<"
echo
countdown 25 "watching the bus for"

usb_tree > $TMP/usb.after
net_ifs   > $TMP/net.after
MARK=$(cat $TMP/dmesg.mark)
dmesg | tail -n +$((MARK + 1)) > $TMP/dmesg.new

echo "   Configuring any interface found..."

{
sec "THE ANSWER: what changed when the dongle was plugged in"
echo "--- new kernel messages ---"
if [ -s $TMP/dmesg.new ]; then cat $TMP/dmesg.new; else
    echo "*** NOTHING. The kernel did not react at all. ***"
fi
echo
echo "--- usb before ---"; cat $TMP/usb.before
echo "--- usb after ---";  cat $TMP/usb.after
echo "--- interfaces before: $(cat $TMP/net.before)"
echo "--- interfaces after:  $(cat $TMP/net.after)"

sec "interface detail"
for n in /sys/class/net/*; do
    [ -d "$n" ] || continue
    echo "$(basename "$n"): operstate=$(cat "$n/operstate" 2>/dev/null) carrier=$(cat "$n/carrier" 2>/dev/null) mac=$(cat "$n/address" 2>/dev/null)"
done
run ip addr
run ip route

sec "usb role and knobs"
run cat /sys/class/udc/ff300000.usb/state
find /sys/devices/platform/ff300000.usb /sys/kernel/debug -maxdepth 3 \
     \( -name "*mode*" -o -name "*role*" -o -name "*otg*" \) 2>/dev/null | while read -r f; do
    [ -f "$f" ] && echo "$f = $(cat "$f" 2>/dev/null)"
done

sec "drivers"
run lsmod
echo "--- host usb-net drivers on disk ---"; ls /lib/modules/*/kernel/drivers/net/usb/ 2>&1
echo "--- gadget drivers on disk ---";       ls /lib/modules/*/kernel/drivers/usb/gadget/ 2>&1

sec "forced bring-up"
for n in /sys/class/net/*; do
    IF=$(basename "$n")
    [ "$IF" = "lo" ] && continue
    echo "--- $IF ---"
    sudo ip link set "$IF" up 2>&1
    sleep 2
    echo "carrier: $(cat "$n/carrier" 2>/dev/null)"
    sudo timeout 20 dhclient -v "$IF" 2>&1 | tail -6
    if ! ip -4 addr show "$IF" | grep -q "inet "; then
        echo "no lease, assigning static 192.168.7.2/24"
        sudo ip addr add 192.168.7.2/24 dev "$IF" 2>&1
    fi
done

sec "sshd"
sudo systemctl start ssh.service 2>&1
run systemctl is-active ssh.service

sec "battery"
for f in voltage_now current_now capacity; do
    echo "$f: $(cat /sys/class/power_supply/battery/$f 2>/dev/null)"
done

sec "full dmesg"
run dmesg
} > "$OUT" 2>&1

sync
clear
echo
echo "  ============================================"
echo "   RESULT"
echo "  ============================================"
echo
if [ -s $TMP/dmesg.new ]; then
    echo "   Kernel DID react to the dongle:"
    grep -iE "usb|eth|net" $TMP/dmesg.new | tail -8 | sed 's/^/     /'
else
    echo "   *** Kernel did NOT react at all. ***"
    echo "   The port never saw the device."
fi
echo
echo "   Addresses:"
ip -4 addr | awk '/inet /{print "     " $NF ": " $2}'
echo
echo "   sshd: $(systemctl is-active ssh.service 2>/dev/null)"
echo
echo "   Report written to:"
echo "     $OUT"
echo
echo "   This screen stays up for 2 minutes. Write down any"
echo "   IP address shown above."
echo
countdown 120 "closing in"
