#!/bin/sh
# pid351 - fetch, build and blob the emulator cores
#
#   ./cores/get.sh            build every core for both targets
#   ./cores/get.sh nes        build one
#
# Cores are not vendored into the repository. They are large, they are not our
# code, and pinning a tarball by hash says the same thing a checked-in copy
# would without carrying it in every clone. Nothing here patches a core: the
# renaming that lets several coexist happens on the object file afterwards,
# so refetching at a newer commit costs nothing.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)

# libretro-common files that a STATIC_LINKING build leaves undefined. Cores
# built as shared objects get these from RetroArch; we are the frontend, so we
# supply them. Compiled into each core's own blob and localised there, which
# is deliberate - two cores carrying different vintages of libretro-common is
# normal and must not become a link error.
LC_SRCS="streams/file_stream.c streams/file_stream_transforms.c
         file/file_path.c file/file_path_io.c string/stdstring.c
         compat/compat_strl.c compat/fopen_utf8.c vfs/vfs_implementation.c
         encodings/encoding_utf.c time/rtime.c"

# name | prefix | tarball | unpacked dir | libretro-common path | make flags
#
# The make flags are where a core is told to produce RGB565. Every core has
# its own spelling for this and several default to 32bpp, so it is per-core
# and not a global. Getting it wrong is not a build failure - the core just
# renders in a format we did not agree to - which is why core.c checks that
# the negotiation actually happened rather than trusting this column.
CORES="fceumm|nes|https://github.com/libretro/libretro-fceumm/archive/refs/heads/master.tar.gz|libretro-fceumm-master|src/drivers/libretro/libretro-common|WANT_32BPP=0"

one() {
    IFS='|' read -r name prefix url dir lc mkflags <<EOF
$1
EOF
    cd "$HERE"
    if [ ! -d "$dir" ]; then
        echo "== fetching $name"
        curl -sL -o "$name.tar.gz" "$url"
        tar xzf "$name.tar.gz"
    fi

    for target in host device; do
        if [ "$target" = host ]; then
            cc=cc; ar=ar; ld=ld; oc=objcopy; out="${prefix}_core.o"
        else
            cc="aarch64-linux-gnu-gcc -mcpu=cortex-a35"
            ar=aarch64-linux-gnu-ar
            ld=aarch64-linux-gnu-ld
            oc=aarch64-linux-gnu-objcopy
            out="${prefix}_core_aarch64.o"
        fi

        echo "== building $name for $target"
        cd "$HERE/$dir"
        make -f Makefile.libretro platform=unix clean >/dev/null 2>&1 || true
        # CFLAGS is not overridden: the core's own makefile builds it out of a
        # dozen -D flags it needs, and replacing it removes them. Arch flags
        # ride along on CC instead.
        # shellcheck disable=SC2086
        make -f Makefile.libretro platform=unix STATIC_LINKING=1 $mkflags \
             CC="$cc" AR="$ar" -j"$(nproc)" >/dev/null

        obj=$(mktemp -d)
        for f in $LC_SRCS; do
            [ -f "$lc/$f" ] || continue
            $cc -c -O2 -DNDEBUG -D__LIBRETRO__ -DHAVE_STDINT_H \
                -I"$lc/include" -o "$obj/$(echo "$f" | tr '/' '_' | sed 's/\.c$/.o/')" \
                "$lc/$f"
        done
        cp "${name}_libretro.so" "$obj/full.a"
        $ar r "$obj/full.a" "$obj"/*.o 2>/dev/null

        "$HERE/blob.sh" "$prefix" "$obj/full.a" "$HERE/$out" "$oc" "$ld"
        rm -rf "$obj"
        echo "   -> cores/$out  $(stat -c%s "$HERE/$out") bytes"
        cd "$HERE"
    done
}

for c in $CORES; do
    case "$1" in
        "") one "$c" ;;
        *)  case "$c" in "$1|"*|*"|$1|"*) one "$c" ;; esac ;;
    esac
done
