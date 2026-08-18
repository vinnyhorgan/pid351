# RG351P — hardware ground truth

What this specific unit actually is. Everything below marked **[dtb]** was read
out of ArkOS's vendor device tree on 2026-08-18; everything marked **[todo]**
needs `tools/recon.sh` run on the device to answer.

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
- DTB selected at runtime by hardware revision: `hwrev == v11` picks between
  `rk3326-odroidgo2-linux-v11.dtb` and `rk3326-rg351p-linux.dtb.13` on GPIO
  `c22`; otherwise `rk3326-odroidgo2-linux.dtb`. **[todo]** which one this unit
  actually gets.
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

For the per-console timing plan, GBA's 59.727 Hz needs a pixel clock of
59.727 x 283,240 = **16.917 MHz** with the porches unchanged. Adjusting vtotal
instead only gets to 59.774 Hz, so the clock is the knob. **[todo]** whether
the VOP's PLL can actually hit 16.917 MHz.

## RGA — present

**[dtb]** `rockchip,rga2` at `0xff480000`, `status = "okay"`, `dma-coherent`.

This is the **vendor RGA2**, driven through `/dev/rga` with Rockchip's own
ioctl interface — *not* mainline's `rockchip-rga` v4l2-m2m driver. Code written
against it will not port to mainline unchanged, which is why RGA stays behind a
CPU fallback (see PLAN.md phase 1).

## Backlight

**[dtb]** `pwm-backlight`, 25,000 ns period (40 kHz), 256 brightness levels,
default 255. Fine-grained control, which matters because the backlight is
almost certainly the single largest consumer on this device.

## Battery — better than expected

**[dtb]** `rk817,battery` fuel gauge on the PMIC I2C bus.

    design_capacity   3450 mAh
    design_qmax       3795
    sample_res        10   (10 milliohm shunt)
    power_off_thresd  3400 mV

A shunt resistor **is** fitted, so `current_now` should be readable. That means
the power benchmark can measure actual milliamps instead of inferring drain
from a voltage curve — far faster and far more trustworthy. **[todo]** confirm
`/sys/class/power_supply/*/current_now` exists and is non-zero.

## CPU / GPU frequencies — a finding worth acting on

**[dtb]** The vendor OPP tables:

    CPU   1008, 1200, 1248, 1296 MHz
    GPU   400, 480, 520 MHz

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

Peripheral mode is therefore supported by the hardware and the DT: the ethernet
gadget plan is viable, and one USB-C cable can carry power and networking to
the laptop at once. **[todo]** whether ArkOS's 4.4 kernel has the gadget modules
built (`/sys/class/udc` will say).

## Input — still open

**[dtb]** There is no `gpio-keys`, `adc-keys` or joypad node in the vendor tree.
`btns` is only a pinctrl group, and `saradc@ff288000` is enabled. So the
gamepad is either a USB HID device or handled by a userspace daemon reading
GPIO and ADC directly.

**[todo]** `/proc/bus/input/devices` settles this, and it is the single most
important thing recon.sh will tell us.
