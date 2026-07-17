"""Generate application taskbar/tray icons from source PNGs."""

from __future__ import annotations

from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
RESOURCES = ROOT / "resources"
ICO_SIZES = [(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)]
# Tray icons look smaller when the artwork leaves extra padding; fill more of the canvas.
CONTENT_FILL = 0.99


def visible_mask(img: Image.Image) -> np.ndarray:
    arr = np.array(img.convert("RGBA"))
    r, g, b, a = arr[..., 0], arr[..., 1], arr[..., 2], arr[..., 3]
    return (a > 16) & ~((r < 20) & (g < 20) & (b < 20))


def crop_visible_square(img: Image.Image) -> Image.Image:
    mask = visible_mask(img)
    ys, xs = np.where(mask)
    if len(xs) == 0:
        return img

    left, right = int(xs.min()), int(xs.max())
    top, bottom = int(ys.min()), int(ys.max())
    width = right - left + 1
    height = bottom - top + 1
    side = max(width, height)
    cx = (left + right) // 2
    cy = (top + bottom) // 2
    half = side // 2
    crop_box = (
        max(0, cx - half),
        max(0, cy - half),
        min(img.width, cx - half + side),
        min(img.height, cy - half + side),
    )
    cropped = img.crop(crop_box)

    side = max(cropped.width, cropped.height)
    square = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    offset = ((side - cropped.width) // 2, (side - cropped.height) // 2)
    square.paste(cropped, offset, cropped)
    return square


def normalize_icon(png_path: Path, canvas_size: int = 256) -> Image.Image:
    source = Image.open(png_path).convert("RGBA")
    content = crop_visible_square(source)
    content_size = max(1, int(canvas_size * CONTENT_FILL))
    content = content.resize((content_size, content_size), Image.LANCZOS)

    canvas = Image.new("RGBA", (canvas_size, canvas_size), (0, 0, 0, 0))
    offset = (canvas_size - content_size) // 2
    canvas.paste(content, (offset, offset), content)
    return canvas


def png_to_ico(png_path: Path, ico_path: Path) -> None:
    icon = normalize_icon(png_path)
    icon.save(ico_path, format="ICO", sizes=ICO_SIZES)
    print(f"Created {ico_path.name} from {png_path.name}")


def main() -> None:
    png_to_ico(RESOURCES / "icon_green.png", RESOURCES / "app_icon.ico")
    # 运行中托盘：红色；空闲：绿色 app_icon。
    png_to_ico(RESOURCES / "icon_red.png", RESOURCES / "tray_running.ico")
    png_to_ico(RESOURCES / "icon_red.png", RESOURCES / "breakout_pause.ico")


if __name__ == "__main__":
    main()
