#!/bin/sh
# setup-wifi.sh - associate and get an IP, once connsys is up and wlan0 exists.
# Run on the phone as root. Pass the network in via the environment so nothing
# secret ends up in a file:
#
#   WIFI_SSID="My Network" WIFI_PSK="mypassword" ./setup-wifi.sh
#
# This kernel has no WEXT, so wpa_supplicant must use the nl80211 driver.
set -e
: "${WIFI_IFACE:=wlan0}"
[ -z "$WIFI_SSID" ] && { echo "set WIFI_SSID and WIFI_PSK in the environment"; exit 1; }

conf=/tmp/wpa.$$.conf
trap 'rm -f "$conf"' EXIT
wpa_passphrase "$WIFI_SSID" "$WIFI_PSK" > "$conf"

ip link set "$WIFI_IFACE" up
pkill -x wpa_supplicant 2>/dev/null || true
sleep 1
wpa_supplicant -B -i "$WIFI_IFACE" -c "$conf" -D nl80211

i=0
while [ $i -lt 15 ]; do
    if wpa_cli -i "$WIFI_IFACE" status 2>/dev/null | grep -q "wpa_state=COMPLETED"; then
        echo "associated"
        break
    fi
    i=$((i + 1)); sleep 1
done

dhcpcd -4 -t 20 "$WIFI_IFACE"
ip -4 addr show "$WIFI_IFACE" | grep inet || echo "no address yet"
