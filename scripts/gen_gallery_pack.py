#!/usr/bin/env python3
"""Galeri görsel paketi üretici (narRtos-wear-c6 Session 16'dan port).

Kaynak JPEG/WEBP → cihazın doğrudan flash'tan çizebileceği RGB565 paketi.
Görseller RAM'e ALINMAZ: 'gallery' bölümüne yazılır, cihaz esp_partition_mmap
ile gösterir (bkz. src/services/gallery_pack.cpp).

  küçük kapak (ızgara): kart ölçüsü, ORTADAN KIRPILMIŞ (kart tam dolar)
  tam ekran           : ekran ölçüsü, TAMAMI SIĞDIRILMIŞ (kırpma yok, kenar siyah)

Renk düzeni: lv_conf.h'de LV_COLOR_16_SWAP=1 — bu bayrak ekrana YAZARKEN
(flush) bayt çevirir; görsel VERİSİ native LITTLE-ENDIAN beklenir. Ters sıra
cihazda yeşil/mor kırçıllı görüntü verir (C6 Session 16 dersi).

Kullanım:
    ~/.platformio/penv/bin/python scripts/gen_gallery_pack.py --board lcd128
    ~/.platformio/penv/bin/python scripts/gen_gallery_pack.py --board amoled \\
        --src ~/Downloads --files a.jpg b.jpg

Sonra cihaza:
    ~/.platformio/penv/bin/python -m esptool --chip esp32s3 \\
        --port <PORT> write_flash 0x800000 build_assets/gallery_pack.bin
"""
import argparse
import os
import struct
import sys

from PIL import Image

MAGIC = b'NRGP'
VERSION = 1
PART_OFFSET = 0x800000
PART_SIZE = 0x300000          # partitions_custom.csv: gallery bölümü

# Kart (kapak) ölçüleri gallery_app.cpp'deki GALL_CARD_W/H ile, tam ekran
# ölçüleri panel çözünürlüğüyle AYNI olmalı.
BOARDS = {
    'lcd128': {'thumb': (100, 122), 'full': (240, 240)},   # 240x240 yuvarlak
    '18inch': {'thumb': (164, 200), 'full': (368, 448)},
    'amoled': {'thumb': (190, 233), 'full': (410, 502)},
}

DEFAULT_FILES = ['icerikler_1.jpeg', 'icerikler_2.jpeg', 'icerikler_3.jpeg',
                 'icerikler_4.jpg',  'icerikler_5.jpg',  'icerikler_7.webp']


def to_rgb565_bytes(im):
    """PIL RGB → RGB565, LITTLE-ENDIAN (düşük bayt önce)."""
    px = im.tobytes()
    out = bytearray(im.width * im.height * 2)
    for i in range(im.width * im.height):
        r, g, b = px[i * 3], px[i * 3 + 1], px[i * 3 + 2]
        v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        out[i * 2] = v & 0xFF
        out[i * 2 + 1] = (v >> 8) & 0xFF
    return bytes(out)


def cover(im, w, h):
    """Kırp-doldur: oranı koru, taşan kısmı ORTADAN kes → boşluk kalmaz."""
    src_ar, dst_ar = im.width / im.height, w / h
    if src_ar > dst_ar:                      # kaynak daha geniş → yanlardan kes
        nw = int(round(im.height * dst_ar))
        box = ((im.width - nw) // 2, 0, (im.width - nw) // 2 + nw, im.height)
    else:                                    # kaynak daha uzun → üst/alttan kes
        nh = int(round(im.width / dst_ar))
        box = (0, (im.height - nh) // 2, im.width, (im.height - nh) // 2 + nh)
    return im.crop(box).resize((w, h), Image.LANCZOS)


def contain(im, w, h):
    """Sığdır: tamamı görünsün, oran korunsun, kalan alan siyah."""
    sc = min(w / im.width, h / im.height)
    nw, nh = max(1, int(round(im.width * sc))), max(1, int(round(im.height * sc)))
    canvas = Image.new('RGB', (w, h), (0, 0, 0))
    canvas.paste(im.resize((nw, nh), Image.LANCZOS), ((w - nw) // 2, (h - nh) // 2))
    return canvas


def main():
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser()
    ap.add_argument('--board', default='lcd128', choices=sorted(BOARDS))
    ap.add_argument('--src', default=os.path.expanduser('~/Downloads'))
    ap.add_argument('--files', nargs='*', default=DEFAULT_FILES)
    ap.add_argument('--out', default=os.path.join(here, 'build_assets', 'gallery_pack.bin'))
    args = ap.parse_args()

    dims = BOARDS[args.board]
    tw, th = dims['thumb']
    fw, fh = dims['full']

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    thumbs, fulls = [], []
    for name in args.files:
        path = os.path.join(os.path.expanduser(args.src), name)
        if not os.path.exists(path):
            sys.exit('YOK: ' + path)
        im = Image.open(path).convert('RGB')
        thumbs.append(to_rgb565_bytes(cover(im, tw, th)))
        fulls.append(to_rgb565_bytes(contain(im, fw, fh)))
        print('%-22s %sx%s → kapak+tam' % (name, im.width, im.height))

    n = len(args.files)
    head = struct.pack('<4sHHHHHHI', MAGIC, VERSION, n, tw, th, fw, fh, 0)
    off = len(head) + n * 16
    entries, blob = [], bytearray()
    for t in thumbs:                         # önce kapaklar (ızgara tek okumada)
        entries.append([off, len(t)])
        blob += t
        off += len(t)
    for i, f in enumerate(fulls):
        entries[i] += [off, len(f)]
        blob += f
        off += len(f)
    index = b''.join(struct.pack('<IIII', *e) for e in entries)

    pack = head + index + bytes(blob)
    if len(pack) > PART_SIZE:
        sys.exit('paket bölüme sığmıyor: %.2f MB > %.2f MB'
                 % (len(pack) / 1048576, PART_SIZE / 1048576))
    with open(args.out, 'wb') as fh_out:
        fh_out.write(pack)
    with open(args.out, 'rb') as fh_in:      # doğrulama
        assert fh_in.read(4) == MAGIC

    print('\n%s\n  %d görsel, %.2f MB (gallery bölümü %.2f MB)'
          % (args.out, n, len(pack) / 1048576, PART_SIZE / 1048576))
    print('  board=%s  kapak %dx%d, tam %dx%d' % (args.board, tw, th, fw, fh))
    print('  yükle: esptool write_flash 0x%06X %s' % (PART_OFFSET, args.out))


main()
