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

## The otg-port patch

`rk3326-rg351p-linux-otg.dtb` is the vendor tree with one property changed:
`otg-port` goes from `status = "disabled"` to `status = "okay"`. Nothing else.

The evidence for it: with the phy's OTG sub-node disabled, the `otg-id`,
`otg-bvalid` and `linestate` interrupts never fire, so dwc2 has no way to learn
that anything has changed on the port. That single fact explains both failures
observed - a dongle in host mode never enumerates, and with `g_ether` loaded
the gadget comes up locally (usb0 at 192.168.7.2, sshd active) while the
attached laptop sees no USB device at all. The controller simply never leaves
the role it booted into.

Restore `rk3326-rg351p-linux.dtb.orig` over both `rk3326-rg351p-linux.dtb` and
`rk3326-rg351p-linux.dtb.13` on the BOOT partition to undo.
