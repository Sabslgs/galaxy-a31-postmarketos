# Installing this on your own A31

This walks through getting postmarketOS onto a Galaxy A31 from scratch, the same
way I did it. It assumes you have the **SM-A315F/G/N** (MediaTek MT6768) variant —
not the Exynos one, if that exists in your region. Check `ro.board.platform` reads
something like `mt6768` before you start.

Set aside an afternoon. The flashing part is quick; WiFi is the part that takes
patience.

## What you need

- The phone, and a USB cable that does data.
- A Linux host (a real install or WSL2 both work — I used WSL2).
- [`pmbootstrap`](https://wiki.postmarketos.org/wiki/Pmbootstrap) installed.
- [`heimdall`](https://gitlab.com/BenjaminDobell/Heimdall) for flashing (or Odin on
  Windows). There is no fastboot on this device.
- An **unlocked bootloader**. On these Samsungs you enable OEM unlocking in
  Android's developer options first; if the bootloader is still locked you'll need
  to unlock it (mtkclient can unlock `seccfg` on the MT6768 — that's its own
  procedure, look it up for your exact build).
- A backup of your stock `boot.img`. Pull it before you overwrite it. Seriously.

And the warning again, because it matters: **never write to `mmcblk0p3` or
`mmcblk0p4`.** Those are efs/sec_efs (IMEI, modem cal). Flash only BOOT and
USERDATA as described below.

## 1. Put the device package into pmaports

Clone pmaports (pmbootstrap keeps a checkout, usually under
`~/.local/var/pmbootstrap/cache_git/pmaports`) and drop in two packages:

```
device/downstream/device-samsung-a31/     <- deviceinfo (from device-samsung-a31/ here)
device/downstream/linux-samsung-a31/       <- APKBUILD (from device-samsung-a31/ here)
```

You also need the kernel config at
`linux-samsung-a31/config-samsung-a31.aarch64`. I base mine on the kernel tree's
`arch/arm64/configs/a31_defconfig` with these changes:

- `CONFIG_DEVTMPFS=y` and `CONFIG_DEVTMPFS_MOUNT=y` (pmOS needs `/dev` populated)
- turn off `CONFIG_COMPAT_VDSO` and `CONFIG_IKHEADERS` (they break the build)

The build-time fixes (the `compiler-gcc.h` tweak, the dtc redirect, the gzip step,
the connsys fragment-size patch, the touch VLA fix) all live in the APKBUILD's
`prepare()`/`package()` — see [docs/kernel.md](docs/kernel.md) for what each one
does and why.

## 2. Build and export the image

```sh
pmbootstrap init            # pick: samsung / samsung-a31, edge, your UI
pmbootstrap install         # builds the rootfs
pmbootstrap export          # drops boot.img + the rootfs image in /tmp/postmarketOS-export
```

One catch if you're on WSL2: a full `pmbootstrap install` that loop-mounts can
fail because the Microsoft kernel has no loop device. Building the packages and
the rootfs image still works; you just flash the exported `boot.img` and the
rootfs from the host instead of letting pmbootstrap write them directly.

## 3. Flash it

Power the phone off. With the USB cable **unplugged**, hold Volume Up + Volume
Down, then plug the cable in — that drops you into Samsung download mode. Accept
the warning with Volume Up.

```sh
heimdall flash --BOOT /tmp/postmarketOS-export/boot.img
# then flash the rootfs to USERDATA (filename will match your export)
heimdall flash --USERDATA /tmp/postmarketOS-export/samsung-a31.img
```

If the bootloader rejects the modified boot image, you may need a vbmeta with
verification disabled flashed to the vbmeta partitions. The bootloader being
unlocked usually covers this.

Two things that will save you grief:

- The boot partition is exactly 32 MB and the uncompressed kernel is ~30 MB once
  it grows, so the kernel **must** be gzipped. The APKBUILD already does this; just
  don't swap in an uncompressed `Image`.
- Flash a **matched** boot + rootfs pair from the same build. If the UUID baked
  into the boot cmdline doesn't match the rootfs, the initramfs stops with
  "failed to mount subpartitions". If you ever need to fix it after the fact,
  `tools/patch_boot_uuid.py` rewrites the UUID in a boot image.

## 4. First boot

The first boot is slow and looks hung — that's `resize2fs` growing the tiny rootfs
to fill userdata. Leave it. It usually only reaches `sshd` on the **second** boot.

postmarketOS comes up as a USB gadget. The phone is `172.16.42.1`. Bring the host
side of the gadget network up and connect:

```sh
ssh user@172.16.42.1     # or telnet 172.16.42.1 from the initramfs debug shell
```

If boot fails, the initramfs gives you two lifelines: a telnet shell on
`172.16.42.1:23`, and a FAT volume called `PMOS_LOGS` that mounts on your host with
`pmOS_init.txt` / `dmesg.txt` / `blkid.txt`. Read those first.

At this point you have a booting Linux phone with a screen (after the FBIOPAN
trick, see [docs/display.md](docs/display.md)) and touch. Now the fun part.

## 5. WiFi

This is the involved one and it has its own write-up:
[docs/wifi.md](docs/wifi.md). The short version:

1. Harvest the connsys firmware from your own device's vendor partition (it's
   inside `super`, mmcblk0p45 — `tools/parse_lp.py` helps you locate it) and copy
   the `soc1_0_ram_*` blobs and the `*.cfg` files into `/lib/firmware`.
2. Build my patched `openmttools` on the phone:
   ```sh
   apk add build-base
   cc -O2 -o mtinit   connsys/mtinit.c
   cc -O2 -o mtdaemon connsys/mtdaemon.c
   ```
3. Run `mtinit` then `mtdaemon` to power the chip on. With the kernel's
   `DEFAULT_PATCH_FRAG_SIZE=256` patch in place, the firmware download completes
   and the MCU boots.
4. Create the interface and connect:
   ```sh
   echo 1 > /dev/wmtWifi          # then: echo S > /dev/wmtWifi
   WIFI_SSID="your-net" WIFI_PSK="your-pass" ./scripts/setup-wifi.sh
   ```

Use `wpa_supplicant -Dnl80211` (this kernel has no WEXT). After that, `dhcpcd
wlan0` and you're online.

## 6. Bluetooth (optional)

Once connsys is up, opening `/dev/stpbt` powers the BT side on and you can talk
raw HCI to it. See [docs/bluetooth.md](docs/bluetooth.md). There's no BlueZ
integration yet, so this is more "the radio works" than "pair your headphones".

## 7. Audio

```sh
./scripts/speaker.sh     # routes DL -> I2S1 -> the SMA1303 amp
aplay something.wav
```

Details in [docs/audio.md](docs/audio.md). Keep the amp volume moderate or it
clips.

## That's it

You've got a Galaxy A31 running Linux with networking and sound. If something
doesn't line up with your unit (different region build, different partition
layout), check [docs/troubleshooting.md](docs/troubleshooting.md) first, and feel
free to open an issue — I'd like to know if this generalizes cleanly to other A31
builds.
