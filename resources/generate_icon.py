from pathlib import Path

from PIL import Image, ImageDraw

size = 256
scale = size // 32
image = Image.new("RGBA", (size, size), (0, 0, 0, 0))
draw = ImageDraw.Draw(image)

draw.rectangle((0, 0, size - 1, size - 1), fill=(10, 18, 28, 255))
draw.ellipse(
    (3 * scale, 3 * scale, size - 4 * scale, size - 4 * scale),
    outline=(44, 188, 117, 255),
    width=3 * scale,
    fill=(16, 31, 46, 255),
)
draw.line(
    [(8 * scale, 23 * scale), (24 * scale, 23 * scale)],
    fill=(120, 210, 255, 255),
    width=2 * scale,
)
draw.line(
    [
        (8 * scale, 23 * scale),
        (12 * scale, 19 * scale),
        (17 * scale, 25 * scale),
        (21 * scale, 18 * scale),
        (24 * scale, 23 * scale),
    ],
    fill=(120, 210, 255, 255),
    width=2 * scale,
)
draw.polygon(
    [
        (10 * scale, 18 * scale),
        (15 * scale, 11 * scale),
        (21 * scale, 18 * scale),
        (18 * scale, 18 * scale),
        (18 * scale, 21 * scale),
        (13 * scale, 21 * scale),
    ],
    fill=(44, 188, 117, 255),
)

output_path = Path(__file__).resolve().with_name("app.ico")
image.save(
    output_path,
    format="ICO",
    sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)],
)
print(f"Created icon: {output_path}")
