#!/bin/bash
# pid351 - USB ethernet diagnosis
#
# Watches the USB bus across an unplug/plug cycle, which is the one thing that
# definitively separates "the port sees nothing" from "it enumerates but no
# driver binds" from "it works but DHCP fails".
#
# Also bypasses ArkOS's own precondition: it refuses to start sshd unless a
# default route exists, i.e. unless there is internet. We do not need internet,
# we need an address the laptop can reach.

OUT="$(dirname "$0")/pid351-netdiag.txt"
TMP=/tmp/pid351-diag
mkdir -p $TMP

sec() { echo; echo "===== $* ====="; }
run() { echo "--- \$ $* ---"; "$@" 2>&1 || echo "(failed or absent)"; }

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
echo "======================================================"
echo "  pid351 USB ethernet diagnosis"
echo "======================================================"
echo
echo "  STEP 1: UNPLUG the ethernet dongle completely."
echo
echo -n "  Then press any key... "
read -n 1 -s
echo; echo

usb_tree > $TMP/usb.before
net_ifs   > $TMP/net.before
dmesg | wc -l > $TMP/dmesg.mark

echo "  Baseline captured."
echo "  USB devices: $(wc -l < $TMP/usb.before)   interfaces: $(cat $TMP/net.before)"
echo
echo "  STEP 2: PLUG the dongle into the OTG port NOW."
echo
echo -n "  Then press any key... "
read -n 1 -s
echo; echo "  Waiting 12s for enumeration..."
sleep 12

usb_tree > $TMP/usb.after
net_ifs   > $TMP/net.after
MARK=$(cat $TMP/dmesg.mark)
dmesg | tail -n +$((MARK + 1)) > $TMP/dmesg.new

{
sec "THE ANSWER: what changed when the dongle was plugged in"
echo "--- new kernel messages ---"
if [ -s $TMP/dmesg.new ]; then cat $TMP/dmesg.new; else
    echo "*** NOTHING. The kernel did not react at all. ***"
    echo "*** The port is not enumerating the device.   ***"
fi
echo
echo "--- usb devices before ---"; cat $TMP/usb.before
echo "--- usb devices after ---";  cat $TMP/usb.after
echo
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
for f in /sys/devices/platform/ff300000.usb/dr_mode \
         /sys/devices/platform/ff300000.usb/mode \
         /sys/kernel/debug/ff300000.usb/force_mode; do
    [ -r "$f" ] && echo "$f = $(cat $f 2>/dev/null)"
done
for e in /sys/class/extcon/*; do
    [ -d "$e" ] && { echo "--- $e ---"; cat "$e/state" 2>/dev/null; }
done

sec "drivers"
run lsmod
echo "--- host-side usb net drivers on disk ---"
ls /lib/modules/*/kernel/drivers/net/usb/ 2>&1
echo "--- gadget-side drivers on disk (fallback route) ---"
ls /lib/modules/*/kernel/drivers/usb/gadget/ 2>&1
ls /lib/modules/*/kernel/drivers/usb/gadget/function/ 2>&1

sec "forced bring-up"
for n in /sys/class/net/*; do
    IF=$(basename "$n")
    [ "$IF" = "lo" ] && continue
    echo "--- $IF ---"
    sudo ip link set "$IF" up 2>&1
    sleep 2
    echo "carrier after up: $(cat "$n/carrier" 2>/dev/null)"
    sudo timeout 20 dhclient -v "$IF" 2>&1 | tail -6
    if ! ip -4 addr show "$IF" | grep -q "inet "; then
        echo "no lease, assigning static 192.168.7.2/24"
        sudo ip addr add 192.168.7.2/24 dev "$IF" 2>&1
    fi
done

sec "sshd"
sudo systemctl start ssh.service 2>&1
run systemctl is-active ssh.service

sec "full dmesg"
run dmesg
} > "$OUT" 2>&1

sync
clear
echo "======================================================"
echo "  RESULT"
echo "======================================================"
echo
if [ -s $TMP/dmesg.new ]; then
    echo "  Kernel DID react to the dongle:"
    grep -iE "usb|eth|net" $TMP/dmesg.new | tail -8 | sed 's/^/    /'
else
    echo "  *** Kernel did NOT react at all. ***"
    echo "  The port never saw the device."
fi
echo
echo "  Addresses now configured:"
ip -4 addr | awk '/inet /{print "    " $NF ": " $2}'
echo
echo "  sshd: $(systemctl is-active ssh.service 2>/dev/null)"
echo
echo "  Report: $OUT"
echo
echo -n "  Press any key to exit... "
read -n 1 -s -t 120
