#!/bin/bash
# pid351 - phase 0 reconnaissance
#
# Runs on the RG351P under whatever distro is currently installed, and dumps
# everything pid351 needs to know about this specific unit into a text file
# next to this script. Reads only; changes nothing.
#
# Put this in ArkOS's tools directory (/roms/tools) so EmulationStation lists
# it as a launchable entry - that gives us a way to run commands on the device
# with nothing but the gamepad, no keyboard and no network.

OUT="$(dirname "$0")/pid351-recon.txt"

say() { echo "$@"; }
sec() { echo; echo "===== $* ====="; }
run() { echo "--- \$ $* ---"; "$@" 2>&1 || echo "(failed or absent)"; }
cat_if() { [ -r "$1" ] && { echo "--- $1 ---"; cat "$1"; }; }

{
say "pid351 recon - $(date 2>/dev/null)"

sec "kernel"
run uname -a
cat_if /proc/version
cat_if /proc/cmdline
# The kernel config tells us what is compiled in - crucially whether the RGA
# and v4l2 m2m drivers exist, and which DRM stack this is.
if [ -r /proc/config.gz ]; then
    echo "--- /proc/config.gz (filtered) ---"
    zcat /proc/config.gz | grep -Ei "rockchip|drm|rga|panfrost|fb_|framebuffer_console|v4l2|dwc2|usb_gadget|no_hz|cpufreq" | grep -v "^#"
else
    say "(no /proc/config.gz - look for /boot/config-* instead)"
    ls /boot 2>/dev/null
fi

sec "device tree"
cat_if /proc/device-tree/model
echo
cat_if /proc/device-tree/compatible
echo
say "(full DTB is at /sys/firmware/fdt if present)"
ls -la /sys/firmware/fdt 2>/dev/null

sec "display"
# The question that decides the whole display backend: is the mode we get
# already landscape 480x320, or the panel's native portrait 320x480?
for c in /sys/class/drm/card*-*; do
    [ -d "$c" ] || continue
    echo "--- $c ---"
    for f in status enabled modes; do
        [ -r "$c/$f" ] && echo "  $f: $(tr '\n' ' ' < "$c/$f")"
    done
done
run ls -la /dev/dri

sec "input"
# /proc/bus/input/devices names each device and maps it to its event node,
# which is how we find the gamepad rather than guessing at event0.
cat_if /proc/bus/input/devices
run ls -la /dev/input

sec "rga / v4l2"
run ls -la /dev/rga /dev/video0 /dev/video1 /dev/video2
run ls /sys/class/video4linux

sec "audio"
cat_if /proc/asound/cards
run aplay -l

sec "backlight"
for b in /sys/class/backlight/*; do
    [ -d "$b" ] || continue
    echo "--- $b ---"
    for f in brightness max_brightness actual_brightness type; do
        [ -r "$b/$f" ] && echo "  $f: $(cat "$b/$f")"
    done
done

sec "power"
for p in /sys/class/power_supply/*; do
    [ -d "$p" ] || continue
    echo "--- $p ---"
    for f in type status present voltage_now current_now capacity charge_full health; do
        [ -r "$p/$f" ] && echo "  $f: $(cat "$p/$f")"
    done
done

sec "cpu"
cat_if /sys/devices/system/cpu/online
cat_if /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
cat_if /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq
cat_if /sys/devices/system/cpu/cpu0/cpufreq/scaling_available_frequencies
cat_if /sys/devices/system/cpu/cpu0/cpufreq/scaling_available_governors
run ls /sys/class/devfreq

sec "usb"
run lsusb
cat_if /sys/class/udc/*/state
run ls /sys/class/udc
run ip link

sec "baseline to beat"
# The numbers that started this project. Worth recording so there is a
# before to compare pid351's after against.
run free -m
say "--- process count ---"
ps ax 2>/dev/null | wc -l
run ps aux --sort=-rss
run systemctl list-units --type=service --state=running

sec "cross-compiled binary"
# Proves a statically linked aarch64 binary built with Debian's gcc 14 runs on
# this 2021-vintage 4.4 kernel, before phase 1 is built on that assumption.
BIN="$(dirname "$0")/pid351"
if [ -x "$BIN" ]; then
    run "$BIN"
else
    say "(pid351 binary not present next to this script)"
fi

sec "modules"
run lsmod

sec "dmesg"
run dmesg

} > "$OUT" 2>&1

sync
echo "pid351: wrote $OUT"
echo "Power off, put the card in the laptop, and copy that file back."
sleep 5
