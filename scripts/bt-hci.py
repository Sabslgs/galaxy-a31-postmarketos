#!/usr/bin/env python3
# bt-hci.py - minimal Bluetooth smoke test over /dev/stpbt.
#
# Once connsys is up, opening /dev/stpbt powers the BT side on and the controller
# speaks raw HCI (H4) over the char device. This sends an HCI Reset and reads the
# Command Complete back. If you get status 0x00, the radio is alive.
#
# There's no BlueZ here, so this is deliberately low-level. Run on the phone as
# root. Note: opening /dev/stpbt can briefly drop the USB gadget (and your SSH),
# so if you're remote, run this detached and write the output to a file.
import os, struct, sys, time

DEV = "/dev/stpbt"

# H4: 0x01 = command packet. Opcode 0x0c03 = HCI_Reset, 0 params.
HCI_RESET = bytes([0x01, 0x03, 0x0c, 0x00])

def main():
    fd = os.open(DEV, os.O_RDWR)
    print(f"opened {DEV}")
    # the act of opening fires WMT func_on(BT); give it a moment
    time.sleep(0.5)

    os.write(fd, HCI_RESET)
    print("sent HCI_Reset")

    # read the event back (0x04 = event packet)
    time.sleep(0.3)
    data = os.read(fd, 64)
    print("got:", data.hex())

    # a Command Complete for Reset looks like 04 0e 04 01 03 0c <status>
    if len(data) >= 7 and data[0] == 0x04 and data[1] == 0x0e:
        status = data[6]
        print("HCI Reset status:", "OK (0x00)" if status == 0 else hex(status))
    else:
        print("unexpected event - check dmesg")
    os.close(fd)

if __name__ == "__main__":
    try:
        main()
    except PermissionError:
        sys.exit("run me as root")
    except FileNotFoundError:
        sys.exit("no /dev/stpbt - bring connsys up first (connsys-up.sh)")
