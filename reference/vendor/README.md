# Vendor device tree

`px30-evb-ddr3-v10-nspanel-pro-86-vendor.dts` is the device tree of the original NSPanel Pro 86 firmware (Android 8.1, `NSPanel86P_1.6.5`, display id 4.9.5), decompiled with `dtc` from the `resource` partition of a development unit.

It is kept only as a hardware reference for writing the mainline board DTS (pin assignments, regulator voltages, panel init sequence and timing). The matching kernel sources are published by ITEAD in [itead/NSPro_Kernel](https://github.com/itead/NSPro_Kernel).

No other part of the original firmware is distributed in this repository.
