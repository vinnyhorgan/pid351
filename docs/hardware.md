# RG351P — hardware ground truth

What this specific unit actually is. **[dtb]** was read out of ArkOS's vendor
device tree; **[live]** came from `tools/recon.sh` run on the device. Both on
2026-08-18. Raw output is in `recon-arkos.txt`.

## Currently installed system

ArkOS, on a 29.2GB card in three partitions:

| Part | Size | FS | Label | Purpose |
|---|---|---|---|---|
| p1 | 112M | vfat | BOOT | kernel, DTBs, `boot.ini` |
| p2 | 14.2G | ext4 | root | the 14GB that started this project |
| p3 | 15G | exfat | EASYROMS | ROMs, and `tools/` |

- Kernel: **Linux 4.4.189**, Linaro GCC 6.3, built 2021-03-03. Rockchip vendor
  BSP, not mainline.
- Bootloader: Odroid Go 2 U-Boot, driven by a **`boot.ini` script we can edit**.
  It `load`s `Image`, `uInitrd` and a DTB from `mmc 1:1` and calls `booti`.
  Adding a second boot option is a text edit, not a bootloader replacement.
- DTB selected at runtime by hardware revision. **[live]** this unit resolves
  to the RG351P tree: `model = "Anbernic RG351P"`, `compatible =
  "rockchip,rk3326-rg351p-linux"`. So `rk3326-rg351p-linux.dtb` is the one to
  diff against mainline.
- Bootargs of note: `fbcon=rotate:3`, `console=/dev/ttyFIQ0`, `quiet splash`.
  There is no `console=tty0`, so kernel messages never reach the panel. For our
  own boot we want the opposite.

## Panel — the rotation question, answered

**[dtb]** `elida,kd35t133` on DSI (`dsi@ff450000`), 1 lane.

    hactive        320      hfront 130   hsync 4   hback 130   -> htotal 584
    vactive        480      vfront   2   vsync 1   vback   2   -> vtotal 485
    clock          17.000 MHz
    physical       42mm x 82mm

The panel is **natively portrait 320x480**. It is not landscape and never was;
ArkOS rotates the console in software with `fbcon=rotate:3`. So pid351 owes the
panel a 90-degree rotation on every frame, which is the RGA's job.

Refresh as configured: 17,000,000 / (584 x 485) = **60.0198 Hz**.

**[live]** `card0-DSI-1` is connected and enabled and advertises exactly one
mode: `320x480p60`. There is no landscape mode to select - the rotation is
entirely ours to do. `/dev/dri/card0` is `root:video` mode 0660.

For the per-console timing plan, GBA's 59.727 Hz needs a pixel clock of
59.727 x 283,240 = **16.917 MHz** with the porches unchanged. Adjusting vtotal
instead only gets to 59.774 Hz, so the clock is the knob. **[todo]** whether
the VOP's PLL can actually hit 16.917 MHz.

## RGA — present

**[dtb]** `rockchip,rga2` at `0xff480000`, `status = "okay"`, `dma-coherent`.

This is the **vendor RGA2**, driven through `/dev/rga` with Rockchip's own
ioctl interface — *not* mainline's `rockchip-rga` v4l2-m2m driver.

**[live]** `/dev/rga` exists (char 10,58, a misc device). `/sys/class/video4linux`
does not exist at all and there are no `/dev/video*` nodes, so there is no v4l2
path on this kernel whatsoever. Code written against it will not port to
mainline unchanged, which is why RGA stays behind a CPU fallback.

## Audio

**[live]** One ALSA card, `rockchip,rk817-codec`, playback device 0. So the
target is simply `hw:0,0` and can be hardcoded.

## Backlight

**[dtb]** `pwm-backlight`, 25,000 ns period (40 kHz), 256 brightness levels,
default 255. Fine-grained control, which matters because the backlight is
almost certainly the single largest consumer on this device.

**[live]** `/sys/class/backlight/backlight/`, `max_brightness` 255, and ArkOS
had it sitting at **63**. Worth knowing when comparing power numbers: any
measurement has to state the backlight level or it means nothing.

## Battery — better than expected

**[dtb]** `rk817,battery` fuel gauge on the PMIC I2C bus.

    design_capacity   3450 mAh
    design_qmax       3795
    sample_res        10   (10 milliohm shunt)
    power_off_thresd  3400 mV

**[live]** `current_now` works:

    voltage_now   3,912,000 uV
    current_now    -344,000 uA   (discharging)
    charge_full   3,450,000 uAh
    status        Discharging

**344 mA at 3.912 V = 1.35 W, sitting idle in a menu.** That is the number to
beat, and it can be read in one second instead of inferred from an overnight
voltage curve. Any benchmark must record backlight level and governor
alongside it.

## CPU / GPU frequencies — a finding worth acting on

**[dtb]** The vendor OPP tables:

    CPU   1008, 1200, 1248, 1296 MHz
    GPU   400, 480, 520 MHz

**[live]** `scaling_available_frequencies` confirms it exactly. Governor is
`interactive`, all four cores online, and it was observed sitting at 1,296,000
- the maximum - while idling in a menu. Available governors include
`powersave` and `schedutil`. `devfreq` exists for `dmc` and `ff400000.gpu`.

**The CPU floor is 1008 MHz.** There is no low OPP declared at all, so ArkOS
physically cannot clock below 1 GHz no matter what governor it uses — while
running emulators that need a fraction of that. The RK3326/PX30 silicon clocks
much lower and mainline's OPP table declares lower points, so our own device
tree can add them and win battery life that ArkOS is structurally incapable of.
**[todo]** confirm the lower OPPs are stable on this unit.

## USB — gadget mode is available

**[dtb]** `usb@ff300000` is `snps,dwc2` with **`dr_mode = "otg"`**, status okay.
The EHCI and OHCI controllers are both `disabled`, so this single OTG port is
all the USB there is.

**[live]** `/sys/class/udc/ff300000.usb` exists and reads `not attached`, so a
UDC is registered and gadget mode is possible in principle.

**But the gadget plan is dead, and this is the important finding of the
session.** The internal gamepad is a USB device on this same controller, at
`usb-ff300000.usb-1.2` - a hub at `1-1`, gamepad at port 2. dwc2 is a single
OTG controller and holds one role at a time. It is currently host, serving the
gamepad. Switching it to peripheral to run an ethernet gadget would disconnect
the gamepad.

So **networking must be a USB ethernet dongle in host mode**, which is exactly
the hardware already on the desk. Because the internal hub is what the gamepad
hangs off, a dongle in the external port becomes another device on that hub and
the two coexist. One cable for power *and* network is off the table.

**[live]** `ip link` shows only `lo`, and `lsmod` lists just six modules -
`exfat, dwc2, sch_fq_codel, ip_tables, x_tables, ipv6`. No `usbnet`, no
`cdc_ether`, no `r8152`, no `asix`. Whether those exist unloaded on the rootfs
is the open question blocking networking; `/lib/modules` on p2 answers it.

## Input — resolved: plain USB HID

**[dtb]** showed no `gpio-keys`, `adc-keys` or joypad node, because there is
nothing to describe: the gamepad announces itself over USB at enumeration time.

**[live]**

    N: Name="OpenSimHardware OSH PB Controller"
    I: Bus=0003 Vendor=1209 Product=3100 Version=0111
    P: Phys=usb-ff300000.usb-1.2/input0
    H: Handlers=sysrq kbd js0 event2

