from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image, ImageDraw, ImageOps


def make_assets(source: Path, output: Path) -> None:
    output.mkdir(parents=True, exist_ok=True)

    # Keep the committed mascot artwork as the source of truth. No redraw,
    # face edits, recoloring, or lossy re-encoding is performed.
    with Image.open(source) as original:
        image = original.convert("RGB")
    width, height = image.size

    # Create the round app badge using the same crop as the existing icon.
    side = min(height, int(width * 0.68))
    left = max(0, min(width - side, int(width * 0.07)))
    top = max(0, min(height - side, int(height * 0.02)))
    portrait = image.crop((left, top, left + side, top + side))

    badge_size = 512
    badge_content = portrait.resize((badge_size, badge_size), Image.Resampling.LANCZOS)
    mask_large = Image.new("L", (badge_size * 4, badge_size * 4), 0)
    ImageDraw.Draw(mask_large).ellipse(
        (8, 8, badge_size * 4 - 8, badge_size * 4 - 8), fill=255
    )
    mask = mask_large.resize((badge_size, badge_size), Image.Resampling.LANCZOS)
    badge = badge_content.convert("RGBA")
    badge.putalpha(mask)
    ImageDraw.Draw(badge).ellipse(
        (4, 4, badge_size - 5, badge_size - 5),
        outline=(238, 147, 192, 255),
        width=8,
    )

    icon_path = output / "YorHud.ico"
    badge.save(
        icon_path,
        format="ICO",
        sizes=[
            (16, 16), (24, 24), (32, 32), (48, 48),
            (64, 64), (128, 128), (256, 256),
        ],
    )

    # Inno Setup uses the portrait ratio 164:314. The 1x image is taken
    # directly from the source crop; the 2x image is generated once for DPI.
    wizard_source = image.crop((0, 0, int(width * 0.72), height))
    wizard_1x = ImageOps.fit(
        wizard_source,
        (267, 512),
        method=Image.Resampling.LANCZOS,
        centering=(0.47, 0.42),
    )
    wizard_2x = wizard_1x.resize((534, 1024), Image.Resampling.LANCZOS)
    wizard_1x.save(output / "YorHud-Wizard-1x.png", format="PNG", optimize=True)
    wizard_2x.save(output / "YorHud-Wizard-2x.png", format="PNG", optimize=True)

    # Transparent 512px badge for the completed page; the old 55px BMP
    # lost transparency and was too small for high-DPI scaling.
    badge.save(output / "YorHud-WizardSmall.png", format="PNG", optimize=True)

    expected = {
        "YorHud-Wizard-1x.png": (267, 512),
        "YorHud-Wizard-2x.png": (534, 1024),
        "YorHud-WizardSmall.png": (512, 512),
    }
    for name, size in expected.items():
        path = output / name
        with Image.open(path) as check:
            if check.size != size:
                raise RuntimeError(f"{name} has size {check.size}, expected {size}")
            if name == "YorHud-WizardSmall.png" and check.mode != "RGBA":
                raise RuntimeError("YorHud-WizardSmall.png must keep its alpha channel")
        print(f"Generated {name}: {path.stat().st_size} bytes")

    with Image.open(icon_path) as ico:
        sizes = sorted(ico.ico.sizes())
        if not sizes:
            raise RuntimeError("YorHud.ico contains no embedded sizes")
        for size in sizes:
            frame = ico.ico.getimage(size).convert("RGBA")
            if frame.getpixel((0, 0))[3] != 0:
                raise RuntimeError(f"YorHud.ico corner is not transparent at {size}")

    print(f"Generated YorHud.ico: {icon_path.stat().st_size} bytes")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    make_assets(args.input, args.output)
