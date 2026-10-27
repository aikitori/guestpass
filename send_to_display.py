#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.12"
# dependencies = []
# ///
"""Sends the current guest Wi-Fi data to the e-paper display (firmware/ in this repo).

Usage:     DISPLAY_URL=http://guest-display.fritz.box DISPLAY_TOKEN=... ./send_to_display.py [JSON_FILE]
JSON_FILE is the output of guest_wifi.py (default: ~/.local/state/guest-wifi/guest-wifi.json).
Optional:  --retries N   attempts if the display is not reachable (default: 1)
           --wait S      seconds between attempts (default: 60)

Does nothing if DISPLAY_URL is not set. The FRITZ!Box is not touched, so it is
safe to run again by hand. guest_wifi.py calls push() after each password change.
"""
import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path


def send(url, token, data):
    req = urllib.request.Request(
        url.rstrip("/") + "/guest-wifi",
        data=json.dumps(data).encode(),
        headers={"Content-Type": "application/json", "Authorization": f"Bearer {token}"},
        method="POST",
    )
    # The display answers after the refresh, which takes a few seconds
    with urllib.request.urlopen(req, timeout=60) as resp:
        return resp.read().decode().strip()


def push(data, url, token, retries=1, wait=60):
    """Sends ssid, password and payload to the display. Raises RuntimeError if it fails."""
    data = {k: data[k] for k in ("ssid", "password", "payload")}
    for attempt in range(1, retries + 1):
        try:
            return send(url, token, data)
        except urllib.error.HTTPError as e:
            # The display rejected the data, another attempt will not help
            raise RuntimeError(f"answered {e.code}: {e.read().decode().strip()}")
        except OSError as e:
            print(f"Display not reachable ({e}), attempt {attempt}/{retries}.", file=sys.stderr)
            if attempt < retries:
                time.sleep(wait)
    raise RuntimeError(f"not reachable after {retries} attempts")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("json_file", nargs="?", default="~/.local/state/guest-wifi/guest-wifi.json")
    p.add_argument("--retries", type=int, default=1)
    p.add_argument("--wait", type=int, default=60)
    a = p.parse_args()

    url = os.environ.get("DISPLAY_URL")
    token = os.environ.get("DISPLAY_TOKEN")
    if not url:
        print("DISPLAY_URL is not set, nothing to do.")
        return
    if not token:
        raise SystemExit("Please set DISPLAY_TOKEN (the same value as in firmware/include/secrets.h).")

    file = Path(a.json_file).expanduser()
    try:
        data = json.loads(file.read_text(encoding="utf-8"))
    except FileNotFoundError:
        raise SystemExit(f"{file} not found. Run guest_wifi.py first.")
    try:
        print(f"Display: {push(data, url, token, a.retries, a.wait)}")
    except RuntimeError as e:
        raise SystemExit(f"Display not updated: {e}")


if __name__ == "__main__":
    main()
