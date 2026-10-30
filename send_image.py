#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.12"
# dependencies = ["paho-mqtt>=2", "pillow>=10"]
# ///
"""Sends an image to the e-paper display (firmware/) via MQTT.

Usage:     MQTT_HOST=... MQTT_USER=... MQTT_PASSWORD=... ./send_image.py IMAGE
           ./send_image.py --clear          back to the guest Wi-Fi screen, image removed
           ./send_image.py IMAGE --preview out.png   only write the 1-bit result, send nothing
Optional:  MQTT_PORT          (default: 1883)
           MQTT_IMAGE_TOPIC   (default: guestpass/image)
           --cover            fill the whole display and crop, instead of fitting with white borders
           --no-dither        hard black/white threshold, better for text and line art

The image is scaled to 400x300, converted to black and white (Floyd-Steinberg dithering)
and published as a retained message: 15000 bytes, rows top to bottom, MSB first, 1 = white.
HOME on the display switches between the image and the guest Wi-Fi. In image view a new image
appears right away, in guest Wi-Fi view it is only saved.
"""
import argparse
import os
import sys

import paho.mqtt.publish as publish
from PIL import Image, ImageOps

SIZE = (400, 300)
DEFAULT_TOPIC = "guestpass/image"


def to_bitmap(path, cover=False, dither=True):
    """Returns the image as a 400x300 1-bit PIL image."""
    img = ImageOps.exif_transpose(Image.open(path)).convert("RGBA")
    white = Image.new("RGBA", img.size, "white")
    img = Image.alpha_composite(white, img).convert("L")
    if cover:
        img = ImageOps.fit(img, SIZE)
    else:
        img = ImageOps.pad(img, SIZE, color=255)
    return img.convert("1", dither=Image.Dither.FLOYDSTEINBERG if dither else Image.Dither.NONE)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("image", nargs="?")
    p.add_argument("--clear", action="store_true")
    p.add_argument("--cover", action="store_true")
    p.add_argument("--no-dither", action="store_true")
    p.add_argument("--preview", metavar="PNG")
    a = p.parse_args()
    if bool(a.image) == a.clear:
        p.error("give an image or --clear")

    if a.clear:
        payload = b"clear"
    else:
        bitmap = to_bitmap(a.image, a.cover, not a.no_dither)
        if a.preview:
            bitmap.save(a.preview)
            print(f"Preview written to {a.preview}")
            return
        payload = bitmap.tobytes()  # mode "1" packs 8 pixels per byte, MSB first, 1 = white

    host = os.environ.get("MQTT_HOST")
    if not host:
        raise SystemExit("MQTT_HOST is not set.")
    user = os.environ.get("MQTT_USER")
    auth = {"username": user, "password": os.environ.get("MQTT_PASSWORD")} if user else None
    topic = os.environ.get("MQTT_IMAGE_TOPIC", DEFAULT_TOPIC)
    try:
        publish.single(topic, payload, qos=1, retain=True, hostname=host,
                       port=int(os.environ.get("MQTT_PORT", 1883)), auth=auth)
    except Exception as e:
        print(f"Broker not reachable ({e}).", file=sys.stderr)
        raise SystemExit(1)
    print(f"Published {len(payload)} bytes to {topic}")


if __name__ == "__main__":
    main()
