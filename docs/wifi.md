# WiFi (the connsys saga)

This is the doc I most wanted to write, because getting WiFi up on the A31 was the single hardest thing in the whole port. It took me multiple days spread over a couple of weeks, a lot of staring at dmesg, disassembling Android binaries on the phone itself, and one kernel constant that I changed almost out of desperation and which turned out to be the actual fix. If you have an MT6768 device and you're reading this hoping for a shortcut: there isn't one, but the path is reproducible now, and I'll lay out every step plus every wall I hit.

If something here is wrong or you know a cleaner way, please open an issue. Some of this I still don't fully understand at the register level.

## What chip this actually is

The A31's WiFi/BT is the legacy MediaTek **WMT/STP "connsys"** combo block built into the MT6768 SoC. Note the word *legacy*: this is the older `wmt_drv` + `gen4m` stack, NOT the newer `conninfra` driver that the more recent MediaTek SoCs use. Half the guides and forum posts you'll find are about conninfra and they do not apply. If you go down a conninfra rabbit hole you'll waste a day like I did.

The driver is compiled **into the kernel** (`=y`), not as modules:

- `gen4m` — the WiFi driver (gen4m is MediaTek's "generation 4" wlan driver)
- `wmt_drv` — the WMT/STP core (power, firmware download, the chip transport)
- `wmt_chrdev_wifi` — the `/dev/wmtWifi` char device

So there's no `.ko` to `modprobe`. That tripped me up early — I kept looking for a module to load. There isn't one. The driver is already there; what's missing is the *userspace* that drives it and the *firmware* it downloads.

On a clean pmOS boot you get exactly one device node:

```
/dev/wmtdetect
```

And that's it. No `/dev/stpwmt`, no `/dev/wmtWifi`, no `/dev/stpbt`, no `wlan0`. Those only appear after userspace does the bring-up dance. `wmtdetect` is the "detection" device — it's the entry point.

Bring-up has two completely separate halves, and you need both: **firmware** (the vendor blobs the chip's MCU actually runs) and **userspace** (the loader/daemon that powers the chip on and feeds it the firmware). I'll do firmware first because without it nothing else matters.

## Part 1: harvesting the firmware

The connsys block is dead silicon until you download firmware into EMI (the shared memory the chip's MCU executes from). postmarketOS obviously doesn't ship MediaTek's proprietary connsys blobs, and they're per-SoC. The good news: my own phone has them sitting right there in its vendor partition, intact, because I never wiped it. So I harvested them off the device.

The blobs I needed:

- `soc1_0_ram_wifi_1a_1_hdr.bin` — WiFi firmware
- `soc1_0_ram_bt_1a_1_hdr.bin` — Bluetooth firmware
- `soc1_0_ram_mcu_1a_1_hdr.bin` — the WMT/MCU firmware
- `soc1_0_patch_mcu_1a_1_hdr.bin` — the MCU ROM patch (this one matters a lot, more below)
- `WMT.cfg` and `WMT_SOC.cfg` — connsys config (clock flags, BTIF settings)
- `wifi.cfg` — wlan config

The catch: on this device the vendor partition lives *inside* the Android `super` container (dynamic partitions / `liblp`). `super` is `mmcblk0p45`. You can't just mount `vendor` directly because it's a logical partition described by Android's LP metadata, not a real GPT partition.

### Mapping vendor out of super

I wrote a tiny LP-metadata parser, `tools/parse_lp.py`, that reads the `super` header and prints every logical partition with its offset and size in 512-byte sectors. The metadata format: there's a geometry block at offset 4096 (magic `0x616C4467`), then the metadata header at 12288 (magic `0x414C5030`), then partition + extent tables. The script walks the extents for each partition and prints the starting sector.

On my A31 it told me `vendor` starts at sector **8239104** and runs **1543776** sectors. With that I mapped it out of `super` using a device-mapper linear target:

```sh
# offset_sectors and size_sectors come from parse_lp.py on mmcblk0p45
dmsetup create vendor --table "0 1543776 linear /dev/mmcblk0p45 8239104"
mount -o ro /dev/mapper/vendor /mnt/vendor
```

(You don't strictly need `dmsetup` — `lpunpack` works too if you have it — but device-mapper is what I had and it's clean and read-only.)

Then the blobs were at `/mnt/vendor/firmware/`. I copied them to where the kernel/daemon look:

```sh
cp /mnt/vendor/firmware/soc1_0_* /lib/firmware/
cp /mnt/vendor/firmware/*.cfg     /lib/firmware/
```

I also dropped copies into `/vendor/firmware` and `/etc/firmware` just to be safe, because at various points I wasn't sure which path the kernel's `request_firmware` would resolve to and I didn't want to debug that too. `/lib/firmware` is the one that actually matters for this stack.

One thing that bit me later: the EEPROM/calibration blob is NOT in `vendor/firmware`. There's only `txpowerctrl.cfg` and the WIFI cfg. The real per-unit calibration lives in the nvram/nvdata partitions, and I worried the MAC would come up random. It didn't — final MAC is the real `00:08:22:84:f1:fb`, so the chip pulled it from somewhere valid. I never had to chase that down, which was a relief.

## Part 2: userspace, or why I had to build my own loader

On stock Android, the connsys bring-up is done by two vendor binaries: `wmt_loader` and `6620_launcher` (sometimes `wmt_launcher`). I grabbed them off the device. They don't run. They're bionic ELF binaries built against Android's libc, and pmOS is Alpine/musl. No bionic, no run. I considered shimming bionic but that's a much bigger project than just reimplementing the loader.

Enter **openmttools** — Dahrkael's open reimplementation of the MediaTek loader stack (`mtinit` + `mtdaemon`). It's plain C, no bionic dependency, so it builds natively on the phone:

```sh
apk add build-base
# build mtinit and mtdaemon natively (musl, aarch64)
```

But out of the box openmttools doesn't know about the MT6768. The chip-ID table is hardcoded, and `0x6768` isn't in it. So I patched the `chipIDs[]` array in **both** binaries to add it. In `connsys/mtinit.c`:

```c
static int chipIDs[] = {
    0x6620, 0x6628, 0x6630,
    0x6571, 0x6572,
    0x6580, 0x6582, 0x6592,
    0x8127, 0x8163,
    0x6752, 0x6735,
    0x0321, 0x0335, 0x0337, // these are 0x6735
    0x6768 // MT6768 (A31) connsys CONNAC1.x
};
```

and the same `0x6768` entry in `connsys/mtdaemon.c`.

The two tools do different jobs:

- **`mtinit`** talks to `/dev/wmtdetect`. It powers/detects the chip and tells the kernel the chip ID, which makes the kernel register the WMT modules and *create the device nodes*. After `mtinit` runs, `/dev/stpwmt`, `/dev/wmtWifi` and `/dev/stpbt` appear. The interesting code path: `COMBO_IOCTL_EXT_CHIP_DETECT` says it's an internal (SoC) chip, then `COMBO_IOCTL_GET_SOC_CHIP_ID` returns `0x6768`, then `COMBO_IOCTL_SET_CHIP_ID` + `COMBO_IOCTL_MODULE_CLEANUP` + `COMBO_IOCTL_DO_MODULE_INIT`. Those ioctls are `_IO?('w', n, int)` — see `connsys/mtinit.c` for the exact numbers.

  dmesg shows `mtinit: SOC chip detected with ID 0x6768` and the nodes pop into existence. This part worked relatively early and felt great. Then everything after it took days.

- **`mtdaemon`** talks to `/dev/stpwmt`. It opens the WMT device, queries the chip, and powers it on (`WMT_IOCTL_LPBK_POWER_CTRL` with arg 1). The power-on ioctl **blocks** while the kernel sends a series of commands back up to userspace over the same fd — so `mtdaemon` runs a poll loop on `/dev/stpwmt` and answers commands while the power-on call is in flight on a separate thread. The kernel asks "go find me the patch files" and the daemon answers.

When `mtdaemon` first ran, dmesg showed the chip waking up:

```
[WMT] query current consys chipid (0x6768)
... version 0x10020501
```

So the chip was alive and reporting its version. But it would not finish powering on. That's where the real saga starts.

## The blockers, in the order I hit them

### Blocker (a): the kernel asks for "srh_rom_patch" and the daemon has no idea what that is

The way the kernel requests firmware from userspace is by writing a command string to `/dev/stpwmt`, which the daemon reads in its poll loop. The stock openmttools only knew the older command `"srh_patch"` (the SDIO-era flow). This kernel, for the connsys SOC, sends a different one: **`"srh_rom_patch"`**, emitted from `wmt_ctrl_get_rom_patch_info` in `wmt_ctrl.c`.

The daemon would read `srh_rom_patch`, find no matching handler, reply "cmd not found", and the kernel would bail:

```
wmt_ctrl_get_rom_patch_info: wmt_ctrl_ul_cmd fail
... pwr_on fail
```

So I added a handler. In `connsys/mtdaemon.c` the command lookup now has both:

```c
static CommandEntry commandLookup[] = {
    { "srh_patch",     search_patch_callback },
    { "srh_rom_patch", search_rom_patch_callback }
};
```

The new `search_rom_patch_callback()` does this: for each connsys subsystem — BT (`WMTDRV_TYPE_BT=0`), WiFi (`WMTDRV_TYPE_WIFI=3`), WMT/MCU (`WMTDRV_TYPE_WMT=4`) — it opens the matching `.bin`, reads the 4-byte `u4PatchAddr` at **offset 24** of the firmware header (`struct wmt_rom_patch`), and pushes it to the kernel with:

```c
#define WMT_IOCTL_SET_ROM_PATCH_INFO _IOW(0xA0, 31, char*)

struct wmt_rom_patch_info_u {
    unsigned int  type;          // WMTDRV_TYPE_*
    unsigned char addRess[4];    // u4PatchAddr from the .bin
    unsigned char patchName[256];
}; // 264 bytes
```

The WMTDRV type enum, for reference: `BT=0, FM=1, GPS=2, WIFI=3, WMT=4, ANT=5`.

With that handler in, dmesg finally showed the blobs being written into EMI: `soc1_0_ram_bt/wifi/mcu` going to `EmiOffset 0x80011 / 0x140011 / 0x11`, with `HVer/SVer 0x8a00`. Progress. Real, visible progress. And then it died at the next step.

### Blocker (b): missing the cfg name and the chipid tell

After the EMI download the chip still wouldn't come up. dmesg complained:

```
wmt_conf_parse: failed to parse 'wifi_config'
```

The kernel needs to be told which config file to parse (clock flags, BTIF transport settings live in there), and the stock daemon never told it. I figured this out by `strings`-ing and then `objdump`-disassembling the stock `wmt_launcher` right on the phone, and decoding which ioctls it fires that openmttools doesn't. Two were missing:

1. **`WMT_IOCTL_WMT_CFG_NAME`** = `_IOWR(0xA0, 21, ...)` — set the WMT config filename. I set it to `"WMT_SOC.cfg"`, which is the SOC variant and has `co_clock_flag=1`. After this, dmesg confirms `WMT config file is set to (WMT_SOC.cfg)`. Without it the kernel uses wrong BTIF/clock defaults and the transport never comes up right.

2. **`WMT_IOCTL_WMT_TELL_CHIPID`** = `_IOW(0xA0, 23, int)` — explicitly tell the kernel the chip ID *after* the query. The stock launcher does this; openmttools didn't. Added it.

At one point in here I also hit repeated `[MPU] EMI MPU violation` (read violation, master `MT6768_M0/M1_AXI_MST`) — the EMI memory protection unit blocking the chip's reads. That cleared up once the cfg/clock setup was correct; it was a symptom of the wrong config, not a separate wall. So I never had to manually poke MPU registers, which I'd been dreading.

After (a) and (b), userspace was, as far as I could tell, byte-for-byte matching what the stock launcher does. I'd diffed the ioctl sequences. And it *still* didn't bring up `wlan0`. The chip powered on (`CONSYS-HW-PWR-ON finish(0)`, all the `vcn18/28/33_bt` regulators on, reset registers reading live-changing values so the silicon was clearly alive) but:

```
MTK-BTIF: wait for tx allowed timeout
wmt_core_hw_check: get hwcode fail(-3)
WMT turn on WIFI fail
```

The chip was alive but the BTIF command transport never became ready. `hal_dma_is_tx_allow` stayed false, "Tx DMA flush in process" never completed. That meant the connsys MCU wasn't draining the BTIF FIFO, which meant the MCU wasn't actually *running* the firmware I'd downloaded. Firmware in EMI, SoC powered, MCU alive — but MCU not executing. That was the deepest, most demoralizing wall. I spent more than a day here convinced it was userspace and re-checking every ioctl.

### Blocker (c): the kernel fragment size that was the real fix

It wasn't userspace. It was a kernel constant.

The firmware download to the connsys MCU goes in fragments over the BTIF transport. The kernel's default fragment size is **1000 bytes**, set by `DEFAULT_PATCH_FRAG_SIZE` in:

```
drivers/misc/mediatek/connectivity/wmt_drv/common_main/core/wmt_ic_soc.c
```

With 1000-byte fragments, the `patch_mcu` download would saturate the BTIF vFIFO partway through (around fragment ~47 in my traces), the STP flow-control window would never recover, and the rest of the patch never made it across. So the MCU got a *partial* firmware image, never booted, and BTIF never went tx-allowed. All the "MCU alive but not running" symptoms came from this.

I shrank it to **256**:

```c
#define DEFAULT_PATCH_FRAG_SIZE 256
```

(See `kernel.md` for how this fits with the rest of the kernel patches and the rebuild/reflash flow.)

With 256-byte fragments the MCU drains the BTIF FIFO fast enough to keep up, the full `patch_mcu` downloads, the chip runs calibration, `wlanProbe` succeeds, and `wlan0` appears. I genuinely did not expect a fragment-size tweak to be the thing. I found it by reading the BTIF driver's flow-control logic line by line, counting how many fragments fit in the vFIFO, and noticing the math didn't work at 1000.

### And the one I want to flag specially: addRess[0]

There's one more fix that's small but was responsible for a lot of wasted time, and it's worth its own callout because the code snapshot in `connsys/mtdaemon.c` shows where it goes.

The 4-byte `u4PatchAddr` I read at offset 24 of the `.bin` header is *not* purely an EMI address. The **low byte is a sequence flag**, not address bits. The stock launcher zeroes it (`strb wzr` in the disassembly). I was passing it through verbatim, so my EMI offsets all ended in `0x11` — that's the suspicious low byte. With `addRess[0]` left at `0x11`, the firmware landed at the wrong EMI offset (`0x80011` instead of `0x80000`) and `wmt_core_hw_check: get hwcode` failed all session long. The kernel computes the offset as `(addr[2]<<16)|(addr[1]<<8)|addr[0]`, so that stray `0x11` poisons it.

Set `addRess[0] = 0` after reading and `EmiOffset 0x80011 → 0x80000`, and `wmt_core_hw_check: get hwcode (0x6768)` finally PASSES.

Related gotcha: for the older `srh_patch` path, the patch name for `0x6768` has to resolve to exactly `soc1_0_patch_mcu` — if you use just `soc1_0` it prefix-matches four files and the kernel complains "4 of 1" (it expected one patch, found four). Be specific.

## Putting it together: the winning sequence

So the four winning fixes, all on top of the harvested `/vendor/firmware` blobs sitting in `/lib/firmware`:

1. `addRess[0] = 0` — strip the sequence flag from the low byte of `u4PatchAddr`.
2. `WMT_IOCTL_WMT_CFG_NAME` → `"WMT_SOC.cfg"` (`co_clock_flag=1`) before power-on.
3. `WMT_IOCTL_WMT_TELL_CHIPID` after the chip-id query.
4. Kernel: `DEFAULT_PATCH_FRAG_SIZE` 1000 → 256.

The first three live in the patched `connsys/mtdaemon.c` (read it for the exact ioctl numbers and structs). The fourth is in the kernel — `kernel.md`. The patched `connsys/mtinit.c` is the one with `0x6768` added to its chip table.

Bring-up, start to finish, on a freshly booted phone (the kernel caches rom-patch info, so if you're iterating on the daemon you sometimes have to reboot to get it to re-request):

```sh
# 1. firmware already in /lib/firmware (see Part 1)

# 2. create the device nodes
./mtinit
# dmesg: "SOC chip detected with ID 0x6768"; /dev/stpwmt /dev/wmtWifi /dev/stpbt appear

# 3. power the chip on + download firmware (keep it running)
nohup ./mtdaemon -p /lib/firmware &
# dmesg: query chipid 0x6768, version 0x10020501, blobs to EMI,
#        WMT_SOC.cfg parsed, get hwcode (0x6768) OK, MCU boots, wlanProbe, wlan0
```

Once `mtdaemon` reports the chip up, create the actual interface:

```sh
echo 1 > /dev/wmtWifi   # function-on WiFi
echo S > /dev/wmtWifi   # STA mode
```

and `wlan0` is there.

## Connecting

CONFIG_CFG80211_WEXT is **not** set in this kernel, so the old wireless-extensions path doesn't exist. You must use nl80211 / `iw`. `wpa_supplicant` with the wext driver won't work; use `-Dnl80211`.

```sh
apk add iw wpa_supplicant dhcpcd

wpa_passphrase "MySSID" "mypassword" > /tmp/wpa.conf
wpa_supplicant -B -i wlan0 -D nl80211 -c /tmp/wpa.conf
dhcpcd -4 wlan0
```

A scan (`iw dev wlan0 scan`) sees ~20 APs across 2.4 and 5 GHz. On my network I associate to a 5 GHz AP at 234 Mbit/s, DHCP leases an address, and `ping 1.1.1.1` over `wlan0` is 0% loss. Real internet, over real WiFi, no USB tether. The MAC is the genuine factory `00:08:22:84:f1:fb`.

## End state

WiFi fully works. Associated, DHCP, internet, both bands. As far as I can tell this is the first time the MT6768 connsys WiFi has been brought up on postmarketOS, which is a nice thing to be able to say after the week it took.

A couple of practical notes:

- `/tmp` is tmpfs and gets wiped on reboot, so if your openmttools build tree lives there it's gone after a reboot — rebuild from source. The firmware in `/lib/firmware` persists.
- Opening the connsys/BT devices briefly toggles something on the USB path, so if you're working over a USB-gadget SSH session your connection can drop for a second when the chip powers on. Run the bring-up detached (`nohup`/`setsid`) so a dropped SSH doesn't kill it mid-download.
- Bluetooth is basically free once this is done — same connsys core, you just open `/dev/stpbt`. That's its own doc.

The files: patched `connsys/mtdaemon.c` and `connsys/mtinit.c` (read these for the exact ioctl numbers and struct layouts — I documented them inline), the LP parser at `tools/parse_lp.py`, and the `DEFAULT_PATCH_FRAG_SIZE` change covered in `kernel.md`.

If you're porting another MT6768 device: harvest your own vendor blobs (they're per-unit/per-SoC, don't use mine), add `0x6768` to both chip tables, apply the four fixes, and expect to spend time reading dmesg. It's reproducible now, but it's not a one-liner, and I won't pretend it was.
