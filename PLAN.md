# pid351 — plan

A single-process OS for one Anbernic RG351P. Four consoles: GBA, NES, SNES,
Genesis.

GB and GBC were dropped once the panel was measured. They are 10:9 against a
3:2 screen - a 35% mismatch that no amount of stretch hides and only black
bars accommodate. The four that remain are either an exact fit (GBA at 240x160
is precisely half the panel) or a uniform 12.5% stretch from 4:3, so every
console fills the screen and there is one display path instead of five.
mGBA still carries GB and GBC incidentally, so the decision is reversible;
what it buys is that nothing in the scaling policy has to bend for them. Priorities, in order: **battery life, minimalism, performance.**

## Constraints

These are not incidental — they invert the normal embedded workflow, so they
are written down first.

- **One microSD card**, now running pid351's own image. No spare, so no rescue
  media. ROCKNIX stays bootable behind a single file rename, and its boot
  partition is copied to the laptop.
- **No USB-UART adapter**, and none coming. Bring-up of any custom kernel is
  blind: a boot failure is a black screen with no diagnostics.
- **No purchases.** Every step must work with what is on the desk.
- Only accessory available is a Kensington UH1400P dock.

**Consequence: userspace first, on a borrowed kernel. Our own kernel and boot
chain come last.** Every embedded instinct says start at the bootloader. With
no serial console that instinct is wrong here — it would mean debugging a
display driver through a device that cannot tell us anything.

**Done, and the order paid off.** pid351 now boots its own mainline 6.12.103
kernel and runs as PID 1; the vendor system is a rollback, not a host. The
detour was not free but it was cheaper than the alternative, and building our
own kernel immediately exposed two operating points ROCKNIX's device tree had
been deleting.

## The three loops

| Loop | Latency | Scope |
|---|---|---|
| A — laptop, SDL3 | ~2s | frontend, menu, savestates, core integration. ~90% of the work. |
| B — device over ssh | ~20s | KMS, RGA, evdev, ALSA, power measurement. |
| C — reflash card | ~3min | kernel, DTB, init. Now the only loop the device has: with the vendor userspace gone there is no ssh, so B is closed and everything device-side arrives through the boot log. `ums` would make C cheap and is still unbuilt. |

## Phase 0 — ground truth

Goal: know exactly what this unit exposes. **No modifications to the SD card.**

- [x] 0.1 Network link to ArkOS — dongle in host mode, else `g_ether` gadget. ssh in.
- [x] 0.2 Cross toolchain on the laptop.
- [x] 0.3 `make push`, run the recon binary.
- [x] 0.4 Write `docs/hardware.md`: kernel version, `/proc/config.gz`, live DTB,
      DRM connectors + modes, evdev nodes and which is the gamepad, ALSA card,
      backlight range, battery sysfs, cpufreq table, RGA node.

**Exit criteria:** we know the connector name, the panel mode, the gamepad
event node, and whether rotation is already handled for us.

> ⚠️ ArkOS is likely a vendor 4.x kernel. Standard DRM/KMS is a stable API and
> ports to mainline unchanged. The RGA interface does **not**: vendor `/dev/rga`
> ioctls versus mainline's `rockchip-rga` v4l2-m2m. So the KMS path is written
> as the portable core, and RGA is an optional accelerator behind a CPU
> fallback. Designing for that split now costs nothing; retrofitting it costs a
> rewrite.

## Phase 1 — the binary runs on the device

- [x] 1.1 DRM/KMS dumb buffers, double buffered, page flip on vblank — which is
      also the frame clock, so the main loop blocks rather than spins.
- [x] 1.2 evdev input.
- [x] 1.3 CPU 2x blit and rotation. GBA needs nothing more than this.
- [x] 1.4 ~~RGA-accelerated scale/rotate~~. **Rejected, twice over.** No RGA
      node exists in the device tree, and no plane carries a rotation
      property, so neither the RGA nor the VOP can do this - it was never a
      fallback, it is the only mechanism. Moot anyway: the staged blit is
      under 5% of a frame.
