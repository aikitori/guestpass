# Plan: MQTT and power saving for the e-paper display

## Goal

The display no longer runs an HTTP server that `guest_wifi.py` has to reach at the right moment.
Instead, the host publishes the guest Wi-Fi data as a **retained** MQTT message, and the display
picks it up whenever it is connected. An existing broker in the home network is used.

```
guest_wifi.py ──publish (retained, QoS 1)──▶ broker ◀──subscribe── display
                 guestpass/wifi
display ──publish (retained)──▶ guestpass/display/status   (no password)
```

## Topics

| Topic | Direction | Content |
|---|---|---|
| `guestpass/wifi` | host → display, retained | `{"ssid", "password", "payload", "updated"}` |
| `guestpass/display/status` | display → broker, retained | `{"online", "shown_ssid", "updated", "rssi", "ip", "uptime", "wifi_drops", "mqtt_drops", "firmware"}`, last will `{"online": false}` |

The broker should restrict access: the host may only write `guestpass/wifi`,
the display may read `guestpass/wifi` and write `guestpass/display/#`.

## Phase 1: always connected, Wi-Fi modem sleep (current)

The display runs on USB, so it stays connected and saves power where it is easy:

- `WiFi.setSleep(WIFI_PS_MAX_MODEM)`: the radio sleeps between beacons, MQTT stays connected
  (keep-alive 60 s).
- CPU clock 80 MHz, the lowest one Wi-Fi works with.
- The panel is only refreshed when the data changes, plus once a day against burn-in.

### Controls

Pins from Elecrow's documentation, all active low with internal pull-ups:

| Control | GPIO | Action |
|---|---|---|
| Rotary switch up | 6 | open the debug screen, previous page |
| Rotary switch down | 4 | open the debug screen, next page |
| Rotary switch press | 5 | rotate the image by 180° (kept in NVS) |
| EXIT | 1 | back to the guest Wi-Fi screen |
| HOME | 2 | back to the guest Wi-Fi screen |

The debug screen closes by itself after 2 minutes without input.
While it is open, new data is saved but drawn only after it closes.

### Debug pages

1. **Status:** firmware build, uptime, last reset reason, time (NTP), Wi-Fi SSID and RSSI, IP, MAC,
   broker and connection state, topic, time of the last message, guest SSID, `updated`,
   Wi-Fi and MQTT drops.
2. **Events:** the last 14 events, newest first (start, Wi-Fi/MQTT connected or lost with reason,
   new or unchanged data, invalid messages, rotation, daily refresh).

No home Wi-Fi password or MQTT credentials are shown.

## Phase 2: deep sleep on battery (later, only if needed)

Only worth it if the display has to run on a battery:

1. Measure the deep sleep current of the board first (CH340, regulator and power LED may dominate).
   One wake-up costs about 0.2 mAh (about 5 s at 120 mA); a sleep current of 50 µA gives about two
   years on 1000 mAh, 2 mA only about three weeks.
2. Wake up once a day (NTP-based, after the Wednesday timer run) and on any key (EXT1 wake-up,
   all keys are on RTC GPIOs), fetch the retained message, redraw only on change, publish the
   status and sleep again.
3. Keep BSSID, channel and a static IP in RTC memory for a fast connect, and keep the event log there.
4. Long press on HOME: maintenance mode with OTA for 10 minutes.

## Status

- [x] `publish_mqtt.py` replaces `send_to_display.py`, `guest_wifi.py` publishes after each change
      (`--mqtt-retries`, adds `updated`)
- [x] Service and `guest-wifi.env.example` use `MQTT_*` instead of `DISPLAY_URL`/`DISPLAY_TOKEN`
- [x] Firmware: MQTT instead of HTTP, modem sleep, status topic, debug screens, 180° rotation
- [x] `platformio.ini` (PubSubClient), `include/secrets.h.example` (MQTT settings)
- [ ] Build and flash (`pio run -t upload`), fill in the MQTT values in `include/secrets.h`
- [ ] Test: retained message after restart, unchanged data does not redraw, broker down keeps the
      image, keys and rotation, debug timeout
- [ ] Update the README section "E-paper display"
