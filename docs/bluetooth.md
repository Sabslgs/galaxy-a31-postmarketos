# Bluetooth

Short version: Bluetooth works at a smoke-test level. I can power the controller on, talk raw HCI to it from userspace, run an inquiry, and read back the names of nearby devices. What I don't have yet is a real BlueZ `hci0` you could pair speakers with. That part has a nasty trap in it, which I'll get to.

Read [wifi.md](wifi.md) first. This isn't optional. BT and WiFi sit on the same MediaTek connsys chip behind the same WMT/STP firmware, so until connsys is brought up the way the WiFi doc describes (firmware blobs in `/lib/firmware`, the mtdaemon-equivalent running, `/dev/stpwmt` alive), Bluetooth has no chance. Once connsys is up, though, BT is almost free.

## Turning it on

The whole "power on" step is: open the char device.

```sh
# /dev/stpbt is char major 192
exec 3<> /dev/stpbt
```

Opening `/dev/stpbt` fires the WMT `func_on(BT)` sequence inside the built-in driver. When it works you see this in dmesg:

```
BT_open: WMT turn on BT OK
```

That line is the entire handshake from my side. No `.ko` to load, no `hciconfig hci0 up`, no firmware path to pass. The WMT/STP combo is built into the kernel (`=y`), the BT firmware is the same connsys blob WiFi already pulled in, and the open() call is what triggers the controller to come alive.

After that, `/dev/stpbt` is just a stream of HCI in the H4 (UART) framing. You write command packets to the fd, you read event packets back. There's no BlueZ in the path at all here, which is the point.

## Talking HCI directly

H4 framing is dead simple: one type byte, then the payload. `0x01` is a command, `0x04` is an event. A command packet is `[0x01][opcode_lo][opcode_hi][param_len][params...]`, where the opcode is `(OGF << 10) | OCF` little-endian.

I confirmed the controller end to end with a small Python script ([`hciscan.py`](../scripts/hciscan.py) in my work tree) that does exactly this against the raw fd:

```python
def send_cmd(ogf, ocf, params=b""):
    opcode = (ogf << 10) | ocf
    pkt = bytes([0x01]) + struct.pack("<H", opcode) + bytes([len(params)]) + params
    os.write(fd, pkt)
```

The sequence I ran, and what came back:

- **HCI Reset** (OGF 0x03, OCF 0x0003): wrote it, read back a Command Complete event with **status 0x00**. The controller is awake and answering.
- **HCI Inquiry** (OGF 0x01, OCF 0x0001) with LAP `0x9e8b33`, inquiry length `0x10` (~20s): I got Inquiry Result events. It picked up my own phone at `C8:BD:4D:A3:3C:A9` and another nearby device.
- **Remote Name Request** (OGF 0x01, OCF 0x0019) per address: came back with the friendly names. One resolved to "S26 Ultra". So the controller does inquiry, scanning, and name resolution. The radio side is genuinely fine.

That's the good news. Raw HCI over `/dev/stpbt` from userspace just works.

## The gotcha: do not naively bridge into BlueZ

This one cost me a few reboots before I understood it, so heed the warning.

The obvious next step is to expose `/dev/stpbt` to BlueZ as an `hci0` by creating a virtual HCI device with the kernel's `hci_vhci` (`/dev/vhci`) and shuttling bytes between the two. Don't do the naive version of this.

If you create a **non-RAW** vhci device, the kernel BT core kicks off its own controller auto-init the moment `hci0` registers, and on this downstream stack that auto-init **panics the kernel**. Hard panic, watchdog reboots the phone. It's not a hang you can recover from, the device just resets.

A **RAW** vhci (open `/dev/vhci`, send the create-device control with opcode `0x80`) does not panic, because RAW mode tells the core to keep its hands off and not auto-init. But that's also the catch: it won't auto-init, so you don't get a usable `hci0` for free either. You've just made an inert node.

So both naive paths fail, one loudly and one quietly:

| Approach | Result |
|---|---|
| non-RAW `hci_vhci` (`/dev/stpbt` → `hci0`) | kernel **panic**, watchdog reboot |
| RAW vhci (opcode `0x80`) | no panic, but no auto-init, unusable as-is |
| raw HCI straight to `/dev/stpbt` from userspace | **works** |

The reliable path is the third one: skip BlueZ and the vhci entirely, talk HCI to `/dev/stpbt` yourself.

`CONFIG_BT_HCIVHCI` is actually built into my kernel (r4), so the plumbing exists. The blocker is purely that panic-on-autoinit behavior in the downstream BT core. To get a proper BlueZ `hci0` you'd need a small userspace shim that opens `/dev/stpbt`, presents a RAW vhci to BlueZ, and drives the init handshake itself instead of letting the core do it. I haven't written that yet. If you build one, please open an issue or a PR. I'd love to pair an actual headset.

## Practical annoyance: SSH drops when BT powers on

My main access into the phone is SSH over the USB gadget. The instant I open `/dev/stpbt`, the USB link blips and my SSH session dies for a moment. Powering the BT side of connsys toggles something on the USB path. It reconnects on its own, but any foreground command you were running over that session gets cut off mid-write, and you lose the output.

The workaround is to run BT stuff **detached and writing to a file**, then read the file after the session comes back:

```sh
# don't run this in the foreground over SSH
setsid python3 hciscan.py >/dev/null 2>&1 &
# ... session may blip here ...
cat /tmp/btresult.txt
```

`hciscan.py` already writes its results to `/tmp/btresult.txt` with an fsync after every line for exactly this reason. Learned that the hard way after staring at a dead terminal wondering if the scan had even run.

## Status

Bluetooth is "works" in the honest, narrow sense: the controller powers on via `/dev/stpbt`, speaks raw HCI-H4, and I've confirmed Reset, Inquiry, and Remote Name Request end to end against real nearby devices. No BlueZ integration, no pairing, no audio profile, none of that. Raw HCI is proven; a usable `hci0` is future work, gated on writing that userspace shim around the autoinit panic.

(Graphics, for the record, are software-rendered on plain fbdev throughout this port. Nothing here touches the GPU.)
