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

- **One microSD card**, currently running ArkOS. No spare, so no rescue media.
- **No USB-UART adapter**, and none coming. Bring-up of any custom kernel is
  blind: a boot failure is a black screen with no diagnostics.
- **No purchases.** Every step must work with what is on the desk.
- Only accessory available is a USB-C ethernet dongle.

**Consequence: userspace first, on ArkOS's kernel. Our own kernel and boot
chain come last.** Every embedded instinct says start at the bootloader. With
no serial console that instinct is wrong here — it would mean debugging a
display driver through a device that cannot tell us anything. So ArkOS stays,
resented but useful, until pid351 can stand up on its own.

## The three loops

| Loop | Latency | Scope |
|---|---|---|
| A — laptop, SDL3 | ~2s | frontend, menu, savestates, core integration. ~90% of the work. |
| B — device over ssh | ~20s | KMS, RGA, evdev, ALSA, power measurement. |
| C — reflash card | ~3min | kernel, DTB, init. Deferred to phase 4, then made cheap by U-Boot `ums`. |

## Phase 0 — ground truth

Goal: know exactly what this unit exposes. **No modifications to the SD card.**

- [ ] 0.1 Network link to ArkOS — dongle in host mode, else `g_ether` gadget. ssh in.
- [ ] 0.2 Cross toolchain on the laptop.
- [ ] 0.3 `make push`, run the recon binary.
- [ ] 0.4 Write `docs/hardware.md`: kernel version, `/proc/config.gz`, live DTB,
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

- [ ] 1.1 DRM/KMS dumb buffers, double buffered, page flip on vblank — which is
      also the frame clock, so the main loop blocks rather than spins.
- [ ] 1.2 evdev input.
- [ ] 1.3 CPU 2x blit and rotation. GBA needs nothing more than this.
- [ ] 1.4 RGA-accelerated scale/rotate, behind the fallback from 1.3.
- [ ] 1.5 ALSA out, large buffers, few wakeups.
- [ ] 1.6 Battery telemetry and benchmark mode.

**Exit criteria:** the test pattern is on the panel, right way up, all four
border edges visible, d-pad moves the box, paced at 59.727Hz and measured.

## Phase 2 — it plays games

- [ ] 2.1 mGBA, statically linked — covers GBA, and GB/GBC for free if we
      ever want them back.
- [ ] 2.2 Per-core symbol prefixing so multiple cores can coexist in one binary.
      Every core exports `retro_run`; without this, core #2 will not link.
- [ ] 2.3 snes9x, a NES core, Genesis Plus GX.
- [ ] 2.4 Savestates, launcher, per-console scaling policy.

## Phase 3 — power

Driven by the harness from 1.6, never by folklore. Backlight curve, core
offlining, OPP cap, GPU never powered on, no busy-waiting anywhere. Each change
gets a measured before and after, ranked by actual milliwatts.

## Phase 4 — our own image

The risky phase, deliberately last, when everything above already works.

- [ ] 4.1 Buildroot: mainline kernel + `rk3326-anbernic-rg351m` DTB, minimal config.
- [ ] 4.2 **fbcon on tty0 is our substitute for the serial console.** If the
      panel probes, we can read boot messages. For failures before that, the
      initramfs `init` dumps dmesg to the FAT partition before doing anything
      else, so a dead screen still tells us something after a power cycle.
- [ ] 4.3 Repartition: ArkOS stays bootable, our boot files live alongside it.
      A marker file on the FAT partition picks which one boots — flippable from
      the laptop, no input needed on the device.
- [ ] 4.4 U-Boot with `ums`: the card mounts on the laptop over the same USB-C
      cable that powers the device. Kills card-swapping permanently, which is
      what makes having only one card survivable.
- [ ] 4.5 pid351 as PID 1, `panic=1` so a crash is a two-second reboot.
- [ ] 4.6 Per-console display modes, savestate-and-poweroff suspend, 2s boot.

### Blind bring-up rules

1. Change exactly one thing per boot.
2. ArkOS stays bootable until the very end.
3. Every experimental kernel writes dmesg to FAT before anything else.
4. No working card reader means no recovery — confirm the reader before the
   first repartition. (Floor below that: RK3326 maskrom over USB with
   `rkdeveloptool`, since an SD-only device with no card enters maskrom.)

## Open questions

Phase 0 answers all of these; none should be guessed at before then.

- What kernel and DRM stack does ArkOS actually run?
- Is the panel already presented as landscape, or do we owe it a rotation?
- Is the gamepad USB HID (as mainline's RG351M notes suggest) or GPIO + ADC?
- Does the ethernet dongle work in host mode, or do we need `g_ether`?
