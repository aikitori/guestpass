#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.12"
# dependencies = ["paho-mqtt>=2"]
# ///
"""Publishes the current guest Wi-Fi data to MQTT, where the e-paper display (firmware/) picks it up.

Usage:     MQTT_HOST=... MQTT_USER=... MQTT_PASSWORD=... ./publish_mqtt.py [JSON_FILE]
JSON_FILE is the output of guest_wifi.py (default: ~/.local/state/guest-wifi/guest-wifi.json).
Optional:  MQTT_PORT     (default: 1883)
           MQTT_TOPIC    (default: guestpass/wifi)
           --retries N   attempts if the broker is not reachable (default: 1)
           --wait S      seconds between attempts (default: 60)

The message is retained, so the display gets it even if it connects later.
Does nothing if MQTT_HOST is not set. The FRITZ!Box is not touched, so it is
safe to run again by hand. guest_wifi.py calls push() after each password change.
"""
import argparse
import json
import os
import sys
import time
from pathlib import Path

import paho.mqtt.publish as publish

DEFAULT_TOPIC = "guestpass/wifi"


def push(data, host, retries=1, wait=60):
    """Publishes ssid, password, payload and updated (retained). Raises RuntimeError if it fails."""
    message = {k: data[k] for k in ("ssid", "password", "payload", "updated") if k in data}
    user = os.environ.get("MQTT_USER")
    auth = {"username": user, "password": os.environ.get("MQTT_PASSWORD")} if user else None
    topic = os.environ.get("MQTT_TOPIC", DEFAULT_TOPIC)
    for attempt in range(1, retries + 1):
        try:
            publish.single(
                topic, json.dumps(message, ensure_ascii=False), qos=1, retain=True,
                hostname=host, port=int(os.environ.get("MQTT_PORT", 1883)), auth=auth,
            )
            return f"published to {topic}"
        except Exception as e:
            print(f"Broker not reachable ({e}), attempt {attempt}/{retries}.", file=sys.stderr)
            if attempt < retries:
                time.sleep(wait)
    raise RuntimeError(f"not published after {retries} attempts")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("json_file", nargs="?", default="~/.local/state/guest-wifi/guest-wifi.json")
    p.add_argument("--retries", type=int, default=1)
    p.add_argument("--wait", type=int, default=60)
    a = p.parse_args()

    host = os.environ.get("MQTT_HOST")
    if not host:
        print("MQTT_HOST is not set, nothing to do.")
        return

    file = Path(a.json_file).expanduser()
    try:
        data = json.loads(file.read_text(encoding="utf-8"))
    except FileNotFoundError:
        raise SystemExit(f"{file} not found. Run guest_wifi.py first.")
    try:
        print(f"MQTT: {push(data, host, a.retries, a.wait)}")
    except RuntimeError as e:
        raise SystemExit(f"MQTT: {e}")


if __name__ == "__main__":
    main()
