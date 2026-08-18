# Vendor artefacts

Pulled off the ArkOS card as reference, unmodified.

- `rk3326-rg351p-linux.dtb.orig` — the DTB this unit actually boots, confirmed
  by `/proc/device-tree/model` reading `Anbernic RG351P`. Keep as the restore
  point for any device tree experiment.
- `rk3326-rg351p-linux.dts` — decompiled with `dtc`, for reading and diffing
  against mainline's `rk3326-anbernic-rg351m`.
- `boot.ini.orig` — the U-Boot script. Restore point before touching boot.

## The USB puzzle

`usb@ff300000` is dwc2 with `dr_mode = "otg"` and `phys = <0x8b>`, which is the
`otg-port` sub-node of `usb2-phy@100` — and that sub-node is
`status = "disabled"` (line 1712 of the .dts). The `host-port` phy next to it
*is* enabled, but it belongs to the EHCI controller at `ff340000`, which is
itself disabled.

Despite that, dwc2 works: the internal gamepad enumerates at
`usb-ff300000.usb-1.2`, behind a hub at `1-1`. So the controller runs in host
mode with its phy's OTG detection turned off, which is consistent with the
external port never noticing anything being plugged into it.
