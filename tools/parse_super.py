#!/usr/bin/env python3
# Parser minimo de metadata liblp del super de Android (formato LpMetadata v1.x)
import struct
DEV = "/dev/mmcblk0p45"
f = open(DEV, "rb")
# geometria primaria en offset 4096 (tras 4096 reservados)
f.seek(4096)
geom = f.read(4096)
gmagic = struct.unpack("<I", geom[0:4])[0]
metadata_max_size = struct.unpack("<I", geom[40:44])[0]
slot_count = struct.unpack("<I", geom[44:48])[0]
print("geom magic=0x%x max_size=%d slots=%d" % (gmagic, metadata_max_size, slot_count))
# header de metadata: tras reservados(4096)+geom primaria(4096)+geom backup(4096)=12288
f.seek(4096 * 3)
hdr = f.read(metadata_max_size)
hmagic = struct.unpack("<I", hdr[0:4])[0]
header_size = struct.unpack("<I", hdr[8:12])[0]
print("hdr magic=0x%x header_size=%d" % (hmagic, header_size))
def desc(off):
    return struct.unpack("<III", hdr[off:off+12])  # offset, num_entries, entry_size
part_off, part_n, part_es = desc(80)
ext_off, ext_n, ext_es = desc(92)
tables = hdr[header_size:]
extents = []
for i in range(ext_n):
    b = ext_off + i*ext_es
    num_sectors = struct.unpack("<Q", tables[b:b+8])[0]
    ttype = struct.unpack("<I", tables[b+8:b+12])[0]
    tdata = struct.unpack("<Q", tables[b+12:b+20])[0]
    extents.append((num_sectors, ttype, tdata))
print("=== logical partitions ===")
for i in range(part_n):
    b = part_off + i*part_es
    name = tables[b:b+36].split(b'\0')[0].decode("ascii", "replace")
    fei = struct.unpack("<I", tables[b+40:b+44])[0]
    nex = struct.unpack("<I", tables[b+44:b+48])[0]
    for e in range(fei, fei+nex):
        ns, tt, td = extents[e]
        print("%-12s  dmsetup: '0 %d linear /dev/mmcblk0p45 %d'  (type=%d)" % (name, ns, td, tt))
