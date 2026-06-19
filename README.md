# postmarketOS on the Samsung Galaxy A31

This is my port of postmarketOS to the Samsung Galaxy A31 (SM-A315G, the MediaTek
MT6768 one). There was no existing device package for it, so I built one from
scratch: kernel, boot image, and then the slow grind of getting the actual
hardware to come up — display, touch, WiFi, Bluetooth, audio.

It boots to a real Linux userland with a working network and an X desktop. None
of this depends on Android being installed; it's mainline-ish downstream Linux
running off its own kernel.

A couple of honest caveats up front:

- This is a **downstream** port (Samsung's 4.14 kernel), not mainline. It's a
  hobby project, so expect rough edges.
- Graphics here are **software-rendered on fbdev**. I didn't chase GPU
  acceleration in this project, so the X desktop runs on plain framebuffer.

If you have an A31 and want to do the same thing, start with [INSTALL.md](INSTALL.md).

## What works

| Thing | State | Notes |
|---|---|---|
| Boots postmarketOS, SSH over USB | yes | `uname`: `Linux samsung-a31 4.14.186 aarch64` |
| Display (mtkfb / fbdev, 1080×2400) | yes | needs the `FBIOPAN_DISPLAY` trick |
| Touchscreen (IST4050) | yes | had to fix a VLA to build the driver |
| X desktop (software) | yes | Xorg + openbox + on-screen keyboard |
| WiFi (MediaTek connsys) | yes | the hard one — see [docs/wifi.md](docs/wifi.md) |
| Bluetooth | partial | raw HCI confirmed, no BlueZ bridge yet |
| Audio (speaker) | yes | manual DAPM routing to the SMA1303 amp |

## How it's laid out

```
device-samsung-a31/   the pmaports material: deviceinfo + kernel APKBUILD
connsys/              my patched openmttools (mtinit + mtdaemon) for WiFi/BT
tools/               little python helpers (parse boot.img / super, fix root UUID)
scripts/             bring-up helpers you run on the phone
docs/                the actual write-ups, one per piece of hardware
```

The docs are where the real detail lives:

- [kernel.md](docs/kernel.md) — building the downstream kernel and the patches it needs
- [flashing.md](docs/flashing.md) — Heimdall, download mode, the gotchas
- [display.md](docs/display.md) — fbdev, the FBIOPAN quirk, the X-on-fbdev desktop
- [touch.md](docs/touch.md) — the IST4050 and the one-line fix to compile it
- [wifi.md](docs/wifi.md) — the MediaTek connsys saga (this took me days)
- [bluetooth.md](docs/bluetooth.md) — raw HCI over `/dev/stpbt`
- [audio.md](docs/audio.md) — routing sound to the speaker by hand
- [troubleshooting.md](docs/troubleshooting.md) — the stuff that bit me

## Before you start — read this

**Do not touch `mmcblk0p3` (efs) or `mmcblk0p4` (sec_efs).** They hold your IMEI
and modem calibration. If you wipe them the radio is gone for good and there's no
clean way back. Keep a backup of your stock `boot.img` before you flash anything.

This is reverse-engineering of a locked-down vendor stack. It works on my unit but
I can't promise it won't break yours. Proceed accordingly.

## Credit where it's due

- [postmarketOS](https://postmarketos.org/) and `pmbootstrap` — the whole foundation.
- [openmttools](https://gitlab.com/Dahrkael/openmttools) by Dahrkael — the MediaTek
  connsys launcher I patched for the MT6768. WiFi/BT would not exist here without it.
- The [Galaxy-MT6768](https://github.com/Galaxy-MT6768/android_kernel_samsung_mt6768)
  LineageOS kernel — the kernel tree I build from.

The vendor WiFi/BT firmware is **not** in this repo. You harvest it from your own
device (see [docs/wifi.md](docs/wifi.md)).

## License

[MIT](LICENSE) for the code I wrote. The kernel APKBUILD references a GPL-2.0
kernel tree, and the deviceinfo follows pmaports conventions.
