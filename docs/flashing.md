# Flashing it

So you've built (or grabbed) a pmOS image for the A31 and now you need to get it onto the phone. The A31 is a Samsung, which means **download mode + Heimdall/Odin**. There's no fastboot. I kept reaching for `fastboot flash boot ...` out of habit and it does nothing here, the device just doesn't speak that protocol. Get that out of your muscle memory early.

I'm on a Windows host with the actual build happening in a WSL2 Ubuntu. If you're fully on Linux you can do all of this with Heimdall directly; on Windows you've got the choice of Heimdall or Samsung's Odin. I'll cover both where it matters.

## What you're flashing

Two things go onto the phone:

| Image | Goes to | Partition |
|-------|---------|-----------|
| `boot.img` (kernel + initramfs) | BOOT | `mmcblk0p35` |
| rootfs (the actual Alpine/pmOS filesystem) | USERDATA | `mmcblk0p52` |

That's it. We don't touch recovery, we don't touch super, and for the love of everything **do not** touch `mmcblk0p3`/`p4` (EFS / sec_efs) — that's your IMEI and modem calibration. There's no putting that back.

The bootloader on my A31 is unlocked. If yours isn't, none of this works and unlocking is its own adventure (OEM unlock in dev settings, then it wipes). I'm assuming unlocked from here on.

## Getting the images out of pmbootstrap

I build with `pmbootstrap`. Normally you'd run `pmbootstrap install` and it produces a flashable image, but here's a wrinkle that cost me an afternoon: **`pmbootstrap install` can't complete inside WSL2.** The Microsoft kernel that ships with WSL2 has no loop device support, and `install` needs to loop-mount the image it's assembling. It just falls over.

The way around it is `export` instead of `install`:

```sh
# inside the WSL2 build distro, as the build user (not root)
pmbootstrap install        # if you CAN run it (native Linux, real loop devices)
# ... or, in WSL2:
pmbootstrap export
```

`pmbootstrap export` drops symlinks to the built artifacts in `/tmp/postmarketOS-export/` — you want `boot.img` and the rootfs image. Copy those out to the Windows side and flash from the host. There's also `pmbootstrap export --odin` which packages things up Odin-style if you'd rather go that route.

One important thing about `install` vs `export`: the `boot.img` and the rootfs are a **matched pair**. More on why that matters in gotcha #2.

## Entering download mode

This tripped me up the first time because the A31 doesn't do the "hold a combo while powering on" thing you might expect. The sequence is:

1. Power the phone fully **off**.
2. With the USB cable **unplugged**, hold **Volume Up + Volume Down** together.
3. Keep holding, and **now plug in the USB cable**.

You'll land on the blue warning screen. Press Volume Up to continue into download mode. If you plug the cable in first and then try the combo, you'll usually just boot normally or land in recovery — order matters, cable last.

## Actually flashing

With Heimdall (Linux, or Windows with the Zadig/libusb driver set up):

```sh
heimdall flash --BOOT path/to/boot.img --USERDATA path/to/rootfs.img
```

You can do them separately too:

```sh
heimdall flash --BOOT path/to/boot.img
heimdall flash --USERDATA path/to/rootfs.img
```

If you're on Odin: load the `boot.img` into the AP/BL slot that maps to BOOT and the rootfs into USERDATA. The `--odin` export makes this less fiddly. Honestly on Windows I found getting Heimdall's USB driver right (Zadig, replace the driver on the Gadget Serial / download-mode device) more annoying than just using Odin, but both work.

## The three gotchas

These are the things that actually bit me. Read them before you flash, not after.

### 1. The boot partition is exactly 32 MB, so the kernel must be gzipped

The BOOT partition is 32 MB. Not "about 32." Exactly. The uncompressed kernel `Image` from this build is around 30 MB on its own, and once you wrap it with the initramfs into a `boot.img` it blows straight past 32 MB and the flash either fails or produces something that won't boot.

The fix lives in the kernel APKBUILD: it `gzip -9`'s the `Image` into `vmlinuz` before the boot.img gets assembled. Compressed it's comfortably under the limit. If you're rolling your own kernel and you skip the gzip step, this is the wall you'll hit. Full detail is in [kernel.md](kernel.md), but the short version: kernel goes in gzipped, always.

