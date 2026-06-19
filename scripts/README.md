# scripts

Small helpers I run on the phone (as root) during bring-up. They read any config
from the environment, so there are no passwords baked in. Adapt paths to your unit
if needed.

| Script | What it does |
|---|---|
| `fb-test.py` | Fill `/dev/fb0` and pan it, to prove the panel works (the FBIOPAN trick). |
| `connsys-up.sh` | Build/run `mtinit` + `mtdaemon`, power the chip, create `wlan0`. |
| `setup-wifi.sh` | Associate + DHCP. `WIFI_SSID` / `WIFI_PSK` from the environment. |
| `bt-hci.py` | Open `/dev/stpbt`, send an HCI Reset, read the status back. |
| `speaker.sh` | Route audio out the speaker (the amixer/DAPM dance). |

Rough order on a fresh boot:

```sh
python3 fb-test.py        # screen on?
./connsys-up.sh           # WiFi/BT chip up, wlan0 created
WIFI_SSID=net WIFI_PSK=pw ./setup-wifi.sh
./speaker.sh              # sound
python3 bt-hci.py         # bluetooth radio alive (optional)
```

Each one maps to a doc under [`../docs/`](../docs/) if you want the why.
