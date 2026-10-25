#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.12"
# dependencies = [
#     "fritzconnection",
# ]
# ///
"""Lists the SSIDs of all WLANConfiguration services, to find the --index of the guest Wi-Fi.

Usage: FRITZ_USER=... FRITZ_PASSWORD=... ./list_wlans.py
Optional: FRITZ_ADDRESS (default: 192.168.178.1)
Read-only, nothing is changed.
"""
import os

from fritzconnection import FritzConnection

fc = FritzConnection(
    address=os.environ.get("FRITZ_ADDRESS", "192.168.178.1"),
    user=os.environ["FRITZ_USER"],
    password=os.environ["FRITZ_PASSWORD"],
)

for i in range(1, 5):
    try:
        info = fc.call_action(f"WLANConfiguration{i}", "GetInfo")
        print(f"WLANConfiguration{i}: {info.get('NewSSID')}")
    except Exception:
        pass
