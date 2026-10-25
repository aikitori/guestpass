#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.12"
# dependencies = [
#     "qrcode",
# ]
# ///
"""Shows the current guest Wi-Fi QR code and password in the terminal.

Usage: ./show_qr.py [JSON_FILE]
JSON_FILE is the output of guest_wifi.py (default: ~/.local/state/guest-wifi/guest-wifi.json).
Read-only, nothing is changed.
"""
import json
import sys
from pathlib import Path

import qrcode

file = Path(sys.argv[1] if len(sys.argv) > 1 else "~/.local/state/guest-wifi/guest-wifi.json").expanduser()
try:
    data = json.loads(file.read_text(encoding="utf-8"))
except FileNotFoundError:
    raise SystemExit(f"{file} not found. Run guest_wifi.py first.")

qr = qrcode.QRCode(border=2)
qr.add_data(data["payload"])
qr.make(fit=True)
qr.print_ascii(invert=True)

print(f"SSID:     {data['ssid']}")
print(f"Password: {data['password']}")
