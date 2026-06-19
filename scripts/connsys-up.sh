#!/bin/sh
# connsys-up.sh - power on the MediaTek connsys chip and bring wlan0 into being.
# Run on the phone as root. Assumes you've already harvested the firmware into
# /lib/firmware (see docs/wifi.md) and built mtinit/mtdaemon (see below).
#
# The full story is in docs/wifi.md; this just runs the sequence.
set +e
HERE="$(cd "$(dirname "$0")/.." && pwd)"

# Build the patched openmttools if they're not built yet (native musl build).
if [ ! -x /tmp/mtinit ] || [ ! -x /tmp/mtdaemon ]; then
    echo "building mtinit/mtdaemon..."
    cc -O2 -o /tmp/mtinit   "$HERE/connsys/mtinit.c"   || exit 1
    cc -O2 -o /tmp/mtdaemon "$HERE/connsys/mtdaemon.c" || exit 1
fi

# mtinit probes the SOC (should report chip 0x6768) and makes the kernel create
# /dev/stpwmt, /dev/wmtWifi and /dev/stpbt.
/tmp/mtinit
[ -e /dev/wmtWifi ] || { echo "no /dev/wmtWifi - mtinit didn't take, check dmesg"; exit 1; }

# mtdaemon downloads the firmware to the chip and powers it on. Keep it running.
/tmp/mtdaemon -p /lib/firmware &
sleep 4

# create the wlan0 interface
echo 1 > /dev/wmtWifi
echo S > /dev/wmtWifi
sleep 1

ip link show wlan0 >/dev/null 2>&1 \
    && echo "wlan0 is up - now run setup-wifi.sh" \
    || echo "no wlan0 yet - check 'dmesg | tail' for the firmware download"