- [x] 1.5 ALSA out. Raw ioctls, no alsa-lib, one implementation shared by
      both targets. Resampled to the panel in exact integer arithmetic.
      Measured on the laptop only; **not yet run on the device.** Note the
      "large buffers, few wakeups" premise lost on measurement - see
      `docs/hardware.md`.
- [x] 1.6 Battery telemetry and benchmark mode.

**Exit criteria:** the test pattern is on the panel, right way up, all four
border edges visible, d-pad moves the box, paced at 60.0186 Hz and measured.
The 59.727 Hz above was wrong - the mode's own timing is exact and says
otherwise.

## Phase 2 — it plays games

- [ ] 2.1 mGBA, statically linked — covers GBA, and GB/GBC for free if we
      ever want them back.
- [ ] 2.2 Per-core symbol prefixing so multiple cores can coexist in one binary.
      Every core exports `retro_run`; without this, core #2 will not link.
- [ ] 2.3 snes9x, a NES core, Genesis Plus GX.
- [ ] 2.4 Savestates, launcher, per-console scaling policy.

### Controls, settled

The console mapping lives per core in `src/core.c`. For the NES both Y/A and
B/A drive the two console buttons at once - Y/A is the real pad's geometry,
B/A matches the printed labels, and there is no config file in which to
record a preference between them.

The hotkey layer is **one click of R3, which opens the menu**. Nothing is
held. Holding a stick click while reaching for a face button is a two-handed
contortion, and the buttons that would be comfortable to hold - the shoulders
- belong to the SNES and the Genesis. R3 is used rather than L3 because the
left stick stands in for the d-pad, so L3 gets clicked by accident while
moving; L3 is therefore left unassigned rather than given a job that would be
triggered by mistake. Rejected: a held modifier, and per-core hotkeys.

The menu carries save, load, reset, brightness, volume and exit. Volume moves
to the shell's physical volume keys once `event3` is mapped.

There is no fast-forward. A held one has the ergonomic problem above, a
toggled one can be left on and quietly spend battery, and no console we
target needs it.

## Phase 3 — power

Driven by the harness from 1.6, never by folklore. Each change gets a measured
before and after, ranked by actual milliwatts. Mostly done, and the ranking
came out nothing like the folklore - see `docs/hardware.md`. Settled:

- [x] OPP cap. 1296 -> 1008 MHz is **54 mA** and costs no frames. (The 21.5 mA
      once quoted here came from selecting an OPP through the governor with no
      check that the core held it. Pinned and re-measured, 816 MHz is 75 mA and
      600 MHz is 89 mA - neither existed on ROCKNIX, whose DT deletes them.)
      The kernel's
      own energy model agrees: `EM: OPP:1296000 is inefficient`, because 1296
      and 1416 share the 1.35 V rail. Use 1008, or 1416. Never 1296.
- [x] Backlight. Full range is 42.0 mA, 10.8% - real, but not the dominant
      term everyone assumes it is.
- [x] The rotate blit. STRIDED -> the staged variants is ~9.5 mA.
- [x] GPU never powered on. Already true without us: the `gpu` genpd is `off`
      at runtime, as are `vpu` and `vi`. Nothing to switch off, nothing to win.
- [x] Core offlining. Rejected: `cluster-sleep` is already entered tens of
      thousands of times a minute, so idle cores cost nothing to begin with.
- [ ] Suspend-to-savestate on power-off. The only large term left.

## Phase 4 — our own image

The risky phase, deliberately last, when everything above already works.

**It buys about zero milliamps.** Measured, not assumed: at the point our
binary runs on autostart, ROCKNIX's userspace costs less than the +-7 mA noise
floor, because sway and EmulationStation have not started yet. What the image
buys is boot time, determinism, a kernel we can strip, and control over
governor and idle policy without fighting anyone. Those are the reasons; do
not go into it expecting battery.

