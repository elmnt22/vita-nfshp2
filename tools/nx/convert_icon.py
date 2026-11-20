"""Convert the project's PNG to a 256x256 JPEG for the Homebrew Menu."""

import sys

from PIL import Image, ImageOps


source, destination = sys.argv[1:]
with Image.open(source) as image:
    artwork = ImageOps.contain(image.convert("RGBA"), (256, 256), Image.Resampling.LANCZOS)
    icon = Image.new("RGB", (256, 256))
    position = ((256 - artwork.width) // 2, (256 - artwork.height) // 2)
    icon.paste(artwork, position, artwork)
    icon.save(destination, format="JPEG", quality=95, optimize=True)