### 2. Mismatched boot + root = "failed to mount subpartitions"

This one is sneaky because both halves flash fine and the phone tries to boot. Then the initramfs throws **"failed to mount subpartitions"** and you're stuck.

What's happening: pmbootstrap bakes the rootfs's filesystem UUID into the kernel command line in `boot.img`, as `pmos_root_uuid=<uuid>`. The initramfs reads that UUID and goes looking for a matching filesystem on USERDATA. If your `boot.img` came from one `pmbootstrap install`/`export` run and your rootfs came from a *different* run, the UUIDs don't match, the initramfs finds nothing, and it bails.

The clean fix is to **always flash a boot + root pair from the same run.** Don't mix and match.

But sometimes you don't want to re-flash root — say you've already got a working install with data on it and you just rebuilt the kernel. Re-flashing root would wipe everything and trigger the slow first boot again. For that case there's a binary patch. The `pmos_root_uuid` in the cmdline is a fixed-length 36-char string (`8-4-4-4-12`), so you can rewrite it in place without changing the file size:

```sh
python3 tools/patch_boot_uuid.py boot.img <uuid-of-the-running-rootfs>
```

`tools/patch_boot_uuid.py` finds the `pmos_root_uuid=` key in the boot.img, prints the old UUID, and overwrites it with the one you pass. My running root's UUID was `780e3809-7a34-4391-a2a8-1b563f9d53f0`, for example. You can find yours from a booting system (`blkid`), or from the `blkid.txt` the initramfs debug tooling dumps if it can't boot.

To find what UUID a given boot.img currently has baked in (and to sanity-check the header offsets while you're at it), `tools/parse_bootimg.py boot.img` dumps the whole Android boot header including the cmdline. Handy for confirming the patch took.

Once patched, you don't even need download mode for a kernel-only update. From a running pmOS over SSH you can `dd` the patched boot.img straight to the boot partition:

```sh
# on the phone, over SSH — note: the by-name symlinks DON'T exist on pmOS,
# use the raw device node
dd if=patched-boot.img of=/dev/mmcblk0p35 bs=4M
reboot
```

That touches only BOOT, leaves root alone, and reboots fast (no resize). It's how I iterate on the kernel without nuking my install every time.

### 3. The first boot is brutally slow — be patient

The rootfs image you flash is small. On first boot, pmOS runs `resize2fs` to grow it to fill the whole USERDATA partition — in my case that's growing the tiny image up to ~105 GB. That resize blocks boot for *minutes*. No display feedback (graphics on this device are software-rendered on plain fbdev and the panel isn't even driven this early), no SSH, nothing. It looks completely dead.

It isn't. Leave it alone. On my A31 it doesn't reach `sshd` until the **second** boot — the first boot does the resize and then the system comes up properly on the reboot after. So: flash, power on, wait, wait some more, and if it seems hung give it a full reboot before you assume something's wrong. I very nearly re-flashed in a panic the first time when it was just busy resizing.

## If the bootloader rejects the modified boot.img

Mine didn't, but if yours refuses the modified boot because of dm-verity / AVB, you may need to flash a **patched vbmeta with verification disabled**. The A31 has `vbmeta` at `mmcblk0p31` and `vbmeta_samsung` at `mmcblk0p32`. The usual move is an empty vbmeta with the verity + verification flags disabled:

```sh
heimdall flash --VBMETA path/to/vbmeta-disabled.img
```

I'm flagging this as a "might need it" rather than a step, because on my unlocked A31 with everything else in place the stock setup accepted the pmOS boot.img fine. If you hit a verity rejection, that's the lever. If someone has a cleaner read on exactly when the A31 bootloader demands this, open an issue — I'd like to nail it down.

## Tools referenced here

Both live in this repo under `tools/`:

- `tools/patch_boot_uuid.py` — rewrite `pmos_root_uuid` in a boot.img in place (fixed length, safe).
- `tools/parse_bootimg.py` — dump the Android boot header (cmdline, offsets, baked-in UUID) so you can verify what you've got before flashing.
