#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.12"
# dependencies = [
#     "fritzconnection",
#     "qrcode[pil]",
# ]
# ///
"""Sets a new password for the guest Wi-Fi of a FRITZ!Box.

Usage:         FRITZ_USER=... FRITZ_PASSWORD=... ./guest_wifi.py
               (uv installs the dependencies automatically; chmod +x once)
Optional:      --password "MyPassword"       (random, 16 characters otherwise)
               --index 3                     (WLANConfiguration index, manual)
               --ssid "Guests"               (or FRITZ_GUEST_SSID; abort if --index
                                             points to another network)
               --address 192.168.178.1       (or set FRITZ_ADDRESS)
               --json-file guest-wifi.json   (output for the ESP, see below)
               --qr-file guest-wifi-qr.png
               --display-retries 10          (attempts if the display is not reachable)

The JSON file contains ssid, password and payload (the WIFI: string for the
QR code). If DISPLAY_URL and DISPLAY_TOKEN are set, the data is then sent to the
e-paper display (send_to_display.py, installed next to this script).
"""
import argparse
import json
import os
import secrets
import string
import sys

import qrcode
from fritzconnection import FritzConnection
from fritzconnection.lib.fritzwlan import FritzGuestWLAN


def wifi_payload(ssid, password):
    """Wi-Fi string in the standard format that phones recognize via QR code."""
    def esc(s):
        for c in ("\\", ";", ",", ":", '"'):
            s = s.replace(c, "\\" + c)
        return s

    return f"WIFI:T:WPA;S:{esc(ssid)};P:{esc(password)};;"


def wifi_qr(ssid, password, file):
    """Creates a Wi-Fi QR code (PNG) and prints it to the terminal."""
    data = wifi_payload(ssid, password)
    qr = qrcode.QRCode(border=2)
    qr.add_data(data)
    qr.make(fit=True)
    qr.make_image(fill_color="black", back_color="white").save(file)
    # umask only applies to new files, so fix the mode of an older one too
    os.chmod(file, 0o600)
    qr.print_ascii(invert=True)
    return file


def random_password(length=16):
    # Without characters that are easy to mix up when typing (0/O, 1/l/I)
    chars = [c for c in string.ascii_letters + string.digits if c not in "0O1lI"]
    return "".join(secrets.choice(chars) for _ in range(length))


def list_ssids(fc):
    """Shows the SSIDs of all WLANConfiguration services (for manual selection)."""
    for i in range(1, 5):
        try:
            info = fc.call_action(f"WLANConfiguration{i}", "GetInfo")
            print(f"  WLANConfiguration{i}: {info.get('NewSSID')}")
        except Exception:
            pass


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--address", default=os.environ.get("FRITZ_ADDRESS", "192.168.178.1"))
    p.add_argument("--password")
    p.add_argument("--index", type=int)
    p.add_argument("--ssid", default=os.environ.get("FRITZ_GUEST_SSID"))
    p.add_argument("--json-file", default="guest-wifi.json")
    p.add_argument("--qr-file", default="guest-wifi-qr.png")
    p.add_argument("--display-retries", type=int, default=1)
    a = p.parse_args()

    user = os.environ.get("FRITZ_USER")
    login_pw = os.environ.get("FRITZ_PASSWORD")
    if not user or not login_pw:
        raise SystemExit("Please set FRITZ_USER and FRITZ_PASSWORD as environment variables.")

    new = a.password or random_password()
    if not 8 <= len(new) <= 63:
        raise SystemExit("The password must be 8 to 63 characters long.")

    # Create the password files (JSON, PNG) readable by the owner only
    os.umask(0o077)

    fc = FritzConnection(address=a.address, user=user, password=login_pw)

    if a.index:
        service = f"WLANConfiguration{a.index}"
        ssid = fc.call_action(service, "GetInfo").get("NewSSID")
        if a.ssid and ssid != a.ssid:
            raise SystemExit(f"{service} is '{ssid}', not '{a.ssid}'. Password not changed.")
        fc.call_action(
            service, "SetSecurityKeys",
            NewWEPKey0="", NewWEPKey1="", NewWEPKey2="", NewWEPKey3="",
            NewPreSharedKey="", NewKeyPassphrase=new,
        )
    else:
        try:
            guest = FritzGuestWLAN(fc=fc)
            ssid = guest.ssid
            if a.ssid and ssid != a.ssid:
                raise SystemExit(f"Guest Wi-Fi is '{ssid}', not '{a.ssid}'. Password not changed.")
            guest.set_password(new)
        except SystemExit:
            raise
        except Exception as e:
            print(f"Guest Wi-Fi not found automatically ({e}). Available networks:")
            list_ssids(fc)
            raise SystemExit("Please run again with --index N.")

    print(f"Guest Wi-Fi '{ssid}' now has the password: {new}")
    print(f"QR code saved: {wifi_qr(ssid, new, a.qr_file)}")

    data = {"ssid": ssid, "password": new, "payload": wifi_payload(ssid, new)}
    with open(a.json_file, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=2)
    os.chmod(a.json_file, 0o600)
    print(f"JSON saved: {a.json_file}")

    if os.environ.get("DISPLAY_URL"):
        # Never fail because of the display: systemd would run the script again and
        # change the password once more. send_to_display.py can resend by hand.
        try:
            import send_to_display
            result = send_to_display.push(
                data, os.environ["DISPLAY_URL"], os.environ.get("DISPLAY_TOKEN", ""), a.display_retries
            )
            print(f"Display: {result}")
        except Exception as e:
            print(f"Display not updated: {e}", file=sys.stderr)


if __name__ == "__main__":
    main()
