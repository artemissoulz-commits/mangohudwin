from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image, ImageDraw, ImageOps


def make_assets(source: Path, output: Path) -> None:
    output.mkdir(parents=True, exist_ok=True)
    # Keep the committed mascot artwork as the source of truth. This script only
    # crops/resizes it for the icon and installer slots; it does not redraw it.
    with Image.open(source) as original:
        image = original.convert("RGB")
    width, height = image.size

    # Square app badge: same portrait crop as before, with a proper transparent
    # alpha mask retained in both the ICO and the installer PNG.
    side = min(height, int(width * 0.68))
    left = max(0, min(width - side, int(width * 0.07)))
    top = max(0, min(height - side, int(height * 0.02)))
    portrait = image.crop((left, top, left + side, top + side))

    badge_size = 512
    badge_content = portrait.resize(
        (badge_size, badge_size), Image.Resampling.LANCZOS
    )
    # Supersample the circular edge so every exported size has a clean,
    # transparent exterior instead of a dark square matte.
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

    # Inno Setup's side panel has a 164:314 aspect ratio. Keep the original
    # composition and export a 3x PNG so the wizard does not enlarge a tiny BMP.
    wizard_source = image.crop((0, 0, int(width * 0.72), height))
    wizard = ImageOps.fit(
        wizard_source,
        (492, 942),
        method=Image.Resampling.LANCZOS,
        centering=(0.47, 0.42),
    )
    wizard.convert("RGBA").save(
        output / "YorHud-Wizard.png", format="PNG", optimize=True
    )

    # The upper-right wizard badge is DPI-scaled by Inno Setup. Export a
    # 512px transparent PNG instead of the old 55px flattened BMP.
    badge.save(output / "YorHud-WizardSmall.png", format="PNG", optimize=True)

    # Validate dimensions and transparency, catching regressions before build.
    for name, minimum in (
        ("YorHud-Wizard.png", (492, 942)),
        ("YorHud-WizardSmall.png", (512, 512)),
    ):
        path = output / name
        with Image.open(path) as check:
            if check.size[0] < minimum[0] or check.size[1] < minimum[1]:
                raise RuntimeError(f"{name} is too small: {check.size}")
            if check.mode != "RGBA":
                raise RuntimeError(f"{name} must retain RGBA mode")
        print(f"Generated {name}: {path.stat().st_size} bytes")

    # Confirm all embedded ICO sizes preserve transparent outer corners.
    with Image.open(icon_path) as ico:
        sizes = sorted(ico.ico.sizes())
        if not sizes:
            raise RuntimeError("YorHud.ico contains no embedded sizes")
        for size in sizes:
            frame = ico.ico.getimage(size).convert("RGBA")
            if frame.getpixel((0, 0))[3] != 0:
                raise RuntimeError(f"YorHud.ico is not transparent at {size}")

    print(f"Generated YorHud.ico: {icon_path.stat().st_size} bytes")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    make_assets(args.input, args.output)
