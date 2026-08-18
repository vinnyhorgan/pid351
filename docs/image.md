# The pid351 image

Three files and one text file. Nothing from ROCKNIX is in any of them.

    image/build.sh          builds everything
    image/pid351.config     kernel config fragment, from the binding census
    image/extlinux.conf     boot config
    tools/install-image.sh  puts it on the card
    tools/restore-rocknix.sh rollback

## Why there is no Buildroot

Buildroot exists to build a userspace with packages and a package manager. We
have no packages. The userspace is one statically linked binary, so the only
thing that needs building is a kernel - and the kernel can carry the binary
itself.

## Why the initramfs is built into the Image

`CONFIG_INITRAMFS_SOURCE` points at a `gen_init_cpio` file list, so the whole
operating system is a single `Image` file. Two reasons, both practical:

- A separate `INITRD` line in `extlinux.conf` needs U-Boot to have
  `ramdisk_addr_r` set. ROCKNIX's own boot never uses `INITRD` - its kernel has
  a built-in initramfs too - so that variable is **unproven on this board and
  untestable without booting**. Building ours in removes the question.
  `fdt_addr_r` by contrast is proven, because ROCKNIX's `FDTDIR` needs it.
- A file list rather than a directory means ownership is stated rather than
  inherited from whoever ran the build. `root:root` without ever being root,
  which is why this project has still never called sudo.

## The kernel

Mainline 6.12.103 - deliberately the same LTS series ROCKNIX runs (6.12.79),
so driver behaviour matches every measurement in `hardware.md`. Changing the
userspace and the kernel version at once would have left nothing fixed to
compare against.

`arm64 defconfig` plus `image/pid351.config`. `CONFIG_MODULES=n` (one file, no
loader, nothing to go missing), `CONFIG_NET=n` (there is no network in the
design), no GPU, no VPU, no media stack, one SoC family. Everything switched
*on* came from the binding census taken off the running device, not from a
defconfig or a wiki.

It is 31 MB, which is not lean. That is deliberate for now: this is the kernel
that boots our hardware, and stripping it further is worth doing when there is
a real workload to measure the result against. Optimising it before the
emulator exists would mean tuning for a program that does not.

## Being PID 1

`plat_boot_init` in `src/plat_drm.c`, and every part of it is a no-op unless
`getpid() == 1`, so the identical binary still runs as an ordinary process.

- Mounts `/dev`, `/proc`, `/sys`. `CONFIG_DEVTMPFS_MOUNT` says in as many
  words that it does not apply to an initramfs root, so nobody does this for us.
- Redirects our stdout and stderr into `/dev/kmsg`. There is no shell
  capturing output and no reachable serial port, so our lines go into the
  kernel ring buffer instead - same place, same order, same timestamps as the
  kernel's own. One dump at exit then saves both, and it still reaches the
  panel through fbcon until we take DRM master.
- Waits for hardware that is not there yet. The DSI panel finishes its
  deferred probe after the initramfs is already executing, and the pad is a
  full speed USB device behind a high speed hub, which takes about 2.4 s to
  enumerate. Another init used to absorb that wait; as PID 1 nobody does.
- `plat_boot_shutdown` never returns, because reaching the end of `main` as
  PID 1 is a kernel panic. Leaving has to be a deliberate act.
- `plat_boot_save_log` mounts the FAT partition and writes the ring buffer to
  it, on the way out and on any failed bring-up. On a machine with no serial
  port that is the only way a boot can explain itself.

## Booting, and getting back

    U-Boot -> boot.scr -> sysboot -> /extlinux/extlinux.conf

Which system boots is therefore a text file on a FAT partition, editable from
the laptop with the card in a reader. `install-image.sh` backs ROCKNIX's copy
up once and `restore-rocknix.sh` puts it back. ROCKNIX's own `KERNEL`,
`boot.scr` and DTBs are never touched.

That rollback is not sentimentality about ROCKNIX. One SD card, no serial
port, no maskrom button: it is the only way back if our kernel does not come
up. It costs 25 MB and one file rename, and it can be deleted the moment ours
boots.
