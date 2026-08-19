#!/bin/sh
# Build the pid351 image: one kernel with our binary inside it, plus a dtb.
#
# The initramfs is built *into* the Image rather than loaded beside it. That
# is not only tidier - it removes a dependency on U-Boot having ramdisk_addr_r
# set, which we cannot test without booting, and the whole point of the first
# image is to have as few untestable assumptions in it as possible.
set -e

KVER=6.12.103
HERE=$(cd "$(dirname "$0")" && pwd)
KDIR="$HERE/linux-$KVER"
OUT="$HERE/out"

KSHA=f143aaade8877ba5616e788b4482576db28481bcf557ef537f4fcc3938fc3176
KURL=https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-$KVER.tar.xz

export ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-

# The kernel tree is not in the repository - it is 2.8 GB of someone else's
# source. Fetch it on demand instead, and check it, so a fresh clone is one
# command away from a bootable image without git ever carrying the weight.
if [ ! -d "$KDIR" ]; then
    TAR="$HERE/linux-$KVER.tar.xz"
    if [ ! -f "$TAR" ]; then
        echo "==> fetching linux-$KVER"
        curl -# -o "$TAR" "$KURL"
    fi
    echo "==> verifying"
    echo "$KSHA  $TAR" | sha256sum -c - || {
        echo "checksum mismatch - refusing to build on it"; exit 1; }
    echo "==> unpacking"
    tar -C "$HERE" -xf "$TAR"
fi

echo "==> building the binary"
make -C "$HERE/.." device

echo "==> staging the initramfs"
# A gen_init_cpio file list rather than a directory, so ownership is stated
# instead of inherited from whoever ran the build. root:root without needing
# to be root, which is the only reason this project has never called sudo.
mkdir -p "$OUT"
# /dev/console has to exist *inside* the initramfs: the kernel opens it for
# init's stdio before anything of ours has run, and devtmpfs is not mounted
# yet. Without it the first boot printed "unable to open an initial console"
# and everything we said before the /dev/kmsg redirect went nowhere.
cat > "$OUT/initramfs.list" <<LIST
dir /dev 0755 0 0
nod /dev/console 0600 0 0 c 5 1
dir /proc 0755 0 0
dir /sys 0755 0 0
dir /boot 0755 0 0
file /init $HERE/../build/device/pid351 0755 0 0
LIST

echo "==> configuring"
cd "$KDIR"
# Derived from arm64 defconfig plus our fragment every time, never edited in
# place. The tree is not in the repository, so a .config that was once set up
# by hand is a fact only this machine has - and the two worst bugs of the
# project so far were both a symbol arriving through defconfig inheritance
# without anyone deciding it. merge_config.sh re-runs the merge and warns for
# every value in the fragment that did not survive, which is the check that
# would have caught CONFIG_SND_ALOOP taking card 0 before a whole session was
# spent looking for the missing audio somewhere else.
./scripts/kconfig/merge_config.sh -Q -m \
    arch/arm64/configs/defconfig "$HERE/pid351.config"
./scripts/config --set-str INITRAMFS_SOURCE "$OUT/initramfs.list"
make olddefconfig >/dev/null

# merge_config -m only concatenates; olddefconfig is what resolves it, and a
# symbol can still lose to a dependency it does not state. So check the result
# rather than the intent, and stop rather than quietly build a kernel whose
# sound card is a loopback again.
grep -q '^# CONFIG_SND_DRIVERS is not set' .config || {
    echo "CONFIG_SND_DRIVERS survived the merge - card 0 would be a loopback"
    exit 1; }
grep -q '^# CONFIG_LOGO is not set' .config || {
    echo "merge lost CONFIG_LOGO=n - the penguins are back"; exit 1; }
grep -q '^CONFIG_FRAMEBUFFER_CONSOLE_ROTATION=y' .config || {
    echo "merge lost fbcon rotation - a panic would print sideways"; exit 1; }
grep -q '^CONFIG_PL330_DMA=y' .config || {
    echo "CONFIG_PL330_DMA lost - the i2s block would have no DMA"; exit 1; }

echo "==> building the kernel"
make -j"$(nproc)" Image

echo "==> building our device tree"
# Built with cpp and dtc directly rather than by adding a line to the kernel's
# own Makefile, so the board file stays out of the tree and carries forward
# across kernel updates on its own. That used to be the whole story - the tree
# was exactly as it shipped - and since the codec patch above it is no longer
# true. It is still worth keeping the board file out of it: one patch to
# rebase is a different thing from two.
DTSDIR="$KDIR/arch/arm64/boot/dts/rockchip"
cp "$HERE/pid351-rg351p.dts" "$DTSDIR/"
${CROSS_COMPILE}cpp -nostdinc \
    -I "$KDIR/include" -I "$KDIR/arch/arm64/boot/dts" -I "$DTSDIR" \
    -undef -D__DTS__ -x assembler-with-cpp \
    "$DTSDIR/pid351-rg351p.dts" -o "$OUT/pid351-rg351p.pre.dts"
dtc -I dts -O dtb -o "$OUT/pid351-rg351p.dtb" "$OUT/pid351-rg351p.pre.dts"

echo "==> collecting"
mkdir -p "$OUT"
cp arch/arm64/boot/Image "$OUT/Image"
cp "$HERE/extlinux.conf" "$OUT/extlinux.conf"

echo
echo "image ready in $OUT:"
ls -la "$OUT"
