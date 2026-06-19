# connsys (WiFi + Bluetooth userspace loader)

This folder holds my patched build of **openmttools** for the A31's MediaTek connsys
chip. The MT6768 reports chip ID `0x6768`, and getting it to power on and download
its WiFi/BT firmware took me an embarrassing number of days. The two source files
here are what finally worked.

- **`mtinit.c`** — detects the SOC connsys chip and pokes the kernel into creating
  `/dev/stpwmt`, `/dev/wmtWifi` and `/dev/stpbt`. I added `0x6768` to the `chipIDs[]`
  table so it stops bailing with "unknown chip ID".
- **`mtdaemon.c`** — the actual loader daemon. This is where most of the work went.
  On top of upstream I added three handlers the 4.14 A31 kernel demands and stock
  openmttools didn't speak:
  - the **`srh_rom_patch`** command handler — the kernel sends this (not the old
    `srh_patch`) to ask where each firmware blob goes in EMI. For each subsystem
    (BT=0, WIFI=3, WMT=4) it reads the patch address out of the `.bin` header and
    feeds it back via `WMT_IOCTL_SET_ROM_PATCH_INFO` (`_IOW(0xA0, 31, ...)`). Without
    it, power-on dies at `wmt_ctrl_get_rom_patch_info ... fail`.
  - **`WMT_CFG_NAME`** (`_IOWR(0xA0, 21)`) — set to `WMT_SOC.cfg` before power-on so
    the kernel parses the right config (that's the one with `co_clock_flag=1`).
    Without it the BTIF/clock defaults are wrong.
  - **`TELL_CHIPID`** (`_IOW(0xA0, 23)`) — tell the kernel the chip ID after the
    QUERY step, exactly like Samsung's stock `wmt_launcher` does.

One detail that cost me real time: the patch name for `0x6768` has to be
`soc1_0_patch_mcu` for the MCU ROM patch, not bare `soc1_0` (that matched four files
and the kernel complained "4 of 1"). And the low byte of the patch address is a
sequence flag, not part of the EMI offset — it has to be zeroed or the firmware lands
at the wrong place and `hw_check` fails forever. The full story of all that is in
[`../docs/wifi.md`](../docs/wifi.md).

## Building on the phone

These are plain C against musl, no autotools, no cross-compile. Build them natively
on the A31 itself:

```sh
apk add build-base
cc -o mtinit mtinit.c
cc -o mtdaemon mtdaemon.c
```

That's it. Two static-ish musl binaries. Heads up: if your `/tmp` is tmpfs it gets
wiped on reboot, so copy the binaries somewhere persistent (I keep mine in
`/home/downstream` or `/opt`) or just rebuild after each boot.

Rough bring-up order once the firmware's in place:

```sh
./mtinit                       # creates /dev/stpwmt, /dev/wmtWifi, /dev/stpbt
./mtdaemon -p /lib/firmware    # downloads firmware, powers the chip on
echo 1 > /dev/wmtWifi          # brings up wlan0
```

## Firmware is NOT included

There's no firmware in this repo and there never will be — it's Samsung/MediaTek
proprietary and unit-specific (the calibration data is tied to your actual chip).
You harvest it from your **own** device's vendor partition. The blobs live in the
intact `super` partition (`mmcblk0p45`), in the `vendor` logical partition, and you
pull `soc1_0_ram_{wifi,bt,mcu}*.bin`, `soc1_0_patch_mcu*.bin`, `WMT_SOC.cfg` and
friends into `/lib/firmware`. The exact `dmsetup`/lpunpack steps and the full
connsys saga are documented in [`../docs/wifi.md`](../docs/wifi.md). Read that before
you touch these binaries.

## Credit

Upstream is **[Dahrkael/openmttools](https://gitlab.com/Dahrkael/openmttools)**. All
I did was add the `0x6768` chip ID and the three handlers above so the A31's
connsys actually boots its MCU. If you've got a cleaner way to do any of this, open
an issue — I'd genuinely like to know.
