#!/usr/bin/env python3
# fb-test.py - prove the panel is alive.
#
# mtkfb won't scan out just because you wrote to /dev/fb0 - it only updates the
# panel on an FBIOPAN_DISPLAY ioctl. This fills the framebuffer with a solid
# colour and then pans (yoffset 0) to push it. If the screen turns that colour,
# the display path works. See docs/display.md.
#
# Run on the phone as root.
import os, mmap, struct, fcntl, sys

FBIOGET_VSCREENINFO = 0x4600
FBIOPAN_DISPLAY     = 0x4606

# from the A31 panel (32bpp, RGBA byte order, line stride 4352, virtual 1088x7200)
W, H, STRIDE = 1080, 2400, 4352

# fb_var_screeninfo is big; we only need to read it, tweak yoffset, write it back.
VSI_SIZE = 160  # plenty for this kernel's struct

def main():
    color = (0, 180, 0)  # green by default; pass r g b as args to change
    if len(sys.argv) == 4:
        color = tuple(int(x) for x in sys.argv[1:4])

    fd = os.open("/dev/fb0", os.O_RDWR)
    vsi = bytearray(fcntl.ioctl(fd, FBIOGET_VSCREENINFO, bytes(VSI_SIZE)))

    fb = mmap.mmap(fd, STRIDE * H, mmap.MAP_SHARED, mmap.PROT_WRITE | mmap.PROT_READ)
    px = bytes([color[0], color[1], color[2], 255])  # R,G,B,A
    row = px * W
    for y in range(H):
        fb[y * STRIDE : y * STRIDE + W * 4] = row
    fb.flush()

    # force yoffset = 0 (bytes 16..20 of fb_var_screeninfo are xoffset, yoffset)
    struct.pack_into("<I", vsi, 20, 0)
    fcntl.ioctl(fd, FBIOPAN_DISPLAY, bytes(vsi))

    print(f"filled {W}x{H} with rgb{color} and panned - look at the screen")
    os.close(fd)

if __name__ == "__main__":
    try:
        main()
    except PermissionError:
        sys.exit("run me as root")