**`/dev/input/event2` is the gamepad.** Standard evdev, handled by usbhid, no
daemon and no ADC polling. It reports `EV=10001f` - keys, absolute axes,
relative axes and misc - and also registers as a keyboard, which is how ArkOS
does text entry with it.

The other two nodes are `event0` (`rk8xx_pwrkey`, the power button, on the PMIC)
and `event1` (headphone jack detect, a switch on the codec). Both are useful:
the power button drives suspend, and jack detect drives output routing.

## Toolchain compatibility — verified

**[live]** A statically linked aarch64 binary built with Debian 13's gcc 14.2
ran correctly on this 4.4.189 kernel. Phase 1 can be built on that assumption.

## The baseline to beat

**[live]** ArkOS idling in EmulationStation:

    RAM used        116 MB of 893 MB usable
    processes       122
    biggest RSS     emulationstation, 91 MB, 28.6% CPU
    power           344 mA / 1.35 W at backlight 63
    CPU             1296 MHz, interactive governor, 4 cores online

Running services include `NetworkManager`, `wpa_supplicant`, `systemd-resolved`,
`systemd-timesyncd` and `networkd-dispatcher` — on a device with no wifi
hardware at all.

## Getting a shell on ArkOS

All eleven USB ethernet drivers ship on the rootfs — `usbnet`, `cdc_ether`,
`cdc_ncm`, `r8152`, `asix`, `ax88179_178a`, `rndis_host`, `smsc95xx` and
friends — and `modules.alias` carries 4237 USB aliases, so a dongle autoloads
its driver on enumeration. Nothing needs installing. `ip link` showed only `lo`
during recon simply because nothing was plugged in.

sshd is installed with host keys already generated, but **is not enabled**.
ArkOS's own toggle is `/opt/system/Enable Remote Services.sh`, reachable from
EmulationStation's Options menu. Reading it:

- it bails out unless a default gateway exists, so plug the dongle in *first*
- it runs `systemctl start ssh` — the `systemctl enable` lines are commented
  out, so **this does not survive a reboot** and must be re-run each time,
  until we enable it properly ourselves over the first ssh session
- it prints the device's IP on screen, which is how we find it (there is no
  avahi, so no `.local` mDNS)
- it also starts samba and a filebrowser on port 80, neither of which we want

Users are `root` and `ark` (uid 1002). `ark` is in `sudo`, `video`, `input` and
`audio`, so **pid351 can open `/dev/dri/card0` and `/dev/input/event2` without
root**. Hostname is `rg351p`.

> ⚠️ EmulationStation will be holding DRM master. Phase 1 has to stop it before
> it can modeset — expect `systemctl stop emulationstation` or equivalent to be
> step zero of every device test.

---

# ROCKNIX — the system pid351 actually targets

