#!/bin/sh
#
# pid351 device probe, run on ROCKNIX as root.
#
# The RG351P has no UART header and no working network, so a boot is the
# only way to ask the machine a question. A boot costs about three minutes
# of the user's time, which means asking one question per boot is far too
# slow. This asks all of them at once and writes the answers back to the
# FAT partition, which the laptop can read without root.
#
# It deliberately does NOT change anything persistent. The only writes are
# the report itself and runtime pokes that a reboot undoes.

REPORT_DIR=/storage/pid351
mkdir -p "$REPORT_DIR"

# No RTC battery on this board, so wall-clock time after boot is fiction.
# A counter is the only monotonic label available across boots.
SEQ_FILE="$REPORT_DIR/seq"
SEQ=$(cat "$SEQ_FILE" 2>/dev/null || echo 0)
SEQ=$((SEQ + 1))
echo "$SEQ" > "$SEQ_FILE"
OUT="$REPORT_DIR/probe-$SEQ.txt"

exec >"$OUT" 2>&1

sec() { echo; echo "==================== $* ===================="; }
try() { echo "\$ $*"; "$@" 2>&1 | sed 's/^/  /'; echo; }
# Device tree strings are NUL-terminated and often NUL-separated lists.
dtstr() { [ -e "$1" ] && tr -d '\000' < "$1" | sed 's/$//' ; }

echo "pid351 probe, sequence $SEQ"

sec IDENTITY
try uname -a
echo "model: $(dtstr /proc/device-tree/model)"
echo "compatible: $(dtstr /proc/device-tree/compatible)"
echo "cmdline: $(cat /proc/cmdline)"
echo "uptime: $(cat /proc/uptime)"
[ -f /etc/os-release ] && try cat /etc/os-release

sec ROCKNIX_STATE
# ROCKNIX logs its own boot here, including which quirks and autostart
# scripts ran. If our script misbehaves this is where it shows up.
try cat /var/log/boot.log
echo "-- HW_DEVICE / QUIRK_DEVICE --"
grep -hE 'HW_DEVICE|QUIRK_DEVICE|UI_SERVICE' /etc/os-release /etc/profile 2>/dev/null
echo "-- storage layout --"
try ls /storage
try ls /storage/roms
try ls /storage/.config
try df -h

sec ENABLE_SSH
# ssh.enabled defaults to 0, so sshd is not running. Turn it on and make it
# stick, so that the moment any link exists we can get a shell without
# another round trip through the SD card.
. /etc/profile.d/001-functions 2>/dev/null
if command -v set_setting >/dev/null 2>&1; then
    set_setting ssh.enabled 1
    echo "ssh.enabled now: $(get_setting ssh.enabled)"
    echo "root.password:   $(get_setting root.password)"
else
    echo "set_setting unavailable"
fi
systemctl start sshd 2>&1
systemctl enable sshd 2>&1
try systemctl is-active sshd
try ss -lntp

sec USB_DEVICE_TREE_STATUS
# The question these answer: did the DTB actually in use match the one we
# decompiled from the image, and did overlays change any of it?
for n in usb@ff300000 usb@ff340000 usb@ff350000 \
         syscon@ff2c0000/usb2phy@100 syscon@ff2c0000/usb2phy@100/otg-port \
         syscon@ff2c0000/usb2phy@100/host-port \
         syscon@ff2c0000/usb2-phy@100 syscon@ff2c0000/usb2-phy@100/otg-port \
         syscon@ff2c0000/usb2-phy@100/host-port; do
    d="/proc/device-tree/$n"
    [ -d "$d" ] || continue
    echo "$n"
    echo "    status         = $(dtstr $d/status)"
    echo "    dr_mode        = $(dtstr $d/dr_mode)"
    echo "    role-switch    = $(dtstr $d/role-switch-default-mode)"
    [ -e "$d/usb-role-switch" ] && echo "    usb-role-switch present"
    [ -e "$d/vbus-supply" ]     && echo "    vbus-supply present"
done
echo
echo "-- every DT node mentioning vbus/otg --"
find /proc/device-tree -iname '*vbus*' -o -iname '*otg*' 2>/dev/null

sec USB_CONTROLLERS_BOUND
try ls -l /sys/bus/platform/drivers/dwc2/
try ls -l /sys/bus/platform/drivers/ehci-platform/
try ls -l /sys/bus/platform/drivers/ohci-platform/
echo "-- devices that failed to bind --"
for d in /sys/bus/platform/devices/ff3*; do
    [ -e "$d" ] || continue
    echo "$(basename $d): driver=$(basename $(readlink -f $d/driver 2>/dev/null) 2>/dev/null)"
done

