#!/usr/bin/env python3
import argparse
from pathlib import Path

from PIL import Image, ImageOps


def main():
    parser = argparse.ArgumentParser(description="Prepare a BF01 boot AI test image (requires Pillow).")
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    arguments = parser.parse_args()
    preview = arguments.output.with_suffix(".png")
    source = arguments.source.resolve()
    if source in (arguments.output.resolve(), preview.resolve()):
        parser.error("Input, planar output and PNG preview must use separate paths.")
    if arguments.output.suffix.lower() != ".rgbp":
        parser.error("The planar output must have the .rgbp extension.")
    with Image.open(arguments.source) as original:
        image = ImageOps.exif_transpose(original).convert("RGB")
        resized = ImageOps.contain(image, (640, 640), Image.Resampling.BILINEAR)
        prepared = Image.new("RGB", (640, 640), (114, 114, 114))
        prepared.paste(resized, ((640 - resized.width) // 2, (640 - resized.height) // 2))
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_bytes(b"".join(channel.tobytes() for channel in prepared.split()))
    prepared.save(preview)
    print(f"{arguments.output}: 640x640 RGB planar uint8, 1228800 bytes; preview: {preview}")


if __name__ == "__main__":
    main()
