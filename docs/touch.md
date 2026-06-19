# Touchscreen

The A31's panel is a `sec_touchscreen` driven by an Imagis IST4050 controller. Once the driver loads it shows up as `/dev/input/event3`, and that's the node everything else (libinput, evtest, my poking around) talks to.

The nice surprise: it reports absolute coordinates straight in panel pixels. X runs 0..1079, Y runs 0..2399, which is exactly the 1080x2400 display. So there's no scaling or calibration math to do. Whatever the controller reports maps 1:1 onto a framebuffer pixel. I half expected to fight a transform matrix and I just... didn't have to.

## The one bug that blocked the driver

When I first got the kernel building I had touch turned off, because the IST4050 driver wouldn't compile under a modern GCC. The error:

```
variable-sized object may not be initialized
```

It comes from a variable-length array with an initializer in `drivers/input/touchscreen/imagis/ist4050/ist40xx_sec.c`:

```c
char buff[msg_len] = {0, };
```

`msg_len` isn't a compile-time constant, so `buff` is a VLA, and C doesn't let you brace-initialize a VLA. Samsung's ancient toolchain apparently let it slide; the GCC I'm cross-compiling with does not. I disabled `TOUCHSCREEN_IST4050` just to get a booting kernel, shipped that, and came back to touch later.

The fix is dumb and one line. `msg_len` in practice is well under 128 bytes, so I just give `buff` a fixed size big enough to cover it and let the zero-init stand. I do it as a `sed` in the kernel APKBUILD's `prepare()` rather than patching the tree, so it survives re-fetching the source:

```sh
# IST4050 touchscreen: fix the one VLA-with-initializer that modern GCC rejects
# ("variable-sized object may not be initialized"), so we can enable touch.
sed -i 's/char buff\[msg_len\] = {0, };/char buff[128] = {0, };/' \
	drivers/input/touchscreen/imagis/ist4050/ist40xx_sec.c
```

After that, flip `CONFIG_TOUCHSCREEN_IST4050=y` (or `=m`) back on in the config and rebuild. It's `linux-samsung-a31/APKBUILD`, in `prepare()`, if you want to see it in context.

If anyone knows the proper fix upstream wanted here (probably a `kmalloc` or a sane fixed bound that's actually documented), open an issue. 128 works because the messages are small, not because I found a guarantee that they always will be.

## Wiring it into X

There's no DRM on this thing, so the desktop is plain Xorg on fbdev, software-rendered. Touch goes in through `xf86-input-libinput`, which picks up `event3` on its own once the driver's there. No custom config file, libinput just finds the absolute-pointer device and treats it as touch. Tapping moves the cursor and clicks, which for an openbox + on-screen-keyboard setup is all I need.

## Checking it works

`evtest` is the quick sanity check. Pick `/dev/input/event3` (it identifies as `sec_touchscreen`) and drag a finger across the glass:

```
$ evtest /dev/input/event3
...
Event: type 3 (EV_ABS), code 53 (ABS_MT_POSITION_X), value 540
Event: type 3 (EV_ABS), code 54 (ABS_MT_POSITION_Y), value 1200
```

If you see ABS_MT_POSITION_X/Y events with values that top out around 1079 and 2399, the controller's alive and the coordinate range is what you expect. That was the moment I knew the VLA fix had actually paid off.
