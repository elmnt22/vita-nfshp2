"""Validate and embed the complete Switch button atlas."""

from pathlib import Path
import sys

from PIL import Image


source, destination = map(Path, sys.argv[1:])
with Image.open(source) as image:
    if image.format != "PNG" or image.size != (128, 256):
        raise SystemExit("buttons.png must be a 128x256 PNG")
    image.load()

data = source.read_bytes()
destination.write_text(
    "".join(",".join(f"0x{byte:02x}" for byte in data[i:i + 16]) + ",\n"
            for i in range(0, len(data), 16)),
    encoding="ascii",
)
