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
COMMON   = src/main.c

HOST_CFLAGS = $(CFLAGS) $(shell pkg-config --cflags sdl3)
HOST_LIBS   = $(shell pkg-config --libs sdl3)
HOST_SRC    = $(COMMON) src/plat_sdl.c

# -static so the device binary carries no runtime dependency whatsoever, which
# is the point: eventually it is the only thing in userspace.
DEV_CFLAGS  = $(CFLAGS) -static
DEV_SRC     = $(COMMON) src/plat_drm.c

.PHONY: all host device run push clean

all: host

host: build/host/pid351
device: build/device/pid351

build/host/pid351: $(HOST_SRC) src/*.h
	@mkdir -p $(@D)
	$(CC) $(HOST_CFLAGS) -o $@ $(HOST_SRC) $(HOST_LIBS)

build/device/pid351: $(DEV_SRC) src/*.h
	@mkdir -p $(@D)
	$(CROSS)gcc $(DEV_CFLAGS) -o $@ $(DEV_SRC)

run: host
	./build/host/pid351

push: device
	scp build/device/pid351 $(RG):~/

clean:
	rm -rf build
