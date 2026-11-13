#!/usr/bin/env python3
"""HQ Mesaj ekranının dairesel avatarını (Genel Müdür fotoğrafı) üretir.

Kaynak fotoğraftan kare bir yüz kadrajı kesip 88 px (LCD128) ve 196 px
(AMOLED 2.06" / 1.8") boyutlarında ARGB8888 LVGL asset'i yazar. Daire dışı
alpha = 0; kenar 4x süperörneklemeyle yumuşatılır.

Kullanım:
    python scripts/gen_hq_avatar.py ~/Downloads/foto.jpeg \\
        --cx 3490 --cy 1700 --side 900

--cx/--cy/--side verilmezse fotoğrafın merkezinden kısa kenar boyunca
kare kesilir (genel kadrajlı fotoğraflarda yüz çok küçük kalır — bu yüzden
kadraj değerlerini gözle belirleyip vermek gerekir).

Pillow gerekir:  ~/.platformio/penv/bin/python scripts/gen_hq_avatar.py ...
"""
import argparse
import os
import sys

from PIL import Image, ImageDraw, ImageOps

SIZES = {88: "BOARD_VARIANT_LCD128", 196: "!BOARD_VARIANT_LCD128"}
SS = 4  # daire maskesi süperörnekleme çarpanı


def circular_avatar(src: Image.Image, size: int) -> Image.Image:
    """Kare RGB görüntüyü verilen boyuta indirip dairesel alpha maskesi uygular."""
    img = src.resize((size, size), Image.LANCZOS).convert("RGBA")
    mask = Image.new("L", (size * SS, size * SS), 0)
    ImageDraw.Draw(mask).ellipse((0, 0, size * SS - 1, size * SS - 1), fill=255)
    img.putalpha(mask.resize((size, size), Image.LANCZOS))
    return img


def emit_c(img: Image.Image, size: int, guard: str, src_name: str, out_path: str):
    """LVGL ARGB8888 C dizisi yaz (bellek düzeni: B, G, R, A)."""
    px = img.load()
    rows = []
    for y in range(size):
        vals = []
        for x in range(size):
            r, g, b, a = px[x, y]
            vals += [b, g, r, a]
        rows.append(",".join("0x%02x" % v for v in vals))

    ifdef = "#ifndef BOARD_VARIANT_LCD128" if guard.startswith("!") else "#ifdef " + guard
    endif = "#endif /* %s */" % guard
    with open(out_path, "w") as f:
        f.write(
            "/* HQ Mesaj ekranı avatarı: Genel Müdür fotoğrafı — %dx%d dairesel.\n"
            " * Kaynak: %s. Üretim: scripts/gen_hq_avatar.py (Pillow) —\n"
            " * yüz odaklı kare kadraj + %dx süperörneklemeli daire maskesi,\n"
            " * daire dışı alpha=0. Yeniden üretim için script'teki kadraj\n"
            " * parametrelerini (--cx/--cy/--side) kullanın. */\n"
            "#include \"lvgl.h\"\n\n"
            "/* Board başına tek boyut derlenir (flash tasarrufu). */\n"
            "%s\n\n"
            "static const uint8_t hq_avatar_%d_map[] LV_ATTRIBUTE_LARGE_CONST = {\n"
            % (size, size, src_name, SS, ifdef, size)
        )
        f.write(",\n".join("  " + r for r in rows))
        f.write(
            "\n};\n\n"
            "const lv_image_dsc_t hq_avatar_%d = {\n"
            "  .header = {\n"
            "    .cf = LV_COLOR_FORMAT_ARGB8888,\n"
            "    .w = %d,\n"
            "    .h = %d,\n"
            "    .stride = %d,\n"
            "  },\n"
            "  .data_size = %d,\n"
            "  .data = hq_avatar_%d_map,\n"
            "};\n\n"
            "%s\n" % (size, size, size, size * 4, size * size * 4, size, endif)
        )


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("photo")
    ap.add_argument("--cx", type=int, help="kadraj merkezi X (kaynak piksel)")
    ap.add_argument("--cy", type=int, help="kadraj merkezi Y")
    ap.add_argument("--side", type=int, help="kare kadraj kenarı")
    ap.add_argument("--outdir", default="src/assets")
    ap.add_argument("--preview", help="kesilen kareyi PNG olarak da yaz")
    args = ap.parse_args()

    path = os.path.expanduser(args.photo)
    im = ImageOps.exif_transpose(Image.open(path)).convert("RGB")

    side = args.side or min(im.size)
    cx = args.cx if args.cx is not None else im.width // 2
    cy = args.cy if args.cy is not None else im.height // 2
    box = (cx - side // 2, cy - side // 2, cx + side // 2, cy + side // 2)
    if box[0] < 0 or box[1] < 0 or box[2] > im.width or box[3] > im.height:
        sys.exit("kadraj görüntü dışına taşıyor: %s (görüntü %s)" % (box, im.size))
    square = im.crop(box)
    if args.preview:
        square.save(args.preview)

    for size, guard in SIZES.items():
        out = os.path.join(args.outdir, "hq_avatar_%d.c" % size)
        emit_c(circular_avatar(square, size), size, guard,
               os.path.basename(path), out)
        print("yazıldı: %s (%dx%d, %d bayt veri)" % (out, size, size, size * size * 4))


if __name__ == "__main__":
    main()
