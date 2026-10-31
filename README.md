# Guest Wi-Fi password for the FRITZ!Box

Sets a new random password for the guest Wi-Fi of a FRITZ!Box via TR-064.
It then creates a Wi-Fi QR code (PNG and ASCII in the terminal) and a JSON file with the
SSID, the password and the QR payload, which `publish_mqtt.py` publishes via MQTT for an e-paper display (see below).

## Requirements

- [uv](https://docs.astral.sh/uv/). It installs the dependencies (`fritzconnection`, `qrcode[pil]`) automatically.
- A FRITZ!Box with access for applications enabled (Home Network > Network > Network Settings)
  and a user with the "FRITZ!Box Settings" permission.

## Usage

```sh
chmod +x guest_wifi.py
FRITZ_USER=... FRITZ_PASSWORD=... ./guest_wifi.py
```

| Option | Meaning |
|---|---|
| `--password "..."` | your own password instead of a random one (8–63 characters) |
| `--index N` | use `WLANConfigurationN` directly instead of automatic detection |
| `--ssid NAME` | abort without changes if the network has another SSID (default: `FRITZ_GUEST_SSID`) |
| `--address IP` | address of the FRITZ!Box (default: `FRITZ_ADDRESS` or `192.168.178.1`) |
| `--json-file PATH` | target of the JSON output (default: `guest-wifi.json`) |
| `--qr-file PATH` | target of the QR code PNG (default: `guest-wifi-qr.png`) |

If the guest Wi-Fi is not found automatically, the script lists the available networks.
Pass the matching index with `--index` (often 3).

`list_wlans.py` lists the SSIDs of all `WLANConfiguration` services without changing anything.
With `--index`, set `FRITZ_GUEST_SSID` (or `--ssid`) as well, so a wrong index cannot change the main Wi-Fi.

`show_qr.py` shows the current QR code, SSID and password in the terminal
(reads `~/.local/state/guest-wifi/guest-wifi.json`, or the file given as argument).

## Weekly via systemd (user unit)

```sh
install -Dm 755 guest_wifi.py ~/.local/bin/guest_wifi.py
install -Dm 644 -t ~/.config/systemd/user/ guest-wifi.service guest-wifi.timer
install -Dm 600 guest-wifi.env.example ~/.config/guest-wifi.env   # fill in the values
systemctl --user daemon-reload
systemctl --user enable --now guest-wifi.timer
loginctl enable-linger "$USER"   # run the timer without a logged-in session
```

The timer runs on Wednesdays at 04:00. Adjust the index in `guest-wifi.service` (`--index 3`)
if needed. If the run fails, for example because the network is not up yet, systemd retries
every 5 minutes, up to 5 times. The output does not go to the journal, so the password does not show up there.
The files `guest-wifi.json` and `guest-wifi-qr.png` are written to `~/.local/state/guest-wifi/`
(created by systemd, readable only by you).

### Test run

Start the service once by hand instead of waiting for Wednesday. This really changes the
guest Wi-Fi password.

```sh
systemctl --user start guest-wifi.service
systemctl --user status guest-wifi.service --no-pager   # expect status=0/SUCCESS
ls -l ~/.local/state/guest-wifi/
systemctl --user list-timers guest-wifi.timer           # next scheduled run
```

If it fails, `journalctl --user -u guest-wifi.service` shows the error.

### Update

After changing the scripts or the units, install them again and reload systemd:

```sh
install -Dm 755 -t ~/.local/bin/ guest_wifi.py publish_mqtt.py send_image.py
install -Dm 644 -t ~/.config/systemd/user/ guest-wifi.service guest-wifi.timer
systemctl --user daemon-reload
```

### Uninstall

```sh
systemctl --user disable --now guest-wifi.timer
rm ~/.local/bin/{guest_wifi,publish_mqtt,send_image}.py ~/.config/systemd/user/guest-wifi.{service,timer}
rm -r ~/.config/guest-wifi.env ~/.local/state/guest-wifi
systemctl --user daemon-reload
loginctl disable-linger "$USER"   # only if no other user services need it
```

## E-paper display

`firmware/` is a PlatformIO project for the
[CrowPanel ESP32 4.2" E-Paper](https://www.elecrow.com/wiki/CrowPanel_ESP32_E-Paper_4.2-inch_HMI_Display.html)
(400×300, ESP32-S3). It joins the home Wi-Fi, shows the QR code, SSID and password,
and gets new data from an MQTT broker (retained message). The last data is kept in flash and drawn again after a restart.
Once a day the panel is refreshed (white, then the image again) so it does not burn in.
Pressing the rotary switch turns the content by 180 degrees, for example when the display stands
upside down; the setting is kept in flash as well.

The panel driver is written for PCB **V1.2A (green round sticker on the back)**.
The older revision has another display controller and needs another driver.

### Flash the firmware

```sh
cp firmware/include/secrets.h.example firmware/include/secrets.h   # fill in the values
cd firmware && pio run -t upload && pio device monitor
```

The serial monitor shows the connection state. Until it gets data, the display says so.
Rotary switch: turn for the debug screens (status, event log), press to rotate the image by 180°.
EXIT: back from the debug screens. HOME: switch between the guest Wi-Fi and an image (see below).
The display also publishes its state (without the password)
to `guestpass/display/status`.

### Publish the data

Add `MQTT_HOST` (and optionally `MQTT_PORT`, `MQTT_USER`, `MQTT_PASSWORD`, `MQTT_TOPIC`) to
`~/.config/guest-wifi.env` (see `guest-wifi.env.example`) and install `publish_mqtt.py` next to `guest_wifi.py`:

```sh
install -Dm 755 publish_mqtt.py ~/.local/bin/publish_mqtt.py
```

After each password change, `guest_wifi.py` publishes the new data as a retained message
(`--mqtt-retries N` attempts one minute apart, the service uses 10).
A failed update does not fail the run, so systemd does not change the password again.
To publish the current data by hand, without changing the password:

```sh
set -a; . ~/.config/guest-wifi.env; set +a
./publish_mqtt.py
```

The password is stored on the broker, so give it a login and an ACL: only the publisher may write
`guestpass/wifi` and `guestpass/image`, only the display may read them and write `guestpass/display/#`.
There is no TLS yet.

### Send an image

`send_image.py` scales any image (PNG, JPEG, ...) to 400×300, dithers it to black and white and publishes it
as a retained message to `guestpass/image`. The display keeps it in flash.

```sh
set -a; . ~/.config/guest-wifi.env; set +a
./send_image.py photo.jpg                    # fit with white borders
./send_image.py logo.png --cover --no-dither # fill and crop, hard threshold for text and line art
./send_image.py photo.jpg --preview out.png  # only write the result, send nothing
./send_image.py --clear                      # remove the image
```

HOME switches between the image and the guest Wi-Fi. New data for the screen that is not shown is
only saved: in guest Wi-Fi view a new image waits for HOME, in image view a new password does too.

### Battery operation

By default the display stays connected (Wi-Fi modem sleep), which takes roughly 40–100 mA.
With `#define SLEEP_INTERVAL_S 900` in `secrets.h` it sleeps instead: every 900 s it wakes up, reads the
retained message, redraws only if it changed, publishes its status and sleeps again (about 1–3 mA on
average, to be measured on the board). Any key wakes it up too and keeps it awake while the debug screen is
open. A new password appears on the display after at most one interval.

### Back to the factory firmware

A full flash dump was made before the first upload (8 MB, read at 460800 baud, the CH340 fails at 921600):

```sh
uvx esptool --port /dev/ttyUSB0 --baud 460800 read-flash 0 0x800000 backup.bin    # backup
uvx esptool --port /dev/ttyUSB0 --baud 460800 write-flash 0 backup.bin            # restore
```

## License

MIT, see [LICENSE](LICENSE).
