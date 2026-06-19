#!/usr/bin/env python3
# Parchea el pmos_root_uuid en el cmdline de un Android boot.img (in-place, mismo largo)
import sys
boot = sys.argv[1]
new_uuid = sys.argv[2]  # el UUID del root que esta corriendo
with open(boot, "rb") as f:
    data = bytearray(f.read())
key = b"pmos_root_uuid="
i = data.find(key)
if i < 0:
    print("ERROR: no encontre pmos_root_uuid en el boot.img"); sys.exit(1)
start = i + len(key)
# el UUID son 36 chars (8-4-4-4-12)
old = bytes(data[start:start+36]).decode("ascii", "replace")
print("UUID viejo en boot.img:", old)
if len(new_uuid) != 36:
    print("ERROR: el UUID nuevo no tiene 36 chars"); sys.exit(1)
data[start:start+36] = new_uuid.encode("ascii")
print("UUID nuevo escrito   :", new_uuid)
with open(boot, "wb") as f:
    f.write(data)
print("OK parcheado:", boot)
