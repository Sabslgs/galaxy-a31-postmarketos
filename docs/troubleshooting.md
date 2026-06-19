# Troubleshooting & notes

This is the grab-bag of things that bit me while bringing postmarketOS up on my Galaxy A31 (SM-A315G, MT6768). If your phone won't boot, won't show anything, or won't talk to WiFi, start here. Most of these cost me hours or days to figure out, so hopefully you skip the painful part.

## Read this first: never touch the EFS partitions

I'll put the scary one up top because it's the only mistake on this list you can't undo.

**Do not erase, format, or `dd` over `mmcblk0p3` (efs) or `mmcblk0p4` (sec_efs).** Those two hold your IMEI and the modem calibration data. Wipe them and your radio is dead permanently — no IMEI, no cellular, and there's no flashing your way back. pmOS doesn't need them, doesn't touch them, and neither should you. When you're poking around `/dev/mmcblk0p*` with `dd` or partition tools, double-check the number every single time.

While I'm at it: keep a backup of your stock `boot.img` somewhere off the phone before you flash anything. It's your only fast way back to Android download mode if a build hangs forever. I pulled mine in TWRP and stashed it on my PC, and I've leaned on it more than once.

The A31 has no fastboot (it's Samsung), so everything goes through Heimdall/Odin download mode. That also means a bad flash isn't bricking the bootloader, just the boot partition — as long as you didn't go near efs.

## "failed to mount subpartitions" in the initramfs

This one stops the boot dead in the pmOS initramfs, usually with a message about failing to mount subpartitions or not finding the root. It looks like the rootfs is gone, but it usually isn't.

What's actually happening: the kernel cmdline baked into `boot.img` carries a `pmos_root_uuid=...`, and the initramfs uses that UUID to find the root filesystem. If you flashed a `boot.img` from one pmbootstrap run and the root from a *different* run, those UUIDs don't match. The initramfs goes looking for a filesystem that isn't there and gives up.

Two ways out:

1. **Flash a matched pair.** Always flash `boot` and the root image from the *same* `pmbootstrap install`. This is the clean fix and it's what I do now by default. See [flashing.md](flashing.md) for the full flash sequence.
2. **Patch the UUID in the boot.img** to match the root that's already on the phone. I wrote `tools/patch_boot_uuid.py` for exactly this — it finds `pmos_root_uuid=` in the boot.img cmdline and overwrites the 36-char UUID in place (same length, so the image stays valid):

   ```sh
   ./tools/patch_boot_uuid.py boot.img 12345678-1234-1234-1234-123456789abc
   ```

   The UUID you pass is the *actual* UUID of the root partition that's flashed. You can read it off the phone with `blkid` — or, more conveniently, off the `PMOS_LOGS` drive described below, which dumps `blkid.txt` for you when boot fails.

## First boot looks completely hung — it's not, it's resize2fs

The first boot is slow. Alarmingly slow. The first time I flashed, I sat there for several minutes staring at a phone that looked dead and assumed I'd broken something.

It's `resize2fs`. The rootfs image gets flashed small and then grows to fill the partition on first boot — on my A31 that's a resize up to about 105 GB, and it blocks the boot while it runs. There's no progress on the panel (the panel isn't even being driven yet at that point), so it just looks frozen.

Leave it alone. It finishes, and on the **second** boot it comes up properly and you get sshd. If you're impatient like me, watch `dmesg` over the debug shell instead of power-cycling — power-cycling mid-resize is how you turn a slow boot into a corrupted one.

## The initramfs debug aids (telnet shell + PMOS_LOGS drive)

When a boot fails before sshd comes up, you're not flying blind. The pmOS initramfs gives you two ways in, and both saved me a lot of guessing.

**Telnet debug shell.** The initramfs brings up the USB gadget network and listens for telnet. Plug the phone into your PC, bring up the host side of the gadget interface, and:

```sh
telnet 172.16.42.1 23
```

