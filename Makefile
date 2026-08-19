# pid351
#
#   make            build for this laptop (SDL2)   -> build/host/pid351
#   make device     build for the RG351P (static)  -> build/device/pid351
#   make run        build and run the host version
#   make push       build for device and scp it over
#
# No build system, no configure step, no dependencies beyond libc and SDL2 on
# the host side. The device binary is static and depends on nothing at all.

CROSS   ?= aarch64-linux-gnu-
# No mDNS on ArkOS, so pass the address: make push RG=ark@192.168.1.x
RG      ?= ark@rg351p

WARN     = -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wconversion
CFLAGS   = $(WARN) -O2 -g -Isrc -D_POSIX_C_SOURCE=200809L
# aud_alsa.c is in COMMON rather than in either backend on purpose: both
# targets are Linux with ALSA, so the audio path that runs on the laptop is
# byte for byte the one that runs on the device.
COMMON   = src/main.c src/aud_alsa.c src/core.c

HOST_CFLAGS = $(CFLAGS) $(shell pkg-config --cflags sdl3)
HOST_LIBS   = $(shell pkg-config --libs sdl3) -lm
HOST_SRC    = $(COMMON) src/plat_sdl.c

# Cores are prebuilt relocatable objects, one per console, each exporting a
# prefixed copy of the libretro API and nothing else. Built by cores/get.sh
# rather than here: they have their own build systems, they will never be
# clean under our warning flags, and they change only when refetched.
# -lm is theirs too - no part of pid351 proper uses libm.
CORE_OBJ     = cores/nes_core.o
CORE_OBJ_DEV = cores/nes_core_aarch64.o

# -static so the device binary carries no runtime dependency whatsoever, which
# is the point: eventually it is the only thing in userspace.
#
# -mcpu=cortex-a35 because that is exactly what an RK3326 has, four of. The
# A35 is in-order, so instruction scheduling actually matters here in a way it
# does not on the out-of-order core gcc assumes by default.
DEV_CFLAGS  = $(CFLAGS) -static -mcpu=cortex-a35 -flto
DEV_SRC     = $(COMMON) src/plat_drm.c

.PHONY: all host device run push clean

all: host

host: build/host/pid351
device: build/device/pid351

build/host/pid351: $(HOST_SRC) src/*.h $(CORE_OBJ)
	@mkdir -p $(@D)
	$(CC) $(HOST_CFLAGS) -o $@ $(HOST_SRC) $(CORE_OBJ) $(HOST_LIBS)

build/device/pid351: $(DEV_SRC) src/*.h $(CORE_OBJ_DEV)
	@mkdir -p $(@D)
	$(CROSS)gcc $(DEV_CFLAGS) -o $@ $(DEV_SRC) $(CORE_OBJ_DEV) -lm

run: host
	./build/host/pid351

push: device
	scp build/device/pid351 $(RG):~/

clean:
	rm -rf build