ArkOS was replaced with **ROCKNIX 20260801** (`BUILD_BRANCH=next`), a
LibreELEC/JELOS-derived image on **mainline Linux 6.12.79**. Everything above
tagged **[dtb]**/**[live]** was ArkOS; everything below is **[rocknix]**, from
`probe-1.txt` through `probe-3.txt` produced by `tools/rocknix/probe.sh`.

Two partitions: `p1` 2G vfat `ROCKNIX` (kernel, DTBs, overlays), `p2` 27.2G
ext4 `STORAGE` (resized on first boot). No eMMC, so the card is still the whole
machine and a bad flash is never fatal.

`model` reads **"Anbernic RG351M"** — the image ships no `rg351p` DTB at all,
and U-Boot's ADC probe (`uboot.hwid_adc=a,v11`) selects
`rk3326-anbernic-rg351m.dtb` for this unit. `vendor/rocknix-rg351m.dts` is
therefore the tree this machine really runs.

## Why this distro, mechanically

- `extlinux.conf` carries **`console=tty0`**, so kernel messages reach the
  panel. That is our substitute for the UART header this board does not have.
- It also carries **`FDTOVERLAYS`**, and `/overlays/` lives on the **vfat**
  partition. Device tree changes are therefore a file copy and a one-line edit,
  with no root, no reflash, and a trivial revert. `extlinux.conf.pre-pid351` is
  kept beside it.
- **`/storage/.config/autostart/` is a directory**, walked as root by
  `/usr/bin/autostart` before EmulationStation starts. Not LibreELEC's single
  `autostart.sh` file — that is silently ignored here. Entries run
  synchronously and are followed by a `wait`, so anything slow must `setsid`
  itself out of the way.
- sshd is installed and its unit is wanted by `multi-user.target`, but
  `ssh.enabled=0` is the shipped default and the daemon script gates on it.
  `set_setting ssh.enabled 1` turns it on for good. Root password is
  ROCKNIX's default, `rocknix`.

## USB — host works, power did not

Mainline is the opposite of ArkOS here, and this is the finding of the session.

`usb@ff300000` gains **`usb-role-switch`** and
**`role-switch-default-mode = "host"`**, so dwc2 comes up as a host with no
coaxing. It enumerates the internal hub and the gamepad at boot:

    usb3    1d6b:0002  DWC OTG Controller
     3-1    05e3:0608  USB2.0 Hub          MxCh=4, Atr=e0 (self-powered)
     3-1.2  1209:3100  OSH PB Controller   Driver=usbhid

This confirms the ArkOS-era conclusion above from the other direction. The hub
is internal and powered from the system rail, which is why it enumerates even
with the port unpowered — and a hub's downstream port can never present itself
as a USB *device*, which is the real reason gadget mode on ArkOS brought up
`usb0` that no host ever saw. **Gadget mode over that socket is impossible by
construction, not by misconfiguration.**

What was broken was power alone:

    usb_midu     enabled   5000000 uV  users=1   RK817 BOOST, the 5V rail
    OTG_SWITCH   disabled          uV  users=0   the switch onto VBUS

Mainline's rk817 regulator driver registers `OTG_SWITCH` from its own table,
but the board tree gives it no node, so it has no phandle, nothing can claim it
as a supply, and it stays off forever. dwc2 looked and found nothing:

    dwc2 ff300000.usb: Looking up vbus-supply property in node /usb@ff300000 failed

`tools/rocknix/overlays/pid351-usb-vbus.dtbo` gives `OTG_SWITCH` a node and
hands it to dwc2 as `vbus-supply`. **Verified** in `probe-2.txt`:

    otg_switch   enabled   users=1
       ff300000.usb-vbus       1

Deliberately not `regulator-always-on`: dwc2 enables it on entering host mode
and drops it otherwise, so an idle console is not running a 5V boost for
nothing.

## USB — what is still not solved

`rockchip-usb2phy` reports **`Requested PHY is disabled`**, because
`usb2phy@100/otg-port` is `status = "disabled"` in both trees. dwc2 works
anyway, but the PHY driver manages neither VBUS nor the id/bvalid interrupts.
Untested; one overlay away if it ever matters.

Enabling `usb@ff340000`/`usb@ff350000` (`pid351-usb-host.dtbo`) **works** —
both bind and register buses 1 and 2 — but nothing is attached to either. Left
out of `FDTOVERLAYS`, since clocking two idle controllers buys nothing. The
`.dtbo` stays on the card.

The Kensington UH1400P dock **never attaches**, with 5V now live on the port
and a USB mass-storage stick behind it: no connect, no reset, no failed
enumeration, no over-current, on any bus. It has a captive USB-C cable and
announces a USB Billboard descriptor, i.e. it waits for CC negotiation — and
there is no `typec`, `fusb302`, `tcpm` or CC GPIO anywhere in either device
tree. **Networking is blocked on a passive USB-C-to-USB-A-female adapter and a
bus-powered device, not on software.**

### The CC pins are grounded — measured, not inferred

An Android phone connected to the console over a C-to-C cable reports
**"analog audio accessory attached"**. Android prints that on detecting
Audio Adapter Accessory Mode, which the Type-C spec defines as *both CC1 and
CC2 pulled to ground*. The phone measured the port for us: the console's
receptacle has **CC1 and CC2 tied low**.

That closes the question. A USB-C device attaches only after seeing a pull-up
(Rp, a resistor to VBUS) telling it a source is present. Ground is not Rp, so
every compliant C device correctly concludes nothing is attached — which is
precisely what the dock did with 5V live and a mass-storage device behind it.
It also explains why the console charges happily from a USB-A-to-C cable,
which puts 5V on VBUS irrespective of CC.

Rp is a resistor on the PCB and it is not there. **No device tree change,
kernel option or role switch can ever make this port present as a USB-C
source.** Anything with a USB-C plug is out, permanently.

Passive C-to-A adapters are unaffected: they ask the console to signal
nothing, and VBUS and D+/D- pass straight through. A USB-A device attaches
normally. The known-good chain, once such an adapter exists, is

    console -> [C plug / A socket adapter] -> bus-powered USB 2.0 hub
            -> [A plug / C socket adapter, which supplies the Rp] -> dock

with the second adapter being what finally makes the dock's AX88179A
reachable, since it emulates the legacy USB-A host the dock is waiting for.

**[predicted, untested]** A USB-C charger on a C-to-C cable should fail to
charge this console, since it will read Ra/Ra and refuse to source.

## Corrections to the ArkOS-era notes above

- **"mainline's OPP table declares lower points" is wrong.** ROCKNIX's
  `opp-table-0` is 1008 / 1296 / 1416 MHz and only 1008 and 1296 survive to
  `scaling_available_frequencies`. Both trees floor at **1008 MHz**. Adding
  sub-GHz OPPs means choosing voltages ourselves, not copying mainline's.
- GPU is a **single 560 MHz** OPP here, against ArkOS's 400/480/520.
- Governors are `ondemand powersave performance schedutil`, default `ondemand`,
  idling at 1008 MHz — better behaved than ArkOS pinning 1296.

## RGA — driver present, node absent

`/dev/rga` does not exist and `/sys/class/video4linux` holds only
`rockchip,px30-vpu-enc` and `-dec` (`/dev/video0`, `/dev/video1`). ROCKNIX's
DTB keeps `qos_rga_rd`/`qos_rga_wr` but **has no RGA device node**, so nothing
probes.

The driver itself **is compiled into the kernel** — the image contains
`rockchip,rk3288-rga`, `rockchip,rk3399-rga`, `Cannot enable rga aclk: %d` and
`Failed to map video buffer to RGA`. ArkOS's tree puts the hardware at
`0xff480000` (`rockchip,rga2`). So hardware scale + rotate is reachable with an
overlay declaring `rga@ff480000` with a mainline-matching compatible and
mainline clock-names. **[todo]** build it; this is the single biggest Phase 1
lever, because the panel needs a 90-degree rotation on every frame.

## Panel, audio, battery, input on mainline

- Panel unchanged: `card0-DSI-1`, connected, advertising **320x480** only.
  Still portrait, still ours to rotate.
- Audio: one card, `rk817int`, `ff070000.i2s-rk817-hifi`, playback and capture
  on `pcmC0D0p`/`pcmC0D0c`. `hw:0,0` as before.
- Battery: `charge_full_design` reads **3500000** here against ArkOS's
  3450000. Idling in EmulationStation at backlight **127/255**:
  `current_avg -541456 uA` at `voltage_avg 3843550 uV` = **2.08 W**. Not
  comparable with the 1.35 W above, which was at backlight 63 — which is
  exactly why every power figure has to carry its backlight level.
- Input: same `OpenSimHardware OSH PB Controller`, but the event numbering
  moved — keyboard `event3`, mouse `event4`, pad `event5`/`js0`, because the
  vibrator, power key and jack detect now take `event0`-`event2`.
  **Never hardcode an event number.** Match on the `1209:3100` VID/PID or the
  device name, and pick the node that reports absolute axes.

## Phase 1, confirmed on hardware

`plat_drm.c` ran on the panel on 2026-08-18. Raw DRM ioctls, two RGB565 dumb
buffers, page flip on vblank, evdev input. Log in `first-light-1.log`.

    pid351: connector 40, crtc 36, mode 320x480@60
    pid351: pad on /dev/input/event5 (OpenSimHardware OSH PB Controller)
    pid351: display up, rotating counter-clockwise
    pid351: exit (combo) after 2063 frames

- **Rotation is counter-clockwise.** Clockwise put the red top-left marker in
  the bottom-right and the green top-right marker in the bottom-left - both
  exactly 180 degrees out, green still wide rather than tall, so a pure
  rotation error with no mirroring.
- **The mode is exactly 320x480** and the one-pixel border reaches all four
  edges, so nothing crops or overscans and the 2x GBA blit lands pixel
  perfect. A dark band along one edge in a photograph is the glass margin,
  not the framebuffer.
- **2063 frames in 90 seconds** with a clean exit: the flip loop is stable and
  never stalled.
- **The pad is `/dev/input/event5`**, found by USB id plus having absolute
  axes. A hardcoded event number would have picked the keyboard node.
- **The d-pad is `ABS_HAT0X`/`ABS_HAT0Y`**, diagonals included; all eight
  directions observed.
- **`BTN_SELECT` (0x13a) and `BTN_START` (0x13b) are correct.**

### The full button map, measured

Derived by pressing every button in a known order with `PAD_TRACE` on
(`first-light-2.log`). The pad emits a plain sequential HID button order from
0x130, and **the kernel's names for that range do not describe this shell**:

| Shell   | Code  | Kernel name  |
|---------|-------|--------------|
| A       | 0x130 | `BTN_A`      |
| B       | 0x131 | `BTN_B`      |
| X       | 0x132 | `BTN_C`      |
| Y       | 0x133 | `BTN_X`      |
| L1      | 0x134 | `BTN_Y`      |
| R1      | 0x135 | `BTN_Z`      |
| SELECT  | 0x136 | `BTN_TL`     |
| START   | 0x137 | `BTN_TR`     |
| L3      | 0x138 | `BTN_TL2`    |
| R3      | 0x139 | `BTN_TR2`    |
| L2      | 0x13a | `BTN_SELECT` |
| R2      | 0x13b | `BTN_START`  |

L2 is `BTN_SELECT` and SELECT is `BTN_TL`, so **mapping this pad by kernel
name is guaranteed to be wrong**. Map by number.

Note that **0x136 is START and 0x137 is SELECT** - the pad reports them in the
opposite order to how they sit on the shell, where SELECT is left of the
screen and START is right. Both stick clicks work and are reported.

### Analog sticks

Two 12-bit axes per stick, range **0 to 4095**, and the two sticks do not
agree about sign:

| Stick | Kernel axis | 0 is        | 4095 is    |
|-------|-------------|-------------|------------|
| Left  | `ABS_Z`     | full right  | full left  |
| Left  | `ABS_RX`    | full down   | full up    |
| Right | `ABS_RY`    | full left   | full right |
| Right | `ABS_RZ`    | full up     | full down  |

So the **left stick is inverted on both axes** relative to the right, and the
right stick follows the usual evdev convention of down and right being
positive.

The left stick now stands in for the d-pad, resolved inside `plat_drm.c` with
a 45% deadzone so that nothing above the platform layer learns this machine
has a stick at all - the cores must not, since none of the consoles we target has
one. The inversion is why that mapping is measured rather than assumed: taking
the obvious convention would have produced a stick that moves the wrong way in
all four directions.

`ABS_THROTTLE` is advertised by the pad and never moves. Presumably a field in
the HID report with nothing behind it.

## The baseline pid351 has to beat, measured

pid351's demo running on the panel, before EmulationStation starts:

    power       400 mA at 3.81 V = 1.52 W   (backlight 127/255)
    cpu         1296 MHz, performance governor
    gpu         560 MHz
    temps       ~50 C both zones
    memory      176 MB of 981 MB
    frame rate  59.72 fps measured against a 59.727 Hz target
    frames      11929 in ~200 seconds, no stalls

Against **2.08 W** for ROCKNIX sitting in EmulationStation at the same
backlight: **a 27% saving before a single power optimisation**, purely from
not running a frontend. And measured with the CPU pinned at its 1296 MHz
maximum under the `performance` governor, so the governor and OPP work in
phase 3 is all still on the table.

The frame pacing is confirmed: 59.72 measured against 59.727 asked for, over
twelve thousand frames.

## Performance, measured

One run, 2026-08-18, everything below from `docs/first-light-4.log`.

### The scanout buffer is write-combined

300KB sequential, best of five, microseconds:

| | write | read | read-modify-write |
|---|---|---|---|
| DRM dumb buffer | 171 | 4542 | 4800 |
| ordinary memory | 157 | 238 | 282 |

Writes stream at the speed of RAM. **Reads are 19x slower.** This is the fact
the whole blit argument was resting on and it had been assumed in both
directions - it decides the winner below, and it means nothing may ever read
back from the scanout buffer in a frame loop.

### Rotate blit, microseconds, min/median at 200 iterations

Every console fills the panel, so the destination is always 153600 pixels.

| source | STRIDED | TILED | STAGED |
|---|---|---|---|
| GBA 240x160 | 1489/1517 | 2219/2334 | **1250/1267** |
| NES 256x240 | 2622/2671 | 2356/2549 | **1322/1409** |
| SNES 256x224 | 2506/2565 | 2325/2463 | **1313/1363** |
| Genesis 320x224 | 1843/1913 | 2466/2632 | **1350/1455** |
| native 480x320 | 2374/2660 | 3815/3865 | **2124/2340** |
| LINEAR control | | | 683/725 |

STAGED wins everywhere and TILED loses almost everywhere, exactly as
write-combined memory predicts: TILED's scattered partial-line writes are the
pathological case for it.

Tile size at 480x320: STAGED goes 2967 / 2348 / 2118 / **1557** at 8 / 16 / 32
/ 64, still improving at the largest tile tested. Longer contiguous runs fill
the write-combining buffers more completely, so the next thing to try is a
rectangular tile - full panel width by 32 rows - which would give whole-row
writes and cache-line-exact reads at once. **[todo]**

### What a millisecond of CPU costs

`current_avg` turned out to be the better instrument, not the coulomb counter.
Three phases run under identical conditions spread 12.7 mA on `current_avg`
against 21.4 mA on charge differencing: the counter only ticks every six or
seven seconds, so a 29 second window holds four or five quantisation steps and
that dominates. **The noise floor is +/- 7 mA, and nothing below about 20 mA
is a result.**

Against a 387.7 mA baseline, adding synthetic blits:

- +5.47 ms/frame -> +37.2 mA (6.79 mA per ms per frame)
- +10.88 ms/frame -> +82.7 mA (7.61 mA per ms per frame)

**About 7 mA per millisecond of Cortex-A35 time per frame**, at 1296 MHz. One
core saturated would be roughly 120 mA. This is the exchange rate that turns
any timing into a battery argument, and until now every such argument in this
project was made without it.

### The levers, ranked

| | saving | of 387.7 mA |
|---|---|---|
| backlight 255 -> 32 | 42.0 mA | 10.8% |
| 1296 -> 1008 MHz at idle load | 21.5 mA | 5.5% |
| STRIDED -> STAGED blit (NES) | 9.4 mA | 2.4% |
| deleting the blit entirely (NES) | 18.9 mA | 4.9% |

Two things this overturns. **The backlight is not the dominant consumer** - it
had been called that three times in this project and it is about a tenth,
across its whole range. And **the entire rotate blit is worth under 5%**, so
RGA's absolute ceiling is under 5% for a device tree node, a driver-compatible
gamble and archaeology for parameters we no longer have. STAGED already takes
half of that for free. **RGA is closed.**

1008 MHz held 59.70 fps with the blit at 3362 us against 2863 at 1296, so the
lower operating point costs nothing this workload can feel and is the largest
lever we actually control.

### Panel timing

`clock=17000 kHz htotal=584 vtotal=485` gives **60.019 Hz** by arithmetic and
**60.109 Hz** measured over 300 flips. We pace to a hardcoded 59.727, so the
panel is 0.64% faster than we present and a frame is shown twice about every
2.6 seconds. For GBA at 59.7275 Hz the fix is the pixel clock: 59.727 x 584 x
485 = **16917 kHz**. Adjusting vtotal instead only reaches 59.78 Hz.

## Extracted from the card, no run required

`docs/dt/rocknix-rg351m.dts` and `docs/dt/rocknix-kernel.config.gz`, taken off
the vfat partition on the laptop. This is most of what phase 4 needed and none
of it cost a boot.

- **Panel is `elida,kd35t133`**, `CONFIG_DRM_PANEL_ELIDA_KD35T133=y`. Its mode
  is hardcoded in the driver, not in DT, so retuning the pixel clock on our own
  image means patching one struct - and we do not need to, see below.
- **`rotation = <270>` is already in the panel node.** That is
  `panel_orientation`, a hint for userspace, not a transform anything performs.
  It independently confirms the direction we settled by looking at the screen.
- VOP is `rockchip,px30-vop-big` at `0xff460000`.
- **CPU OPPs with voltages:** 1008 MHz at **1.175 V**, 1296 and 1416 both at
  **1.35 V**. Those are the anchor points any sub-GHz OPP we author has to be
  extrapolated from, and they were the missing piece.
- **Backlight** is `pwm-backlight`, 25 us period (40 kHz), `brightness-levels`
  a plain linear 0-255 ramp, default 128. So our 255/32 measurement spans 87%
  of the range and the full backlight is about **48 mA**, a little over a
  tenth of the total.
- **cpuidle is real:** `enable-method = "psci"`, a `cpu-sleep` idle state on
  every core, `CONFIG_ARM_PSCI_CPUIDLE=y`. Idle cores are not merely spinning
  in WFI, so offlining the other three would be chasing something already
  handled - and any gain would sit under the 20 mA noise floor.
- `CONFIG_VIDEO_ROCKCHIP_RGA=y` confirms the driver was always there and only
  the node was missing. Moot now that RGA is closed on the numbers.
- Partitions are 2 GB vfat plus 27.2 GB ext4, so phase 4.3 has room to put our
  boot files beside ROCKNIX's.
- `extlinux.conf` asks for `/overlays/mipi-panel.dtbo`, which **does not exist
  on the card**. That is ROCKNIX's own line, not ours, and U-Boot evidently
  skips a missing overlay without complaint. The panel comes entirely from the
  base DTB.

### Inputs, all six nodes

`event0` pwm-vibrator, `event1` rk805 pwrkey, `event2` rk817 headphone detect,
`event3`/`event4` the pad's keyboard and mouse interfaces, `event5` the pad
itself. **The volume buttons are not on event5**, so they are almost certainly
on the keyboard interface and are currently unmapped. That only matters once
there is audio, so it belongs with that work.

### The refresh mismatch does not need a clock retune

The panel runs at **exactly 60.0186 Hz** - cpll is 408 MHz, the VOP divides by
24 for a 17.000000 MHz pixel clock, and 584x485 totals do the rest with no
rounding anywhere. The two flip-rate measurements, 60.109 and 60.050 Hz,
bracket that within their own start/stop alignment error rather than
contradicting it. We were pacing to 59.727, 0.49% slow. Rather than
retune the pixel clock to 16917 kHz - which needs the VOP PLL to be able to
produce it, and nobody knows whether it can - **pace to the panel**. Running
GBA 0.64% fast is a pitch shift of about a tenth of a semitone's ninetieth,
which is inaudible, against a duplicated frame every 2.6 seconds, which is not
invisible. Same argument already applied to NES and SNES, whose rates never
matched anything. So the display clock is the master and audio is resampled to
it, and the PLL question never has to be asked.

## Fifth run: what the lean image actually needs, and what it will not buy

`docs/first-light-5.log` and `docs/probe-4-drivers.txt`. This was the last boot
before the image, so the point was to settle every open question that did not
need audio.

### The VOP cannot rotate. Verified, not argued.

Every property on every plane was dumped. The connector carries `panel
orientation = 2 [Left Side Up]`, which is informational - it tells a compositor
what to do, and nothing more. The three planes carry **only** `type` and
`IN_FORMATS`. There is no `rotation` property anywhere on the pipeline, so
there is no rotation to enable. The CPU rotate is not a workaround for a
feature we failed to find; it is the only mechanism that exists.

Plane 32 primary, 34 cursor, 37 overlay, all on crtc 36, all offering `RG16`.
So the double buffer and the 16-bit format were already the right calls.

### The GPU costs us nothing, and the removal test was the wrong test

`delete_module("panfrost")` failed both times with ENOENT because **ROCKNIX
does not use panfrost**. It uses the ARM vendor blob, `mali_kbase`, bound as
`mali <- ff400000.gpu`. So the `no-gpu` phase is a duplicate of `base`, which
turns out to be useful as a third identical-condition control.

The question it was meant to answer is answered better elsewhere. The genpd
summary reads:

    gpu   off-0        ff400000.gpu   suspended
    vpu   off-0        ff442000.video-codec suspended
    vi    off-0        ff4a8000.iommu suspended
    vo    on           vop, dsi, phy, iommu all active

The GPU power domain is **off** while we run, and the driver logs a WARN from
`disable_gpu_power_control` at 17.4 s doing exactly that. There is no GPU cost
to recover. My 5-15 mA pessimistic allowance was wrong in the safe direction
and should be struck: it is zero. Same for the VPU and for the RGA's iommu
domain - all three are gated off, all three are free.

Incidentally there is no `rockchip-rga` device on the platform bus at all, so
the missing DT node was the whole story. RGA stays closed.

### ROCKNIX's daemons cost less than we can measure

Four phases, and the two instruments disagreed in *direction*, so one of them
had to be broken:

| phase | charge-derived | `current_avg` |
|---|---|---|
| base | 448448 | 380808 |
| no-gpu (= base) | 427093 | 374788 |
| no-daemons (22 stopped) | 341674 | 403512 |
| restored | 448446 | 387000 |

The coulomb counter is the broken one, and the arithmetic says so exactly. Each
29 s window contained **4 counter steps**, so the quantum is ~880 uAh. The
deltas were 3612, 3440, **2752**, 3612 uAh - which is 4, 4, **3**, 4 steps. The
`no-daemons` window simply closed one update early. The apparent 107 mA saving
is precisely one quantum, one quarter of the reading. It is an artifact, and
the near-perfect base/restored agreement (2 uA apart) was luck, not precision.

That leaves `current_avg`. Three identical-condition phases read 380808,
374788 and 387000: a 12.2 mA spread around 380.9, consistent with the +-7 mA
floor established last run. `no-daemons` read 403512, i.e. **22.6 mA higher** -
stopping the daemons did not help, and may have hurt (22 SIGSTOPped processes
leave systemd, dbus and journald retrying against them).

So the honest conclusion, and it is not the one I expected:

> **At the moment our program runs, ROCKNIX's userspace costs less than our
> noise floor.** Everything expensive - sway, swaybg, mako, EmulationStation,
> the GPU compositor - has not started yet. The 553 mA we measured for a full
> ROCKNIX session is real, but it is never concurrent with us.

Our own image therefore buys **approximately zero milliamps** over "ROCKNIX
plus our binary on autostart". The earlier 20 mA allowance is withdrawn. What
the image actually buys is boot time, determinism, the freedom to set the
governor and idle policy without fighting anyone, a kernel we can strip, and
the project itself. Those are good reasons. Battery is not one of them, and it
would have been dishonest to keep implying otherwise.

Revised projection for the finished thing, against 380.9 mA today:

| lever | saving | how it is known |
|---|---|---|
| 1296 -> 1008 MHz | 21.5 mA | measured, and free: 59.70 fps held |
| STRIDED -> the chosen blit | ~9.5 mA | 2599 -> 1245 us at NES, at 7 mA/ms |
| deleting ROCKNIX | 0 mA | measured this run |
| **total** | **~31 mA** | **~350 mA, an 8% improvement** |

The kernel's own energy model independently confirms the frequency call:
`cpu cpu0: EM: OPP:1296000 is inefficient`. 1296 and 1416 MHz share the same
1.35 V rail, so 1296 is dominated by 1416 on every metric. **Use 1008, or jump
straight to 1416. Never 1296** - which is exactly what ROCKNIX boots at.

### The biggest wakeup source is the gamepad's USB bus

`/proc/interrupts` at ~35 s uptime, sorted by what matters:

    63:  234651   ff300000.usb, dwc2_hsotg:usb1
    11:    7965   arch_timer (CPU0)
    56:    3280   dw-mci
    27:    4337   ff3a0000.spi
    28:    1950   ff180000.i2c
    25:    1249   vop + its iommu

dwc2 fires **~6700 times a second**, fifty times the timer tick and 190 times
the VOP. The topology explains it: the pad is a *full-speed* HID device behind
a *high-speed* internal hub (`1-1.2: 1209:3100` at 12 Mb/s, under `1-1:
05e3:0608` at 480 Mb/s). dwc2 schedules split transactions for full-speed
periodic endpoints off the start-of-frame interrupt, one per 125 us microframe.
8000/s is the ceiling and we are at 6700.

At ~2-4 us of handler that is 16-32 ms per second, or **2-4 mA** at our
exchange rate - small, but it is the single thing that keeps CPU0 from sitting
still, and it is unavoidable because the pad genuinely is a USB device. Worth
knowing before blaming anything else for a jittery idle. Not worth chasing now.

Idle itself is working: cpu0 logged 38688 `cluster-sleep` entries and 95149
WFI in 35 s, and cpus 1-3 spend most of their time in `cluster-sleep`. Nothing
to fix there.

### Thermals are a non-issue

46.8 C soc, 46.4 C gpu, first passive trip at 70 C, critical at 115 C. We are
23 degrees below the first intervention while running the display flat out.
Nothing in the power plan needs a thermal caveat.

### Storage ceiling

`mmc0` negotiated SD high-speed, 4-bit, 50 MHz - not UHS, not SDR104 - and
measured **22.2 MB/s** sequential with caches dropped, on a 29.2 GiB card. That
is the number to design ROM loading and any save-state size against.

### U-Boot: there is a recovery path, but not the one I wanted

    U-Boot 2017.09 (Aug 01 2026)
    board string: Rockchip RK3326 ODROID-GO Advanced
    commands present: rockusb, bootm, booti, sysboot, setexpr
    commands absent:  ums

No `ums`, so U-Boot cannot expose the card as a USB mass-storage device - the
"fix a broken image without pulling the SD" trick is out. `rockusb` is present,
which is the Rockchip loader protocol, but reaching it needs either a console
we do not have or the maskrom button. **So the recovery plan stays physical:
keep ROCKNIX's partitions intact and add ours beside them, and never overwrite
the only known-good boot path.**

The kernel command line shows U-Boot already does board detection for us:
`uboot.hwid_adc=a,v11`. It reads the ADC and passes the result, so our own
extlinux entry inherits that without doing anything.

### The driver list our kernel has to reproduce

This is the census the lean config is built from - everything that actually
bound to hardware, with the noise stripped:

- **CPU/power**: `psci-cpuidle`, `psci-cpuidle-domain`, `cpufreq-dt`,
  `rockchip-pm-domain`, `rockchip-iodomain` (x2), `armv8-pmu`,
  `rockchip-cpuinfo`, `syscon-reboot-mode`
- **PMIC**: `rk8xx-i2c` on i2c 0-0020, then `rk808-regulator`, `rk808-rtc`,
  `rk808-clkout`, `rk805-pwrkey`, `rk817-charger`, `rk817-codec`
- **Display**: `rockchip-drm` (display-subsystem), `rockchip-vop`,
  `dw-mipi-dsi-rockchip`, `inno-dsidphy`, `panel-elida-kd35t133`,
  `pwm-backlight`, `rk_iommu` x3
- **Storage**: `dwmmc_rockchip` + `mmcblk`, `rockchip-sfc` + `spi-nor`
- **Input/USB**: `dwc2`, `hub`, `usbhid`, `hid-generic`
- **Audio**: `rockchip-i2s`, `asoc-simple-card`, `rk817-codec`, `snd-soc-dummy`
- **Misc**: `rockchip-pinctrl`, `rockchip-gpio` x4, `rk3x-i2c` x2,
  `rockchip-pwm` x3, `rockchip-saradc`, `rockchip-thermal`, `rockchip-otp`,
  `leds-gpio`, `leds_pwm`, `pwm-vibrator`, `dw-apb-uart` x2

Everything else in ROCKNIX's config supports the other thirty-nine handhelds.
Droppable outright: `mali_kbase`, `hantro_vpu` and its four v4l2 helpers,
`ntfs3`, `exfat`, `snd_seq`, `algif_aead`, `nfnetlink`, and the whole network
stack - `NetworkManager`, `iwd`, `avahi`, `sshd`, `resolved`, `timesyncd`,
`hwdb`. The blame list shows those cost 6+ seconds of boot between them.

Two more image-level notes from the boot log:

- **CMA is 64 MiB.** We need two 480x320x2 dumb buffers, about 1.2 MB. `cma=8M`
  hands 56 MB back to the emulator.
- The DT has **no `reserved-memory` node**, so nothing else is claiming RAM.

### What is still open

Audio only. The volume keys are still unmapped and headphone detect on
`event2` is still untouched, both deferred deliberately. Everything else that
needs hardware in front of it has now been measured.

## Building our own kernel: ROCKNIX was hiding two OPPs

Mainline 6.12.103 carries `rk3326-anbernic-rg351m.dts` upstream, and it is the
right board: `elida,kd35t133`, `rotation = <270>`, same reset GPIO, same
supplies as the DTB on the card. The compiled DTB is 44735 bytes against
ROCKNIX's 65105 - 31% smaller for the same hardware.

The interesting difference is the CPU OPP table:

| frequency | mainline | ROCKNIX |
|---|---|---|
| 600 MHz | **0.950 V**, `opp-suspend` | deleted |
| 816 MHz | **1.050 V** | deleted |
| 1008 MHz | 1.175 V | present |
| 1200 MHz | 1.300 V | deleted |
| 1296 MHz | 1.350 V | present |
| 1416 MHz | absent | added (same 1.350 V) |

**ROCKNIX strips every OPP below 1008 MHz** - they are shipping an emulation
frontend and want headroom, so the low end is dead weight to them. It is not
dead weight to us. Dynamic power goes as f*V^2, so against 1008 MHz:

- 816 MHz at 1.05 V: `(816 x 1.05^2) / (1008 x 1.175^2)` = **0.65**
- 600 MHz at 0.95 V: `(600 x 0.95^2) / (1008 x 1.175^2)` = **0.39**

And the 21.5 mA we measured going 1296 -> 1008 was mostly a *static* saving,
because `vdd_arm` drops with the OPP. 1008 -> 816 cuts that rail another 11%,
which should be worth a similar amount again on the floor. **This is a lever
that did not exist while we were running someone else's device tree**, and it
is the first time building our own kernel has a measurable payoff attached
rather than an aesthetic one.

Whether a GBA core fits inside a frame at 816 MHz is the open question, and it
is measurable the moment there is a core. The frontend and the lighter systems
almost certainly fit at 600.

Note mainline has no 1416 MHz. ROCKNIX added it at the same 1.350 V as 1296,
so it is proven on this silicon and we can add it back if we ever need it -
but the kernel's own energy model already told us 1296 is dominated, so it
would *replace* 1296, never sit beside it.

## Audio needs no oracle after all

The whole topology is in the device tree, and the controls are in the driver:

    simple-audio-card,widgets  = "Microphone","Mic Jack",
                                 "Headphone","Headphones", "Speaker","Speaker"
    simple-audio-card,routing  = "MICL","Mic Jack", "Headphones","HPOL",
                                 "Headphones","HPOR", "Speaker","SPKO"
    simple-audio-card,hp-det-gpio = <&gpio0 22 0>    format = i2s, mclk-fs 256

`sound/soc/codecs/rk817_codec.c` exposes exactly two controls that matter:

- `"Master Playback Volume"` - `SOC_DOUBLE_R_RANGE_TLV` on `DDAC_VOLL/VOLR`
- `"Playback Mux"` - a two-entry enum, `"HP"` / `"SPK"`

Everything else in the codec is a DAPM supply, which the framework powers up
on its own when the PCM opens and the route is active. **There is nothing to
unmute by hand.** Bring-up is: set the mux from the headphone-detect GPIO, set
the volume, open `/dev/snd/pcmC0D0p`, write. Both can be driven through the
ALSA ioctls directly, the same way we drive DRM, so alsa-lib is not a
dependency either.

I had argued we needed one more ROCKNIX boot to learn this at runtime. We did
not - it was all in the source, and going to the source is the better answer.

## First boot of our own kernel

It came up. U-Boot -> our extlinux entry -> mainline 6.12.103 -> our binary as
PID 1, display, pad, a full measurement run, exit on the button combo, log
written to the FAT partition, power off. Every link in that chain worked the
first time.

(The raw log was lost: the installer clears `pid351-boot.log` so the next
boot's is unambiguous, and it was run before the file was archived. The
installer now moves logs into `docs/logs/` instead of deleting them. The
numbers below were read out of it before that happened.)

### The refresh question is closed, exactly

    mode clock=17000 kHz htotal=584 vtotal=485 -> exact 60.019 Hz
    vblank measured over 300 flips: 60.018 Hz

**One millihertz apart.** Two earlier runs measured 60.109 and 60.050 and I
argued those were start/stop alignment error against an exact arithmetic
result rather than evidence of a different rate. That is now settled: the
panel runs at 60.0186 Hz and nothing else. Pacing to 16661 us instead of the
old 16743 moved the loop from **59.72 fps to 60.01**, which is the frame we
were quietly dropping every 20 seconds.

### Low frequencies are cheaper than they look

The sweep pinned each operating point through `scaling_min/max_freq` and the
frequency read back exactly every time, so the pinning worked:

| OPP | blit median | work median | fps |
|---|---|---|---|
| 1296 MHz | 1665 us | 2666 us | 59.99 |
| 1200 MHz | 1726 us | 2831 us | 59.99 |
| 1008 MHz | 1962 us | 3226 us | 59.99 |
| 816 MHz | 2314 us | 3777 us | 59.99 |
| 600 MHz | 2951 us | 4790 us | **60.00** |

Two things fall out of this, and neither was predictable from the clock alone.

**The blit does not scale with the clock.** 600 MHz is 2.16x slower than 1296,
but the blit only takes 1.77x longer. It is bound by write-combined stores to
scanout, not by the core, so dropping the clock costs less than proportionally
- exactly the shape you want when the frequency is also buying you a voltage
cut. Anything memory-bound gets cheaper at low clocks in relative terms.

**60 fps holds at 600 MHz.** 4790 us of a 16661 us frame, with the panel still
locked. That is 3.5x headroom at the lowest and cheapest operating point on
the machine, and it makes 600 MHz a serious candidate for the frontend and the
lighter systems rather than a curiosity.

The blit itself also got slightly faster on our kernel than on ROCKNIX -
STAGED at 480x320 went 1512 -> 1464 us - which is consistent with a machine
that has fewer things interrupting it.

### What did not work, and why

**No fuel gauge at all.** Every power column read -1: no `current_avg`, no
`voltage_avg`, no charge counter. `CONFIG_CHARGER_RK817=y` was set and the MFD
registers the cell unconditionally, but mainline's `rk3326-anbernic-rg351m.dtsi`
**has no battery node**, so `rk817_charger` bails out of probe on
`power_supply_get_battery_info` and never registers a power supply. The whole
point of the sweep produced no power numbers.

Fixed in `image/pid351-rg351p.dts`: a `simple-battery` node and a `charger`
subnode carrying `monitored-battery`. Upstream's odroid-go2/go3 do exactly
this and the RG351 shares their cell chemistry - the OCV table and the charger
tuning are identical in upstream and in ROCKNIX's vendor tree, and only the
pack capacity differs (3500 mAh here against the OGA's 3000). The pack numbers
also match what we read out of sysfs on this device months before we built a
kernel, so nothing here is guessed.

**`Warning: unable to open an initial console`.** The initramfs had no
`/dev/console`, and the kernel opens it for init's stdio before any of our
code runs - devtmpfs is not mounted yet at that point. Everything we printed
before the `/dev/kmsg` redirect went nowhere. Fixed with one line in the
initramfs file list: `nod /dev/console 0600 0 0 c 5 1`.

**`g_mass_storage` failed to bind with -22.** Harmless noise from
`arm64 defconfig`, now off. Worth remembering though: a mass storage gadget is
precisely the `ums` capability the stock U-Boot lacks, so pid351 could one day
offer the SD card to the laptop over the same cable that charges it. That
would end card swapping without touching the bootloader at all.

**The RTC has no valid time** - `rk808-rtc` set the clock to 2017-08-06, which
is why files written on the device carry that date. There is no backup cell.
Nothing depends on wall-clock time yet; savestates eventually will.

Not a bug, though it looks like one: the `gov=schedutil` in every conditions
line is correct. The sweep selects `performance` inside the block and restores
the original governor afterwards, and all three of those lines are printed
outside it.

## The operating point sweep, measured

`docs/logs/4-pid351-boot.log`. Half brightness (833 of a max_brightness of
1667, which is 49.97% duty and therefore directly comparable to ROCKNIX's
127 of 255), test card workload, each point pinned by writing both
`scaling_min_freq` and `scaling_max_freq` and confirmed by reading the
frequency back in every phase.

| OPP | current_avg | vs 1296 | work median | fps |
|---|---|---|---|---|
| 1296 MHz | 377.9 mA | - | 2740 us | 59.99 |
| 1200 MHz | 367.0 mA | -5.2 | 2893 us | 59.99 |
| 1008 MHz | **318.0 mA** | **-54.2** | 3250 us | 59.99 |
| 816 MHz | **296.9 mA** | **-75.3** | 3811 us | 59.99 |
| 600 MHz | **283.6 mA** | **-88.6** | 4845 us | 60.00 |
| 1296 again | 366.5 mA | - | 2748 us | 59.99 |

The two 1296 phases bracket the sweep 11.4 mA apart, which is the drift floor
for this run and the yardstick for everything in it. So:

- 1296 -> 1200 is **not resolved**. 5.2 mA is inside the noise.
- 1296 -> 1008 saves **54 mA**, comfortably outside it.
- 1008 -> 816 saves another **21 mA**.
- 816 -> 600 saves **13 mA**, which is marginal but survives.

**60 fps holds at every one of them**, including 600 MHz at 4845 us of a
16661 us frame.

### The 21.5 mA figure this file carried for 1296 -> 1008 was wrong

It is 54 mA, two and a half times larger. The earlier number came from a run
that selected an operating point through the governor on ROCKNIX rather than
pinning both frequency bounds, so there is no evidence the core actually held
1008 for the whole window - and this sweep verifies the frequency by reading
it back in every phase and bounds its own drift by measuring 1296 at both
ends. The new number supersedes the old one.

Every battery projection in this file that was built on 21.5 mA understated
the OPP lever by roughly half. They are replaced below.

### The idle floor

    before bench  cpu_khz=600000 gov=schedutil ... curr_ua=-219472

**219 mA** with the panel lit at half brightness, schedutil at the bottom of
the table, and nothing but our process on the machine. That is the floor
everything else is measured against, and it is 59% of what the test card
costs at 1296 MHz.

### What it means for playing games

Using the measured floors (total minus work times the rate at that point) and
the measured sub-linear blit scaling, `t ∝ f^-0.74`, against an mGBA-class
core taken at 8 ms per frame at 1296 MHz:

| config | frame used | draw | hours at half brightness |
|---|---|---|---|
| GBA @ 1296 | 9.4 / 16.7 ms | ~419 mA | 7.3 - 7.8 |
| GBA @ 1008 | 11.9 ms | ~354 mA | 8.6 - 9.2 |
| **GBA @ 816** | 14.6 ms | **~326 mA** | **9.4 - 10.0** |
| GBA @ 600 | 19.6 ms, does not fit | - | - |
| NES/SNES @ 600 | 8.9 ms | ~290 mA | 10.5 - 11.2 |
| menu, idle @ 600 | ~1 ms | ~277 mA | 11.0 - 11.7 |

Usable capacity taken as 3050-3250 mAh against the gauge's learned
`charge_full` of 3380.

Two conclusions worth acting on. **816 MHz is the GBA target**: 600 cannot
hold the frame (the core alone wants 17.3 ms of 16.7) and 1008 costs 28 mA
for headroom we do not need. And **the spread between the worst and best
configuration is about three and a half hours**, which is a far larger prize
than anything left anywhere else in this project - the entire blit rewrite
was worth 9 mA.

The charge-implied column was noise again, as expected: 427, 299, 278, 342,
320, 320 mA against current_avg's clean monotonic 378, 367, 318, 297, 284.
Four counter steps in a 29 second window cannot resolve this. `current_avg`
is the instrument; the coulomb counter is not.

## Audio, built on the laptop — what is settled and what is not

Written away from the device. Everything below marked **[laptop]** was
measured on an HP `sof-hda-dsp` card, everything marked **[source]** was read
out of the 6.12.103 tree we actually build, and nothing has run on the
handheld yet.

### There is nothing to abstract, so there is one implementation

The laptop and the handheld are both Linux with ALSA, so unlike the display
there is no per-target audio backend. `aud_alsa.c` is in `COMMON` in the
Makefile and compiles into both binaries — the ioctls tested on the laptop are
byte for byte the ones that will run on the device. That makes the laptop a
real test rig for this subsystem rather than a simulation of one, which is not
true of anything else in the project.

Raw ioctls, no alsa-lib, for the same reason the display talks to DRM
directly. The whole path is seven: `HW_REFINE`, `HW_PARAMS`, `SW_PARAMS`,
`PREPARE`, `WRITEI_FRAMES`, `STATUS`, `DROP`.

### The device tree already has the card — [source]

`rk3326-anbernic-rg351m.dtsi` carries a `simple-audio-card` named
`rk817_int`, codec `&rk817`, cpu `&i2s1_2ch` (`rockchip,px30-i2s` at
`ff070000`), `mclk-fs = <256>`. So unlike the battery, no board file change is
needed — the node mainline ships is complete.

Note `hp-det-gpio = <&gpio2 RK_PC6 GPIO_ACTIVE_HIGH>`. Earlier notes recorded
this as `gpio0 22`, which came from the vendor DTS; `RK_PC6` is pin 22 of
**gpio2**. Mainline is what we boot.

### The config prerequisite that was only there by accident

The i2s block moves samples by DMA (`dmas = <&dmac 18>, <&dmac 19>`, a PL330
at `ff240000`). Without `CONFIG_DMADEVICES`/`CONFIG_PL330_DMA` the codec
defers forever and the machine has **no sound card at all** — not a broken
one, an absent one.

Both were already `=y` in our merged config, but only by inheritance from
`arm64 defconfig`, and *leaning the kernel is on the roadmap*. They are now
written into `image/pid351.config` explicitly. This is the first case found of
a working feature resting entirely on inheritance we intend to delete; it is
probably not the last, and it argues for auditing the merged config against
the fragment before that work starts rather than after.

### Mixer controls, read from the driver rather than remembered — [source]

`sound/soc/codecs/rk817_codec.c`:

- `"Master Playback Volume"` — `SOC_DOUBLE_R_RANGE_TLV`, range **-95.00 dB to
  0 dB**, registers `DDAC_VOLL`/`DDAC_VOLR`.
- `"Playback Mux"` — `SOC_DAPM_ENUM`, values **`"HP"`** and **`"SPK"`**.

Everything else in that driver is DAPM and powers itself up along the route.
Two controls is the whole mixer surface we need.

### Resampling is exact, not approximate — [laptop]

The panel is the master clock and audio is resampled to it, so the sample
count per video frame is carried in integer pixel-clock ticks rather than a
period in microseconds:

    acc += rate * PANEL_FRAME_PX;   n = acc / PANEL_PIXEL_HZ;   acc %= PANEL_PIXEL_HZ;

At 48 kHz that is 799.7365 samples a frame, emitted as a 799/800 pattern.
Measured over 3600 frames (60 s): **2879051 samples emitted against 2879051.3
exact — an error of 0.29 samples, bounded rather than accumulating.** Buffer
level stayed inside 2602..2682 frames and there were **zero underruns**.

Rounding `FRAME_US` to 16661 µs instead would drift about a sample every four
seconds. This is the one place in the project where rounding is unrecoverable.

### A blocking write costs no CPU — [laptop]

1.16 s of wall clock to play 1.0 s of audio through blocking `WRITEI_FRAMES`
cost **0.005 s user and 0.000 s sys**. Blocking on the audio buffer really is
a sleep, which is what CLAUDE.md's rule assumes but had never been checked.

### Period sizing: the battery argument loses, and should

512-frame periods over an 8-period buffer is 85 ms of cushion at 48 kHz and 94
interrupts a second. Battery-first would argue for longer periods and fewer
interrupts. It loses, because the gamepad's USB bus already costs ~6700
interrupts a second, so 94 more is not measurable, whereas the added latency
would be audible. **Where a power term sits below the noise floor of a term we
cannot remove, it is not a power term.**

`HW_REFINE` on the laptop accepted period_size 384..524288 and periods 2..256.
The device's constraints are unknown and are printed at every open for exactly
that reason — on a machine with no serial port, the boot log is the only way
that answer reaches us.

### The one open question, and why it is not being fixed yet

Running the full demo on the laptop showed `aud_lvl` pinned near the top of
the buffer (3580..4020 of 4096) and one underrun, where the standalone test
holds a rock-steady 2601..2658 with none.

That difference is a **host artifact, not a defect**. The laptop has no
vblank: `plat_sleep_until` returns immediately when late and `next` falls
behind, so the loop free-runs in bursts, over-produces, and `aud_write` blocks
— which quietly makes *audio* the pacing source. On the device `plat_present`
blocks on a real page flip, so the panel stays master and this cannot happen
the same way.

What remains genuinely unknown is drift between the panel's crystal and the
codec's, which are independent oscillators. Exact arithmetic stops us adding
error of our own; it cannot hold the level steady against two clocks. The
correction term is a single number and measuring it needs the device — watch
`aud_lvl` in the ten-second report over several minutes and read the slope.

Adding an adaptive controller now, tuned against a laptop artifact, would
repeat the mistake this project has already had to correct twice: trusting the
wrong oracle. `aud_lvl` is in the report line and on screen precisely so the
first device boot answers it.
