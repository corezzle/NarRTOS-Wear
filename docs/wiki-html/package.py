# -*- coding: utf-8 -*-
"""Wiki'yi tek dosyalık, kendi kendine yeten HTML'e paketler.

index.html tek başına gönderilemez: CSS, JS ve 33 görsel göreli yolla bağlı,
ayrıca iki alt sayfaya link veriyor. Bu betik üçünü tek belgede birleştirir,
CSS/JS'i gömer ve tüm görselleri base64 data URI'ye çevirir. Çıkan dosya
çift tıklayınca her yerde açılır — internet, sunucu, klasör gerekmez.

Kullanım:  python3 docs/wiki-html/package.py
Çıktı:     docs/narrtos-wiki.html  (+ docs/narrtos-wiki.zip)
"""
import base64
import mimetypes
import re
import shutil
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
OUT_HTML = HERE.parent / "narrtos-wiki.html"
OUT_ZIP = HERE.parent / "narrtos-wiki.zip"

# (dosya, birleşik belgedeki bölüm id'si, TOC'ta görünecek başlık)
PAGES = [
    ("index.html", None, None),
    ("telemetry-and-positioning.html", "sayfa-telemetri", "Telemetri ve Konumlandırma"),
    ("server-setup.html", "sayfa-sunucu", "Sunucu Kurulumu"),
    ("vscode-setup.html", "sayfa-vscode", "VS Code Kurulumu"),
    ("vs-code.html", "sayfa-vs-code", "VS Code Kurulumu (genel)"),
    ("imager-install.html", "sayfa-imager", "Corezzle Imager ile Yükleme"),
]

LINK_MAP = {
    "telemetry-and-positioning.html": "#sayfa-telemetri",
    "server-setup.html": "#sayfa-sunucu",
    "vscode-setup.html": "#sayfa-vscode",
    "vs-code.html": "#sayfa-vs-code",
    "imager-install.html": "#sayfa-imager",
    "index.html": "#ust",
}


def data_uri(rel_path):
    p = (HERE / rel_path).resolve()
    if not p.exists():
        print(f"  !! bulunamadı: {rel_path}")
        return None
    mime = mimetypes.guess_type(p.name)[0] or "application/octet-stream"
    b64 = base64.b64encode(p.read_bytes()).decode("ascii")
    return f"data:{mime};base64,{b64}"


def inline_images(html, cache):
    """src="assets/..." → data URI. Aynı dosya birden çok yerde geçse de
    tek kez kodlanır (cache), böylece çıktı gereksiz şişmez."""
    def repl(m):
        quote, path = m.group(1), m.group(2)
        if path not in cache:
            cache[path] = data_uri(path)
        uri = cache[path]
        return f'src={quote}{uri or path}{quote}'
    return re.sub(r'src=(["\'])((?:assets|\.\./assets)/[^"\']+)\1', repl, html)


def rewrite_links(html):
    """Sayfalar arası bağlantıları belge içi çapalara çevirir."""
    def repl(m):
        quote, href = m.group(1), m.group(2)
        page, _, frag = href.partition("#")
        if page in LINK_MAP:
            return f'href={quote}{("#" + frag) if frag else LINK_MAP[page]}{quote}'
        return m.group(0)
    return re.sub(r'href=(["\'])([^"\'#][^"\']*\.html(?:#[^"\']*)?)\1', repl, html)


def main_content(html):
    m = re.search(r"<main>(.*?)</main>", html, re.S)
    return m.group(1) if m else ""


def main():
    css = (HERE / "assets/css/wiki.css").read_text(encoding="utf-8")
    js = (HERE / "assets/js/wiki.js").read_text(encoding="utf-8")
    cache = {}

    blocks = []
    for fname, sec_id, title in PAGES:
        raw = (HERE / fname).read_text(encoding="utf-8")
        body = main_content(raw)
        body = rewrite_links(body)
        body = inline_images(body, cache)

        if sec_id:
            # Alt sayfanın hero'sunu sade bir h2'ye indir — TOC'ta bölüm olarak çıksın
            body = re.sub(r'<div class="hero">.*?</div>\s*(?=<)', "", body, count=1, flags=re.S)
            body = re.sub(r"<h1>.*?</h1>", "", body, count=1, flags=re.S)
            body = re.sub(r'<p class="lede">.*?</p>', "", body, count=1, flags=re.S)
            blocks.append(f'<hr /><h2 id="{sec_id}">{title}</h2>\n{body}')
        else:
            blocks.append(body)
        print(f"  ✓ {fname}")

    doc = f"""<!doctype html>
<html lang="tr">
<head>
<meta charset="UTF-8" />
<meta name="viewport" content="width=device-width, initial-scale=1" />
<title>NarRTOS Wiki</title>
<style>
{css}
</style>
</head>
<body>
<div class="topbar" id="ust">
  <div class="topbar-in">
    <div class="brand">NarRTOS <span>Wiki</span></div>
    <div class="ver mono">v1.4.81 · ESP32-S3 · LVGL 9</div>
  </div>
</div>
<div class="wrap">
<div class="layout">
<main>
{"".join(blocks)}
</main>
<aside class="toc">
  <div class="toc-h">Bu sayfada</div>
  <nav class="toc-list"></nav>
</aside>
</div>
</div>
<script>
{js}
</script>
</body>
</html>
"""
    OUT_HTML.write_text(doc, encoding="utf-8")
    mb = OUT_HTML.stat().st_size / 1048576
    print(f"\n✓ tek dosya: {OUT_HTML.name}  ({mb:.1f} MB, {len(cache)} görsel gömüldü)")

    # Yedek: klasörün tamamı zip olarak (üç sayfalık yapı korunur)
    with zipfile.ZipFile(OUT_ZIP, "w", zipfile.ZIP_DEFLATED) as z:
        for f in ["index.html", "telemetry-and-positioning.html", "server-setup.html"]:
            z.write(HERE / f, f"narrtos-wiki/{f}")
        for p in (HERE / "assets").rglob("*"):
            if p.is_file():
                z.write(p, f"narrtos-wiki/{p.relative_to(HERE)}")
    print(f"✓ zip     : {OUT_ZIP.name}  ({OUT_ZIP.stat().st_size / 1048576:.1f} MB)")


if __name__ == "__main__":
    main()
