#!/usr/bin/env python3
# Parser de Android boot image (v0/v1/v2) + deteccion de header MediaTek.
# Uso: python parse_bootimg.py a31_boot.img
import sys, struct, math

path = sys.argv[1] if len(sys.argv) > 1 else "a31_boot.img"
with open(path, "rb") as f:
    d = f.read()

if d[0:8] != b"ANDROID!":
    print("OJO: no hay magic 'ANDROID!' al inicio. Primeros 8 bytes:", d[0:8])

(kernel_size, kernel_addr, ramdisk_size, ramdisk_addr,
 second_size, second_addr, tags_addr, page_size,
 header_version, os_version) = struct.unpack("<10I", d[8:48])

name = d[48:64].split(b"\x00")[0].decode("latin1")
cmdline = d[64:576].split(b"\x00")[0].decode("latin1")
extra = d[608:1632].split(b"\x00")[0].decode("latin1")

print("=== Android boot image header ===")
print("header_version :", header_version)
print("page_size      :", page_size)
print("kernel_size    :", kernel_size)
print("ramdisk_size   :", ramdisk_size)
print("second_size    :", second_size)
print("os_version raw :", hex(os_version))
print("name           :", repr(name))
print("cmdline        :", repr(cmdline))
print("extra_cmdline  :", repr(extra))

print("\n--- direcciones absolutas ---")
print("kernel_addr  :", hex(kernel_addr))
print("ramdisk_addr :", hex(ramdisk_addr))
print("second_addr  :", hex(second_addr))
print("tags_addr    :", hex(tags_addr))

if header_version >= 1:
    off = 1632
    rec_dtbo_size, = struct.unpack("<I", d[off:off+4]); off += 4
    rec_dtbo_off,  = struct.unpack("<Q", d[off:off+8]); off += 8
    header_size,   = struct.unpack("<I", d[off:off+4]); off += 4
    print("recovery_dtbo_size :", rec_dtbo_size)
    print("header_size        :", header_size)
    if header_version >= 2:
        dtb_size, = struct.unpack("<I", d[off:off+4]); off += 4
        dtb_addr, = struct.unpack("<Q", d[off:off+8]); off += 8
        print("dtb_size :", dtb_size)
        print("dtb_addr :", hex(dtb_addr))

base = kernel_addr - 0x00008000
print("\n--- deviceinfo (asumiendo kernel_offset = 0x00008000) ---")
print("deviceinfo_flash_offset_base    =", "0x%08x" % base)
print("deviceinfo_flash_offset_kernel  =", "0x%08x" % (kernel_addr - base))
print("deviceinfo_flash_offset_ramdisk =", "0x%08x" % (ramdisk_addr - base))
print("deviceinfo_flash_offset_second  =", "0x%08x" % (second_addr - base))
print("deviceinfo_flash_offset_tags    =", "0x%08x" % (tags_addr - base))
print("deviceinfo_flash_pagesize       =", page_size)
print("deviceinfo_header_version       =", header_version)
print("deviceinfo_kernel_cmdline       =", repr(cmdline))

def mtk(off):
    c = d[off:off+8]
    if len(c) >= 4 and c[0] == 0x88 and c[1] == 0x16 and c[2] == 0x88 and c[3] == 0x58:
        size, = struct.unpack("<I", d[off+4:off+8])
        nm = d[off+8:off+40].split(b"\x00")[0].decode("latin1")
        return (True, size, nm)
    return (False, None, None)

koff = page_size
roff = page_size + math.ceil(kernel_size / page_size) * page_size
print("\n--- headers MediaTek (label kernel/ramdisk) ---")
ok, sz, nm = mtk(koff)
print("kernel  @", hex(koff), "-> MTK:", ok, "label:", repr(nm), "size:", sz)
ok2, sz2, nm2 = mtk(roff)
print("ramdisk @", hex(roff), "-> MTK:", ok2, "label:", repr(nm2), "size:", sz2)
if ok or ok2:
    print("=> deviceinfo_bootimg_mtk_label_kernel  =", repr(nm))
    print("=> deviceinfo_bootimg_mtk_label_ramdisk =", repr(nm2))
