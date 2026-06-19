# Building the kernel

This is the part I expected to be straightforward and wasn't. The A31's a downstream MediaTek device, so there's no mainline kernel for it. I build the Samsung/LineageOS fork with pmbootstrap, and getting that 4.14 tree to compile with a modern GCC on Alpine took a fair amount of poking. Here's everything I had to do, with the actual fixes from the APKBUILD so you can reproduce it.

## The source

I use the LineageOS-flavoured fork, not Samsung's raw release:

- Repo: <https://github.com/Galaxy-MT6768/android_kernel_samsung_mt6768>
- Branch: `lineage-19.1`
- Commit: `48d7bfb222e1e722b9c77f78beef056d0011f4cc`
- Version: `4.14.186`

One reason I went with the LineageOS tree instead of stock Samsung is that it already has `CONFIG_UH` (Samsung's RKP, the Real-time Kernel Protection hypervisor) and DEFEX disabled. On a stock Samsung tree those two will fight you hard, and chasing them down is its own multi-day project. The Lineage maintainers already did that work, so I didn't have to.

## Config

The base is `arch/arm64/configs/a31_defconfig`. On top of that I add and remove a handful of options. The committed config in this repo is `device-samsung-a31/config-samsung-a31.aarch64`, but the deltas that matter are:

- `CONFIG_DEVTMPFS=y` and `CONFIG_DEVTMPFS_MOUNT=y` — without these the initramfs has no `/dev`, and pmOS's init can't bring anything up. This is the single most important add. The Android boot flow populated `/dev` differently, so the stock defconfig never needed it.
- Disable `CONFIG_COMPAT_VDSO` — it breaks the build on this toolchain.
- Disable `CONFIG_IKHEADERS` — the in-kernel headers blob just bloats things and I don't use it.
- I originally disabled `CONFIG_TOUCHSCREEN_IST4050` to get the thing to compile at all, then re-enabled it once I'd fixed the driver. See [touch.md](touch.md) for that story.

## Building with pmbootstrap

The whole thing is a cross-compile driven by pmbootstrap (aarch64 target, x86_64 host). The package is `device-samsung-a31/APKBUILD` in this repo. Build it the normal pmbootstrap way:

```sh
pmbootstrap build linux-samsung-a31
```

One thing I learned the annoying way: run pmbootstrap as a normal user with the full path to the script, not as root and not via a flaky PATH symlink. On my setup that's `/home/pmos/pmbootstrap/pmbootstrap.py`. Running it as root gives it the wrong `$HOME` and things go sideways.

## What `prepare()` actually fixes

This is the meat. The 4.14 tree predates the GCC I'm building with by years, and Samsung's build system assumes a glibc host. The `prepare()` function papers over both. Going through it in order:

### 1. Keep the kernel's own compiler-gcc.h, but defang the inline macro

```sh
REPLACE_GCCH=0 . downstreamkernel_prepare
sed -i 's/always_inline, unused/unused/' include/linux/compiler-gcc.h
```

pmbootstrap's `downstreamkernel_prepare` normally swaps in a generic `compiler-gcc.h`. On this tree that triggers a `#error "Don't include <linux/compiler-gcc.h> directly..."` and the build dies immediately, so I set `REPLACE_GCCH=0` to keep the kernel's own copy.

But then the kernel's `compiler-gcc.h` defines `inline` to carry `__attribute__((always_inline))`. With a modern GCC that turns into `inlining failed in call to always_inline ... function body not available` on functions GCC decides it can't actually inline, and the build dies a different way. The `sed` strips the forced `always_inline` out of the macro (keeping `__gnu_inline`/`unused`), which lets GCC make its own inlining decisions. Combined with the gnu89 flag below, this is what finally got it linking.

### 2. Use the kernel's own dtc instead of Samsung's prebuilt

```sh
sed -i 's,$(srctree)/scripts/dtc/dtc_overlay,$(DTC),g' scripts/Makefile.lib
```

Samsung ships a prebuilt `dtc_overlay` binary in the tree for building DT overlays. It's a glibc ELF built for their build servers and it simply won't execute on musl/Alpine. The kernel builds its own `dtc` anyway (`$(DTC)`), and a recent enough dtc handles overlays with `-@`, so I just point the overlay rule at the freshly-built one. After this the DTBs build natively.

### 3. gnu89 inline semantics

```sh
KCFLAGS="-fgnu89-inline"
```

(This one lives in `build()`, not `prepare()`, but it's part of the same fight.) The 4.14 code assumes the old GNU89 inline semantics. Modern GCC defaults to C99 inline, which changes how `extern inline` / bare `inline` functions emit, and you get a pile of duplicate/missing-symbol link errors. `-fgnu89-inline` puts it back the way the code expects.

### 4. The connsys fragment-size fix

```sh
sed -i 's/#define DEFAULT_PATCH_FRAG_SIZE (1000)/#define DEFAULT_PATCH_FRAG_SIZE (256)/' \
    drivers/misc/mediatek/connectivity/wmt_drv/common_main/core/wmt_ic_soc.c
```

This one isn't a compile fix at all, it's a runtime WiFi fix that happens to live in the kernel. The connsys firmware download to the MT6768 chip is chunked into fragments. At the stock 1000-byte fragment size, the BTIF vFIFO saturates partway through the firmware download (around fragment 47) and the STP transport window never recovers, so the MCU never boots and you get no `wlan0`. Shrinking the fragment to 256 bytes lets the connsys MCU drain the FIFO in time, the full `patch_mcu` downloads, calibration runs, and WiFi comes up. The whole connsys saga is in [wifi.md](wifi.md) — this define is the kernel half of it.

### 5. The IST4050 touch VLA fix

```sh
sed -i 's/char buff\[msg_len\] = {0, };/char buff[128] = {0, };/' \
    drivers/input/touchscreen/imagis/ist4050/ist40xx_sec.c
```

The Imagis IST4050 touch driver has one variable-length array that's initialized at declaration (`char buff[msg_len] = {0, };`). Modern GCC rejects a VLA with an initializer outright (`variable-sized object may not be initialized`). The many `char msg[msg_len];` declarations without an initializer compile fine — it's only the one with `= {0, }` that breaks. Pinning it to a fixed `buff[128]` is enough; `msg_len` never exceeds that here. This is what let me turn `CONFIG_TOUCHSCREEN_IST4050` back on. Full details in [touch.md](touch.md).

## The boot-partition gotcha: you must gzip the kernel

This one cost me a flash cycle before I figured it out. The A31's boot partition (`mmcblk0p35`) is exactly 32 MB. The uncompressed `Image` this kernel produces is around 30 MB on its own, and once the ramdisk and DTBs get packed into a boot.img alongside it, it overflows 32 MB and the flash either fails or produces an unbootable image. Samsung's LK bootloader expects a gzip-compressed kernel anyway — the stock kernel was about 13 MB gzipped.

So `package()` throws away the uncompressed vmlinuz that `downstreamkernel_package` installs and replaces it with a gzip'd one:

```sh
package() {
    downstreamkernel_package "$builddir" "$pkgdir" "$_carch" \
        "$_flavor" "$_outdir"
    gzip -9 -c "$builddir/$_outdir/arch/$_carch/boot/Image" > "$pkgdir"/boot/vmlinuz
    make dtbs_install O="$_outdir" ARCH="$_carch" \
        INSTALL_DTBS_PATH="$pkgdir"/boot/dtbs
}
```

`gzip -9` brings it back down comfortably under the limit. If you skip this and wonder why your perfectly-good kernel won't flash or boot, this is almost certainly why.

## Result

With all of that, the kernel compiles, flashes, and boots:

```
Linux samsung-a31 4.14.186 #2-postmarketOS aarch64
```

`/dev/fb0` shows up (the downstream mtkfb framebuffer — graphics are software-rendered on plain fbdev, see [display.md](display.md)), `/dev/input/event0-4` are present, and from there everything else in this repo builds on top.

If you find a cleaner way to make this tree compile — especially if there's a less hacky route around the `compiler-gcc.h` / `always_inline` mess — open an issue, I'd genuinely like to know.
