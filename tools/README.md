# tools

These are the throwaway Python scripts I wrote while porting the A31. No pip dependencies, no argparse, nothing fancy. Pure stdlib (`struct`, `sys`), so just run them with `python3`. Comments inside are half in Spanish because they're mine, sorry about that.

I'm leaving them here because they each got me past a specific wall, and someone else with an A31 (or another MT6768 Samsung) will probably hit the same walls.

## parse_bootimg.py

```sh
python3 parse_bootimg.py a31_boot.img
```

Dumps the Android boot image header (v0/v1/v2). It prints kernel/ramdisk/second/tags addresses, page size, the cmdline, and then spits out the `deviceinfo_flash_offset_*` lines already computed assuming the standard 0x8000 kernel offset, so you can paste them straight into `deviceinfo`. That's how I confirmed the A31 values: header v2, pagesize 2048, base 0x40078000, kernel 0x8000, ramdisk 0x07c08000, tags/dtb 0x0bc08000.

It also sniffs for a MediaTek header (the `0x88 0x16 0x88 0x58` magic) on the kernel and ramdisk. On the A31's stock image there isn't one, it's plain AOSP, but I wanted to be sure before assuming. If your image has it, the script tells you the labels.

## parse_super.py

```sh
python3 parse_super.py super.img
```

Parses an extracted `super` image (the dynamic-partitions blob) and lists every logical partition (`vendor`, `system`, `product`, etc.) with its offset and size in 512-byte sectors plus bytes and MB. I needed this to figure out where `vendor` actually lived so I could pull the WiFi firmware out of it. It reads the liblp geometry at offset 4096 and the metadata header right after the two geometry copies.

## parse_lp.py

```sh
python3 parse_lp.py
```

Same idea as `parse_super.py`, but it reads the LP metadata directly off the device (`/dev/mmcblk0p45`, hardcoded at the top of the file, edit it if yours differs) and prints a ready-to-use `dmsetup` line per partition. So instead of carving the image on my PC, I could map the logical partitions live on the phone and mount `vendor` to harvest firmware. Run it as root.

## patch_boot_uuid.py

```sh
python3 patch_boot_uuid.py a31_boot.img <36-char-rootfs-uuid>
```

Rewrites `pmos_root_uuid=` in the boot image's cmdline in place (same length, 36 chars), so the boot half matches your actual rootfs. This fixes the "failed to mount subpartitions" trap in the initramfs. I hit it when I flashed a boot.img and a rootfs from two different pmbootstrap runs: the UUID baked into the cmdline no longer matched the filesystem's UUID, and the initramfs just gave up. The clean fix is to always flash a matched boot+root pair from the same install. This script is the dirty fix when you've already got a boot.img you don't want to rebuild: grab the running root's UUID (`blkid` / `lsblk -f`) and patch the image. It refuses if it can't find the key or your UUID isn't 36 chars.
