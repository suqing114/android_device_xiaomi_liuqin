# Recovery-only kernel modules

`nt36532_touch.ko` is the stock vendor_dlkm module from
`device/xiaomi/liuqin-kernel` (sha256 `6260743c…bd24a`) with one instruction
changed, used only in the vendor ramdisk for recovery mode
(`BOARD_VENDOR_RAMDISK_KERNEL_MODULES` / `..._RECOVERY_KERNEL_MODULES_LOAD`).

The NT36532 controller is flashless: the driver downloads its firmware from a
delayed work (`nvt_fwu_wq`) that `nvt_ts_probe()` queues 3500 jiffies (14 s at
HZ=250) after probe. In recovery the module loads at ~1.7 s and the UI is up at
~2.5 s, so touch stayed dead until ~15.8 s. The patch changes that delay to
750 jiffies (3 s); the panel is already on by then.

    nvt_ts_probe+0x... : mov w3, #0xdac  ->  mov w3, #0x2ee   (3 bytes, file offset 0x3a2a8)

The module is unsigned like the rest of the vendor modules; vermagic and
modversions are unchanged. Normal boot still loads the original from
/vendor_dlkm. To redo it after a kernel update, patch the new module the same
way (MOVZ w3 imm16 right after `mov w0, #0x20`, before `queue_delayed_work_on`).
