# Getting the screen on

This is the part that made me feel like I'd lost a day to nothing, then feel like a genius for about ten minutes, then realize the trick is kind of dumb. Here's how I got pixels onto the A31's panel with no GPU involved at all. Graphics here are 100% software-rendered on plain fbdev, and none of that is needed to get a working X desktop.

## What the kernel gives us

The downstream MediaTek `mtkfb` driver loads and exposes a regular framebuffer at `/dev/fb0`. No DRM at all — there's no `/dev/dri`, no `/dev/dri/card0`, nothing. That single fact shapes everything below.

The mode it comes up in:

| Property | Value |
|---|---|
| Visible resolution | 1080 x 2400 |
| Bits per pixel | 32 |
| Byte order | RGBA (red byte first) |
| Line stride | 4352 bytes |
| Virtual resolution | 1088 x 7200 (triple-buffered) |

A couple of things to notice. The stride is 4352, not `1080 * 4 = 4320`. mtkfb pads each line out to 1088 pixels (4352 / 4 = 1088), so if you assume a tight 4320-byte stride your image comes out sheared and walking diagonally. Ask me how I know.

The virtual height of 7200 is 2400 * 3 — three full frames stacked vertically. That's the triple buffer, and it's the whole reason the pan trick below works.

Backlight lives at `/sys/class/backlight/panel`. Writing a brightness value there does what you'd expect, and it's independent of whether anything is being scanned out. So early on I had a fully-lit panel still showing the bootloader's Samsung logo, which was confusing until I understood the next bit.

## The quirk that cost me a day

Writing pixels into the mmap'd `/dev/fb0` does nothing on its own. Nothing. You can `memset` the whole buffer bright magenta and the panel keeps showing whatever was last latched — in my case the Samsung boot logo, sitting there mocking me.

mtkfb only actually pushes a frame to the panel when you issue an `FBIOPAN_DISPLAY` ioctl. That's `0x4606`. The pan tells the controller "scan out starting at this yoffset," and that's the moment the panel contents update. Pixels in the buffer are inert until you pan.

So the working pattern is two steps, always:

1. Fill the mmap'd buffer (your frame).
2. Pan to it with `FBIOPAN_DISPLAY`, yoffset 0.

```c
struct fb_var_screeninfo var;
ioctl(fd, FBIOGET_VSCREENINFO, &var);
var.yoffset = 0;               /* triple buffer exists, but I just keep it at 0 */
ioctl(fd, FBIOPAN_DISPLAY, &var);   /* 0x4606 — THIS is what lights the panel */
```

I kept yoffset pinned at 0 rather than cycling through the three buffers. With a software renderer drawing into a single buffer you don't gain much from flipping, and pinning yoffset=0 sidesteps a class of tearing/ordering headaches. If someone wants to do proper page-flipping here, go for it, but for my purposes one buffer plus a forced pan was enough.

`scripts/fb-test.py` is the minimal demonstration of exactly this: open `/dev/fb0`, mmap it, fill it with a color (respecting the 4352 stride and red-first order), then `FBIOPAN_DISPLAY` with yoffset 0. If you're bringing up a different MTK panel and want to confirm the pan-to-scan-out behavior on your hardware, that script is the smallest reproducible thing I've got.

I genuinely don't know the deep reason mtkfb behaves this way — whether the panel handoff is gated on the pan ioctl by design or whether it's some downstream quirk of how Samsung wired the MIPI path. It works, reliably, every time I pan. That's as far as my understanding goes. If you actually know the mechanism, open an issue, I'd love to read it.

## Why no Wayland

Because there's no DRM, the modern compositors are out. weston, wlroots-based stuff, phosh — they all want `/dev/dri` and KMS, and there's nothing for them to talk to. I tried, briefly, and it's a non-starter without writing a fbdev backend that nobody maintains anymore.

fbcon is also not enabled in this kernel, so you don't even get a text console on the panel. First boot is headless over USB (telnet/SSH), which is fine, but it means the screen stays dark until I bring up X myself.

## X11 on fbdev

What works is the old-school path: Xorg with `xf86-video-fbdev` pointed straight at `/dev/fb0`. The fbdev driver knows how to draw into the framebuffer, and X gives me a real (if software-rendered) desktop.

But — and this is the part that ties back to the quirk — `xf86-video-fbdev` writes pixels into the framebuffer and never issues `FBIOPAN_DISPLAY`. Why would it? On a normal framebuffer, writing the bytes *is* the update. On mtkfb it isn't. So X draws a beautiful desktop into a buffer that never gets scanned out, and the panel just sits there.

The fix is a small thing I call the pan daemon (`pandaemon.py`). It's a loop that does nothing but issue `FBIOPAN_DISPLAY` with yoffset 0, about 45 times a second, forever. It doesn't touch pixels at all. It just keeps poking mtkfb to scan out whatever Xorg has currently drawn into the buffer. Crude, a little wasteful, and it works.

The stack I actually run:

- Xorg 1.21 + `xf86-video-fbdev` on `/dev/fb0`
- the pan daemon next to it, hammering `FBIOPAN_DISPLAY` at ~45 Hz
- openbox as the window manager
- xterm
- matchbox-keyboard for an on-screen keyboard (there's no physical one, and touch is the only real input — see the touch doc)

That gives me a usable graphical desktop on the A31. It's all CPU rendering, so don't expect anything smooth or fancy, but openbox + a terminal + an on-screen keyboard is a real, daily-usable X session on the phone's own panel.

## Autostart

I wired the whole thing to come up on boot through an OpenRC `local` service: `/etc/local.d/desktop.start` runs `/opt/start-desktop.sh`, which launches the pan daemon, Xorg, openbox, matchbox-keyboard, and an xterm (and a small audio helper). So after a cold boot the phone lands on the openbox desktop without me touching telnet.

One honest caveat: networking isn't part of this and isn't persistent. The desktop autostarts fine on its own, but if you need the phone online you still re-run the NAT/tethering setup after each reboot. Not a display problem, but it bit me enough times that I'll mention it here too.

## The short version

mtkfb won't show your pixels until you pan. Fill the buffer, `FBIOPAN_DISPLAY` (0x4606) at yoffset 0, done. For a desktop, run Xorg on `xf86-video-fbdev` and keep a tiny daemon panning at ~45 Hz so X's output actually reaches the panel. No DRM, no Wayland, no GPU — just fbdev and a stubborn little loop.
