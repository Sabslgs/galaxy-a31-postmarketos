#!/usr/bin/env python3
# Parser minimo de metadata de Android "super" (dynamic partitions / liblp)
# Print each logical partition in `super` with its offset and size (in 512-byte sectors).
import struct, sys
data = open(sys.argv[1], "rb").read()

GEOM_OFFSET = 4096          # PARTITION_RESERVED_BYTES
GEOM_MAGIC = 0x616C4467
HDR_MAGIC  = 0x414C5030

# geometry
g = data[GEOM_OFFSET:GEOM_OFFSET+52]
magic, struct_size = struct.unpack_from("<II", g, 0)
if magic != GEOM_MAGIC:
    print("geometry magic mal:", hex(magic)); sys.exit(1)
metadata_max_size, metadata_slot_count, logical_block_size = struct.unpack_from("<III", g, 4+4+32)
print(f"# geometry ok, max_size={metadata_max_size} slots={metadata_slot_count} lbs={logical_block_size}")

# metadata header: despues de 2 copias de geometry (cada una 4096)
META_OFFSET = GEOM_OFFSET + 4096*2   # 12288
h = data[META_OFFSET:META_OFFSET+256]
hmagic, major, minor, header_size = struct.unpack_from("<IHHI", h, 0)
if hmagic != HDR_MAGIC:
    # probar slot por offset alternativo
    print("header magic mal en", META_OFFSET, ":", hex(hmagic)); sys.exit(1)
# tras magic(4)+major(2)+minor(2)+header_size(4)+header_checksum(32)+tables_size(4)+tables_checksum(32)
off = 4+2+2+4+32+4+32
# 4 table descriptors: partitions, extents, groups, block_devices  (cada uno: offset u32, num u32, entry_size u32)
def desc(i):
    o = off + i*12
    return struct.unpack_from("<III", h, o)  # (offset, num_entries, entry_size)
p_off, p_num, p_sz = desc(0)
e_off, e_num, e_sz = desc(1)
print(f"# partitions: num={p_num} size={p_sz} ; extents: num={e_num} size={e_sz}")

tables_base = META_OFFSET + header_size
# partitions
parts = []
for i in range(p_num):
    base = tables_base + p_off + i*p_sz
    name = data[base:base+36].split(b"\x00")[0].decode("latin1")
    attributes, first_extent_index, num_extents, group_index = struct.unpack_from("<IIII", data, base+36)
    parts.append((name, first_extent_index, num_extents))
# extents
extents = []
for i in range(e_num):
    base = tables_base + e_off + i*e_sz
    num_sectors, target_type, target_data, target_source = struct.unpack_from("<QIQI", data, base)
    extents.append((num_sectors, target_type, target_data))

print("# === PARTICIONES ===")
for name, fei, ne in parts:
    total = 0
    first_off = None
    for j in range(fei, fei+ne):
        ns, tt, td = extents[j]
        if first_off is None: first_off = td
        total += ns
    print(f"{name}\toffset_sectors={first_off}\tsize_sectors={total}\toffset_bytes={ (first_off or 0)*512 }\tsize_MB={ total*512//1024//1024 }")