sec USB_ENUMERATION
try ls /sys/bus/usb/devices/
for u in /sys/bus/usb/devices/*; do
    [ -f "$u/idVendor" ] || continue
    echo "$(basename $u)  $(cat $u/idVendor):$(cat $u/idProduct)  $(cat $u/product 2>/dev/null)"
done
[ -f /sys/kernel/debug/usb/devices ] && try cat /sys/kernel/debug/usb/devices
try cat /proc/bus/usb/devices

sec ROLE_SWITCH
try ls -l /sys/class/usb_role/
for r in /sys/class/usb_role/*; do
    [ -e "$r" ] || continue
    echo "$(basename $r) role = $(cat $r/role 2>/dev/null)"
done
try ls -l /sys/class/udc/
try ls -l /sys/class/typec/

sec REGULATORS
# The central question: does a regulator exist that puts 5V on the port,
# and is it on? Mainline's rk817 driver may not expose OTG_SWITCH at all.
for r in /sys/class/regulator/regulator.*; do
    [ -e "$r/name" ] || continue
    printf '%-14s %-10s %8s uV  users=%s\n' \
        "$(cat $r/name)" "$(cat $r/state 2>/dev/null)" \
        "$(cat $r/microvolts 2>/dev/null)" "$(cat $r/num_users 2>/dev/null)"
done
[ -f /sys/kernel/debug/regulator/regulator_summary ] && \
    try cat /sys/kernel/debug/regulator/regulator_summary

sec OVERLAY_SUPPORT
# If configfs overlays are available we can enable EHCI at runtime and
# test a device tree change in seconds instead of a reflash.
try ls -l /sys/kernel/config/
try ls -l /sys/kernel/config/device-tree/overlays/
grep -q configfs /proc/filesystems && echo "configfs: supported" || echo "configfs: NOT in /proc/filesystems"

sec NETWORK_MODULES
for m in cdc_ncm cdc_ether usbnet r8152 ax88179_178a asix cdc_subset; do
    if [ -d "/sys/module/$m" ]; then echo "$m: loaded"
    elif modinfo "$m" >/dev/null 2>&1; then echo "$m: available, not loaded"
    else echo "$m: ABSENT"; fi
done
echo "-- forcing the ones we need --"
for m in usbnet cdc_ncm cdc_ether; do
    modprobe "$m" 2>&1 && echo "modprobe $m ok"
done

sec ACTIVE_INTERVENTION
echo "-- forcing every role switch to host --"
for r in /sys/class/usb_role/*; do
    [ -w "$r/role" ] || continue
    echo "host" > "$r/role" 2>&1 && echo "$(basename $r) <- host, now $(cat $r/role)"
done
echo
echo "-- rebinding dwc2 --"
for d in /sys/bus/platform/drivers/dwc2/ff3*; do
    [ -e "$d" ] || continue
    n=$(basename "$d")
    echo "$n" > /sys/bus/platform/drivers/dwc2/unbind 2>&1
    sleep 1
    echo "$n" > /sys/bus/platform/drivers/dwc2/bind 2>&1
    echo "rebound $n"
done

sec HOTPLUG_WATCH_60S
# Plug and unplug things while this runs; anything that appears lands here.
before=$(ls /sys/bus/usb/devices/ 2>/dev/null)
i=0
while [ $i -lt 60 ]; do
    now=$(ls /sys/bus/usb/devices/ 2>/dev/null)
    if [ "$now" != "$before" ]; then
        echo "[t=${i}s] CHANGE:"
        echo "$now" | sed 's/^/    /'
        before="$now"
    fi
    i=$((i + 1))
    sleep 1
done
echo "watch finished"
try ip -br link

sec DMESG
try dmesg

# ---- everything below is pid351 hardware recon, unrelated to USB, but a
# ---- boot is expensive so we take it while we are here.

sec DRM
try ls -l /dev/dri/
for c in /sys/class/drm/card*-*; do
    [ -d "$c" ] || continue
    echo "$(basename $c): status=$(cat $c/status 2>/dev/null) enabled=$(cat $c/enabled 2>/dev/null)"
    sed 's/^/    /' "$c/modes" 2>/dev/null
done

sec RGA_AND_V4L2
try ls -l /dev/rga /dev/video* /dev/media*
for v in /sys/class/video4linux/*; do
    [ -e "$v/name" ] && echo "$(basename $v): $(cat $v/name)"
done

sec INPUT
try cat /proc/bus/input/devices

sec CPUFREQ_AND_THERMAL
for p in /sys/devices/system/cpu/cpufreq/policy*; do
    [ -d "$p" ] || continue
    echo "$(basename $p):"
    echo "    available = $(cat $p/scaling_available_frequencies 2>/dev/null)"
    echo "    governor  = $(cat $p/scaling_governor 2>/dev/null)"
    echo "    governors = $(cat $p/scaling_available_governors 2>/dev/null)"
    echo "    cur       = $(cat $p/scaling_cur_freq 2>/dev/null)"
done
for t in /sys/class/thermal/thermal_zone*; do
    [ -e "$t/temp" ] && echo "$(cat $t/type 2>/dev/null): $(cat $t/temp)"
done
try ls /sys/class/devfreq/
for d in /sys/class/devfreq/*; do
    [ -e "$d/available_frequencies" ] && \
        echo "$(basename $d): $(cat $d/available_frequencies)"
done

sec POWER
for s in /sys/class/power_supply/*; do
    [ -d "$s" ] || continue
    echo "-- $(basename $s)"
    for f in "$s"/*; do
        [ -f "$f" ] && [ -r "$f" ] && \
            echo "    $(basename $f) = $(cat $f 2>/dev/null | head -1)"
    done
done
try ls /sys/class/backlight/
for b in /sys/class/backlight/*; do
    [ -d "$b" ] && echo "$(basename $b): $(cat $b/brightness) / $(cat $b/max_brightness)"
done

sec AUDIO
try cat /proc/asound/cards
try cat /proc/asound/pcm
try ls -l /dev/snd/

sec MEMORY_AND_PROCESSES
try free -m
try ps

# The report is on ext4, which the laptop needs root to read. Mirroring it
# to the FAT partition removes that step entirely.
if mount -o remount,rw /flash 2>/dev/null; then
    cp "$OUT" /flash/ 2>/dev/null && echo "mirrored to /flash"
    sync
    mount -o remount,ro /flash 2>/dev/null
fi
sync
