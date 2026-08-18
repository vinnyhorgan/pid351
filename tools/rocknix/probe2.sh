#!/bin/sh
#
# The last text probe before we build our own image. Everything here is a
# question a custom kernel would otherwise answer by failing to boot on a
# machine with no serial console.
#
# The one that matters most is the driver census: every device that bound and
# every device that did not. That converts ROCKNIX's config, which supports
# forty handhelds, into ours, which supports one - without guessing at a
# single symbol.

OUT=/storage/pid351/probe2.txt
mkdir -p /storage/pid351

s() { echo; echo "==================== $1 ===================="; }
r() { echo "\$ $1"; eval "$1" 2>&1 | sed 's/^/  /'; }

{
echo "=== pid351 image probe, $(date) ==="
uname -a
echo "uptime: $(cat /proc/uptime)"

s WHAT_IS_RUNNING_WHEN_WE_RUN
# Our own measurements were taken from autostart, which runs before the UI.
# Whether sway and EmulationStation were up decides what the 387.7 mA
# baseline already excludes, and every estimate of ROCKNIX overhead rests on
# that.
r "ps -o pid,ppid,rss,comm 2>/dev/null | head -60"
r "cat /proc/loadavg"

s MODULES
r "lsmod"

s DRIVERS_THAT_BOUND
for b in platform i2c spi usb hid mmc; do
    [ -d /sys/bus/$b/drivers ] || continue
    echo "-- bus: $b"
    for d in /sys/bus/$b/drivers/*/; do
        n=$(basename "$d")
        for dev in "$d"*/; do
            [ -L "${dev%/}" ] || continue
            case "$dev" in *bind|*unbind|*uevent|*module|*new_id|*remove_id) continue;; esac
            echo "  $n <- $(basename "$dev")"
        done
    done
done

s PLATFORM_DEVICES_WITH_NO_DRIVER
# Anything here is a node our own device tree can simply not contain.
for d in /sys/devices/platform/*/ /sys/devices/platform/*/*/; do
    [ -e "$d/uevent" ] || continue
    [ -e "$d/driver" ] && continue
    echo "  $(basename "$d")"
done | sort -u

s INTERRUPTS
# A line with zero counts is hardware we are carrying and never using.
r "cat /proc/interrupts"

s CPUIDLE_ACTUALLY_USED
for c in /sys/devices/system/cpu/cpu*/cpuidle; do
    [ -d "$c" ] || continue
    echo "-- $c"
    for st in "$c"/state*/; do
        echo "  $(cat $st/name 2>/dev/null) usage=$(cat $st/usage 2>/dev/null) time=$(cat $st/time 2>/dev/null) us disabled=$(cat $st/disable 2>/dev/null)"
    done
done

s REGULATORS
for r in /sys/class/regulator/*/; do
    echo "  $(cat $r/name 2>/dev/null): state=$(cat $r/state 2>/dev/null) uV=$(cat $r/microvolts 2>/dev/null) users=$(cat $r/num_users 2>/dev/null)"
done | sort

s POWER_DOMAINS_AND_CLOCKS
mount -t debugfs none /sys/kernel/debug 2>/dev/null
r "cat /sys/kernel/debug/pm_genpd/pm_genpd_summary 2>/dev/null | head -40"
r "cat /sys/kernel/debug/clk/clk_summary 2>/dev/null | head -80"

s MMC_MODE_AND_SPEED
r "cat /sys/kernel/debug/mmc0/ios 2>/dev/null"
r "cat /sys/block/mmcblk0/device/name /sys/block/mmcblk0/device/date 2>/dev/null"
echo "\$ sequential read throughput, 64MB, cache dropped"
sync; echo 3 > /proc/sys/vm/drop_caches 2>/dev/null
dd if=/dev/mmcblk0 of=/dev/null bs=1M count=64 2>&1 | sed 's/^/  /'

s UBOOT
# Decides phase 4.4: if this u-boot already does ums, the card never has to
# leave the console again.
echo "\$ strings of the first 16MB of the card"
dd if=/dev/mmcblk0 bs=1M count=16 2>/dev/null | strings -n 5 > /tmp/ub.txt
grep -iE "^U-Boot 2[0-9]" /tmp/ub.txt | sort -u | head -5 | sed 's/^/  version: /'
for cmd in ums rockusb fastboot mmc usb dfu gpt setexpr bootm booti sysboot pxe; do
    grep -qx "$cmd" /tmp/ub.txt && echo "  command present: $cmd"
done
grep -iE "rockchip|px30|rk3326" /tmp/ub.txt | sort -u | head -10 | sed 's/^/  /'
rm -f /tmp/ub.txt

s BOOT_TIMING
r "dmesg | tail -5"
r "systemd-analyze 2>/dev/null || echo 'no systemd-analyze'"
r "systemd-analyze blame 2>/dev/null | head -20"

s USB_TOPOLOGY
for d in /sys/bus/usb/devices/*/; do
    [ -e "$d/idVendor" ] || continue
    echo "  $(basename $d): $(cat $d/idVendor):$(cat $d/idProduct) $(cat $d/product 2>/dev/null) speed=$(cat $d/speed 2>/dev/null)"
done

s DRM_SYSFS
for c in /sys/class/drm/card0-*/; do
    echo "-- $(basename $c)"
    echo "  status=$(cat $c/status 2>/dev/null) enabled=$(cat $c/enabled 2>/dev/null)"
    ls $c 2>/dev/null | sed 's/^/    /'
done

s THERMAL
for z in /sys/class/thermal/thermal_zone*/; do
    echo "  $(cat $z/type): $(cat $z/temp) mC"
    for t in $z/trip_point_*_temp; do
        [ -e "$t" ] && echo "    $(basename $t)=$(cat $t) type=$(cat ${t%_temp}_type 2>/dev/null)"
    done
done

s DMESG_FULL
r "dmesg"

echo
echo "=== probe2 complete ==="
} > "$OUT" 2>&1

if mount -o remount,rw /flash 2>/dev/null; then
    cp "$OUT" /flash/ 2>/dev/null
    sync
    mount -o remount,ro /flash 2>/dev/null
fi