- [x] 4.1 ~~Buildroot~~: no Buildroot. Buildroot exists to build a userspace
      with packages and we have none - the userspace is one static binary.
      Mainline 6.12.103 (same LTS series ROCKNIX runs, so driver behaviour
      matches every measurement we have), our own config from the binding
      census in `docs/hardware.md`, `CONFIG_MODULES=n`, `CONFIG_NET=n`, no
      GPU, no VPU. `image/build.sh` produces the whole thing.
- [x] 4.2 **fbcon on tty0 is our substitute for the serial console**, and our
      own stdout is redirected into the kernel ring buffer so both end up in
      one place with one set of timestamps. `plat_boot_save_log` writes that
      buffer to the FAT partition on the way out and on any failed bring-up,
      so a dead screen still explains itself after a power cycle.
- [x] 4.3 ~~Repartition~~: not needed. The boot chain ends in
      `sysboot ... /extlinux/extlinux.conf`, so which system boots is a text
      file on a FAT partition, editable from the laptop. `tools/install-image.sh`
      installs beside ROCKNIX; `tools/restore-rocknix.sh` is the rollback.
      Original plan, kept for the record: repartition: ROCKNIX stays bootable, our boot files live alongside it.
      A marker file on the FAT partition picks which one boots — flippable from
      the laptop, no input needed on the device.
- [ ] 4.4 U-Boot with `ums`: the card mounts on the laptop over the same USB-C
      cable that powers the device. Kills card-swapping permanently, which is
      what makes having only one card survivable. **The stock U-Boot 2017.09 on
      the card does not have `ums`** - it has `rockusb`, which needs a console
      or the maskrom button to reach - so this has to be built, and until it is
      the recovery plan is entirely physical.
- [x] 4.5 pid351 as PID 1. It mounts `/dev`, `/proc` and `/sys` itself
      (`CONFIG_DEVTMPFS_MOUNT` does not apply to an initramfs root), waits for
      the DSI panel to finish its deferred probe and for the pad to enumerate
      over USB - about 2.4 s, a wait another init used to absorb for us - and
      never returns from `main`, because PID 1 returning is a kernel panic.
      `panic=5` rather than 1 during bring-up: long enough to read the screen.
- [ ] 4.6 Per-console display modes, savestate-and-poweroff suspend, 2s boot.

### Blind bring-up rules

1. Change exactly one thing per boot.
2. ROCKNIX stays bootable until the very end.
3. Every experimental kernel writes dmesg to FAT before anything else.
4. No working card reader means no recovery — confirm the reader before the
   first repartition. (Floor below that: RK3326 maskrom over USB with
   `rkdeveloptool`, since an SD-only device with no card enters maskrom.)

## Open questions

Phase 0 and the five device runs answered all of the original ones:

- ~~What kernel and DRM stack does the stock system run?~~ ROCKNIX, Linux
  6.12.79, `rockchip-drm` + `rockchip-vop` + `dw-mipi-dsi-rockchip` + the
  `elida,kd35t133` panel driver. GPU is the `mali_kbase` vendor blob, not
  panfrost, and its power domain is off at runtime.
- ~~Is the panel already presented as landscape?~~ No. It is a 320x480 portrait
  panel and **the VOP has no `rotation` property on any plane**, so we owe it a
  CPU rotate and always will.
- ~~Is the gamepad USB HID or GPIO+ADC?~~ USB HID: `1209:3100` at full speed,
  behind an internal 480 Mb/s hub. That hub is why dwc2 fires ~6700 times a
  second, the largest wakeup source on the machine.
- ~~Does the ethernet dongle work in host mode?~~ Moot. There is no network in
  the design and the `gmac` power domain is off.

Still open, and deliberately so:

- Audio. ALSA on `rk817-codec` via `rockchip-i2s`, the unmapped volume keys on
  the pad's keyboard interface, and headphone detect on `event2`.
