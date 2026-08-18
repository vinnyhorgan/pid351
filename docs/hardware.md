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
has a stick at all - the cores must not, since none of the five consoles has
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
