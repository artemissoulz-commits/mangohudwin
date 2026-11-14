from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image, ImageDraw, ImageOps


def make_assets(source: Path, output: Path) -> None:
    output.mkdir(parents=True, exist_ok=True)
    image = Image.open(source).convert("RGB")
    width, height = image.size

    # Square portrait crop: silver hair, blindfold, face and ribbon are the app icon.
    side = min(height, int(width * 0.68))
    left = max(0, min(width - side, int(width * 0.07)))
    top = max(0, min(height - side, int(height * 0.02)))
    portrait = image.crop((left, top, left + side, top + side)).resize(
        (256, 256), Image.Resampling.LANCZOS
    )

    # Build a clean round icon while keeping transparency outside the badge.
    badge = Image.new("RGBA", (256, 256), (0, 0, 0, 0))
    mask = Image.new("L", (256, 256), 0)
    ImageDraw.Draw(mask).ellipse((5, 5, 250, 250), fill=255)
    badge.paste(portrait, (0, 0), mask)
    draw = ImageDraw.Draw(badge)
    draw.ellipse((4, 4, 251, 251), outline=(238, 147, 192, 255), width=5)
    badge.save(output / "YorHud.ico", format="ICO",
               sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])

    # Inno Setup's classic side artwork area is narrow and tall.
    wizard_source = image.crop((0, 0, int(width * 0.72), height))
    wizard = ImageOps.fit(
        wizard_source, (164, 314), method=Image.Resampling.LANCZOS,
        centering=(0.47, 0.42)
    )
    wizard.save(output / "YorHud-Wizard.bmp", format="BMP")

    small = ImageOps.fit(
        badge.convert("RGB"), (55, 55), method=Image.Resampling.LANCZOS
    )
    small.save(output / "YorHud-WizardSmall.bmp", format="BMP")

    for name in ("YorHud.ico", "YorHud-Wizard.bmp", "YorHud-WizardSmall.bmp"):
        file = output / name
        if not file.exists() or file.stat().st_size < 100:
            raise RuntimeError(f"Could not generate {name}")
        print(f"Generated {name}: {file.stat().st_size} bytes")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    make_assets(args.input, args.output)
