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
