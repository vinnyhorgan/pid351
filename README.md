# pid351

A single-process OS for one Anbernic RG351P. One binary, five consoles, no
daemons, no config files, no init system worth the name.

Not general purpose. Not portable. Not for you. That is the point — every
setting that can be a compile-time constant is one, and every feature that
exists for hardware this device does not have is absent.

See [PLAN.md](PLAN.md) for the roadmap and the constraints driving it.

## Layout

    src/pid351.h     panel geometry, pixel format, button bitmask
    src/platform.h   the platform interface — six functions, two backends
    src/scale.h      scaling policy, shared so backends cannot disagree
    src/main.c       main loop
    src/plat_sdl.c   laptop backend (SDL3), development only
    src/plat_drm.c   RG351P backend (KMS/RGA/evdev/ALSA)

Exactly one backend is linked at a time, chosen by the Makefile. The interface
is plain functions rather than a struct of pointers because the choice is known
at build time, so runtime indirection would buy nothing.

## Build

    make            laptop build (SDL3)      -> build/host/pid351
    make device     RG351P build (static)    -> build/device/pid351
    make run        build and run locally
    make push       cross-compile and scp to the device

Laptop controls: arrows are the d-pad, `X`/`Z`/`S`/`A` are A/B/X/Y,
Enter is Start, Backspace is Select, Escape quits.

## Pixel format

RGB565 end to end. Cores emit it natively, the RK3326 VOP scans it out
directly, and it halves the bandwidth of every blit versus XRGB8888. On a
device where battery life is the whole point, that is not a micro-optimisation.
