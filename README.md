# Guest Wi-Fi password for the FRITZ!Box

Sets a new random password for the guest Wi-Fi of a FRITZ!Box via TR-064.
It then creates a Wi-Fi QR code (PNG and ASCII in the terminal) and a JSON file with the
SSID, the password and the QR payload, for example for a display on an ESP.

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

## Weekly via systemd (user unit)

```sh
install -Dm 755 guest_wifi.py ~/.local/bin/guest_wifi.py
install -Dm 644 -t ~/.config/systemd/user/ guest-wifi.service guest-wifi.timer
install -Dm 600 guest-wifi.env.example ~/.config/guest-wifi/env   # fill in the values
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

After changing the script or the units, install them again and reload systemd:

```sh
install -Dm 755 guest_wifi.py ~/.local/bin/guest_wifi.py
install -Dm 644 -t ~/.config/systemd/user/ guest-wifi.service guest-wifi.timer
systemctl --user daemon-reload
```

### Uninstall

```sh
systemctl --user disable --now guest-wifi.timer
rm ~/.local/bin/guest_wifi.py ~/.config/systemd/user/guest-wifi.{service,timer}
rm -r ~/.config/guest-wifi ~/.local/state/guest-wifi
systemctl --user daemon-reload
loginctl disable-linger "$USER"   # only if no other user services need it
```

## To do

- [ ] Send the new password / QR code to an ESP with an e-ink display
  (`guest-wifi.json` already contains `ssid`, `password` and the QR `payload`)

## License

MIT, see [LICENSE](LICENSE).
