#!/bin/bash
# pid351 - peripheral mode fallback
#
# ONLY run this if netdiag showed the port never enumerated the dongle. This
# tries the opposite approach: make the handheld a USB device rather than a
# host, so it appears as an ethernet adapter to the laptop over a plain USB-C
# cable.
#
# WARNING: the internal gamepad is a USB device on this same dwc2 controller,
# which holds one role at a time. If this succeeds the gamepad stops working
# until you reboot. That is survivable: the power button is on the PMIC
# (input event0), not on USB, so you can always power the device off.
#
# Nothing here is persistent. A reboot undoes all of it.

OUT="$(dirname "$0")/pid351-gadget.txt"

sec() { echo; echo "===== $* ====="; }
run() { echo "--- \$ $* ---"; "$@" 2>&1 || echo "(failed or absent)"; }

clear
echo "======================================================"
echo "  pid351 - USB peripheral mode (fallback)"
echo "======================================================"
echo
echo "  This will probably disable the gamepad until reboot."
echo "  The power button still works regardless."
echo
echo "  Connect a USB-C cable from the handheld to the laptop"
echo "  FIRST, then press any key. Ctrl-C or wait to abort."
echo
echo -n "  Press any key to continue... "
read -n 1 -s -t 60 || { echo; echo "  aborted."; exit 0; }
echo; echo "  Working..."

{
sec "gadget modules available"
ls /lib/modules/*/kernel/drivers/usb/gadget/ 2>&1
ls /lib/modules/*/kernel/drivers/usb/gadget/function/ 2>&1
ls /lib/modules/*/kernel/drivers/usb/gadget/legacy/ 2>&1

sec "current role"
run cat /sys/class/udc/ff300000.usb/state
# Vendor kernels hide the role switch in different places, so cast a wide net
# rather than guessing one path.
echo "--- candidate mode knobs ---"
find /sys/devices/platform/ff300000.usb /sys/kernel/debug -maxdepth 3 \
     \( -name "*mode*" -o -name "*role*" -o -name "*otg*" \) 2>/dev/null | while read -r f; do
    [ -f "$f" ] && echo "$f = $(cat "$f" 2>/dev/null)"
done

sec "attempt: force peripheral"
for f in /sys/devices/platform/ff300000.usb/dr_mode \
         /sys/kernel/debug/ff300000.usb/force_mode \
         /sys/devices/platform/ff300000.usb/mode; do
    if [ -w "$f" ] || sudo test -w "$f"; then
        echo "writing 'peripheral' to $f"
        echo peripheral | sudo tee "$f" 2>&1
    fi
done

sec "attempt: load ethernet gadget"
sudo modprobe libcomposite 2>&1
sudo modprobe usb_f_ecm 2>&1
sudo modprobe g_ether 2>&1
sleep 3
run cat /sys/class/udc/ff300000.usb/state
run ip link

sec "configure usb0"
if [ -d /sys/class/net/usb0 ]; then
    sudo ip link set usb0 up
    sudo ip addr add 192.168.7.2/24 dev usb0 2>&1
    echo "usb0 configured as 192.168.7.2/24"
else
    echo "*** no usb0 appeared - peripheral mode is not available ***"
fi

sec "sshd"
sudo systemctl start ssh.service 2>&1
run systemctl is-active ssh.service

sec "result"
run ip addr
run dmesg
} > "$OUT" 2>&1

sync
clear
echo "======================================================"
echo "  RESULT"
echo "======================================================"
echo
if [ -d /sys/class/net/usb0 ]; then
    echo "  usb0 is up at 192.168.7.2"
    echo
    echo "  On the laptop, set the matching side:"
    echo "    sudo ip addr add 192.168.7.1/24 dev <usb iface>"
    echo "    sudo ip link set <usb iface> up"
    echo "    ssh ark@192.168.7.2"
else
    echo "  *** peripheral mode did not work ***"
    echo "  No usb0 interface appeared."
fi
echo
echo "  sshd: $(systemctl is-active ssh.service 2>/dev/null)"
echo "  Report: $OUT"
echo
echo "  Reboot to restore the gamepad."
echo
echo -n "  Press any key to exit... "
read -n 1 -s -t 120