That drops you into a shell *inside the initramfs*, before the real root is mounted. From there you can poke at `/dev`, run `blkid`, check what the initramfs thinks the root UUID is, and generally figure out why it's stuck.

**The PMOS_LOGS USB drive.** This one's the real gift. When boot fails, the initramfs exposes a small FAT filesystem over USB mass storage labelled `PMOS_LOGS`, and it just shows up as a removable drive on your host (Windows mounted it as a drive letter for me, no fuss). On it you get:

- `pmOS_init.txt` — what the init scripts did and where they bailed
- `dmesg.txt` — full kernel log
- `blkid.txt` — every block device and its UUID/label

When I was chasing the UUID mismatch above, `blkid.txt` was how I read the real root UUID without even getting a shell. If your boot dies, grab these three files first.

## Getting back in over USB

The phone is always **172.16.42.1** on the USB gadget network — both in the initramfs and in the booted system. Itself is `.1`, the host is `.2`. If you lose your connection (and you will, especially when toggling things like Bluetooth that flap the USB), bring the host side of the gadget interface up again and:

```sh
ssh user@172.16.42.1
```

If you want internet on the phone through the PC, that's a separate NAT setup — not covered here. The point is just: when in doubt, it's `.1`, and SSH lives there once you've booted past the initramfs.

## Panel still shows the Samsung logo after boot

This threw me the first time the system actually booted fine. SSH worked, `uname` was happy, everything was alive — but the screen still showed the stale Samsung boot logo. Nothing was broken.

The mtkfb framebuffer (`/dev/fb0`) is loaded, but the display hardware doesn't scan out new content until you tell it to. Until you issue an `FBIOPAN_DISPLAY` ioctl, the panel keeps showing whatever was in the framebuffer when the bootloader left off — which is the Samsung logo. Drive the panel and it updates. Graphics on this port are plain software-rendered on fbdev, so all the panel handling is on the CPU side. The full story (and the pan loop I run) is in [display.md](display.md).

## WiFi: firmware download stalls around fragment ~47

If you're bringing up the connsys WiFi (mtdaemon powering the chip, firmware blobs harvested from the vendor partition), there's a specific wall you can hit where everything *looks* right — the chip powers on, `mtinit` detects SOC chipid `0x6768`, `mtdaemon` starts pushing the ROM patch into EMI — and then the firmware download just stalls partway through. In my logs it consistently died around fragment **~47** of the `patch_mcu` download, followed by BTIF complaining it never got tx-allowed and the connsys MCU never coming alive.

The cause is the BTIF vFIFO saturating. The downstream default fragment size is 1000 bytes, and at 1000B the patch_mcu fragments come in faster than the connsys MCU can drain the BTIF FIFO, so it backs up and the transfer wedges. The fix is a one-line kernel patch dropping the fragment size to 256 bytes:

```c
// drivers/misc/mediatek/connectivity/wmt_drv/common_main/core/wmt_ic_soc.c
#define DEFAULT_PATCH_FRAG_SIZE 256   // was 1000
```

At 256B the MCU keeps up, the full patch_mcu downloads, calibration runs, `wlanProbe` fires, and `wlan0` finally appears. This was one of four fixes I needed for WiFi (the other three live in `mtdaemon.c`); the full saga is in the WiFi doc. But if your symptom is specifically a stall mid-download around fragment 47, this is the one you want. As far as I know this is the first time MT6768 connsys WiFi has run on pmOS, so if you hit a *different* connsys wall, open an issue — I'd genuinely like to know.

## A couple of smaller things

- **The kernel must be gzipped.** The A31 boot partition is exactly 32 MB. An uncompressed kernel doesn't fit. If your flash succeeds but the device won't boot at all, check the image size.
- **`/tmp` is tmpfs**, so anything you build there is gone after a reboot. I learned this the slow way by rebuilding the same tools twice. Put anything you want to keep under `/home` or `/opt`.
- If something here is wrong or you found a cleaner way, open an issue. I still don't fully understand every corner of the connsys boot sequence, and I'd rather the docs be right than be mine.
