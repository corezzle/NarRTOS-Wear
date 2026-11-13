# -*- coding: utf-8 -*-
"""docs/wiki/*.md → docs/wiki-html/index.html gövdesi.

Markdown sayfaları tek bir kaydırmalı ana sayfaya çevirir. Telemetri ve
Sunucu Kurulumu ayrı HTML sayfalarıdır; bu üretici onların yerine, _sidebar
sırasındaki doğru noktaya (Yetenekler grubunda) bir yönlendirme bloğu koyar.

Kullanım:  python3 docs/wiki-html/build.py
"""
import html
import re
import unicodedata
from pathlib import Path

HERE = Path(__file__).resolve().parent
WIKI = HERE.parent / "wiki"
OUT = HERE / "index.html"

# _sidebar.md sırası. "Telemetri ve Konumlandırma" bilinçli olarak yok —
# yerine DERIN_BLOK giriyor.
TITLE_FILE = {  # wiki sayfa başlığı → docs/wiki/<dosya>.md
    "Donanım ve Board Varyantları": "hardware-and-board-variants",
    "Telemetri ve Konumlandırma": "telemetry-and-positioning",
    "Çekirdek ve Servisler": "core-and-services",
    "Uygulama Geliştirme": "app-development",
    "Kullanıcı Arayüzü": "user-interface",
    "Komut Arayüzü": "command-interface",
    "OTA Güncelleme": "ota-updates",
    "Sorun Giderme": "troubleshooting",
    "Başlarken": "getting-started",
    "Mimari": "architecture",
}

PAGES = [
    ("Başlarken", "Başlangıç"),
    ("Mimari", "Sistem"),
    ("Çekirdek ve Servisler", "Sistem"),
    ("Donanım ve Board Varyantları", "Sistem"),
    ("Uygulama Geliştirme", "Geliştirme"),
    ("Kullanıcı Arayüzü", "Geliştirme"),
    ("Komut Arayüzü", "Yetenekler"),
    ("__DERIN__", "Yetenekler"),
    ("OTA Güncelleme", "Yetenekler"),
    ("Sorun Giderme", "Yardım"),
]

DERIN_BLOK = """
      <h2 id="derinlemesine">Telemetri, Konum ve Sunucu</h2>
      <p>
        Telemetri ve konumlandırma, kendi ayrı sayfalarında ayrıntılı olarak anlatılıyor —
        animasyonlu akış şemaları, etkileşimli örnekler ve panel ekran görüntüleriyle.
      </p>

      <div class="cards">
        <a class="card" href="telemetry-and-positioning.html">
          <span class="k">Cihaz tarafı</span>
          <h4>Telemetri ve Konumlandırma →</h4>
          <p>
            60 saniyelik rapor döngüsü, BSSID taraması, GPS'siz ±19 m konum çözümü,
            veri dürüstlüğü kuralı ve maliyet koruması.
          </p>
        </a>
        <a class="card" href="server-setup.html">
          <span class="k">Sunucu tarafı</span>
          <h4>Sunucu Kurulumu →</h4>
          <p>
            Backend'i sıfırdan kurun: veritabanı ve şema, ortam değişkenleri,
            konum sağlayıcısı, mesaj kuyrukları ve admin paneli.
          </p>
        </a>
      </div>

      <figure>
        <img src="assets/gif/scan-cycle.gif" alt="Tarama ve rapor döngüsü" />
        <figcaption>
          Tarama ve rapor neden ayrı turlarda? —
          <a href="telemetry-and-positioning.html#dongu">ayrıntılı anlatım</a>
        </figcaption>
      </figure>
"""


# Bir paragrafı sonlandıran blok başlangıçları — convert() içindeki blok
# testleriyle aynı desenler.
BLOCK_START = re.compile(r"^(\s*[-*] |\s*\d+\. |#|>|\||```|---\s*$)")


SCREENS = [
    ("watchface-analog", "Analog kadran", "FACE_ANALOG"),
    ("watchface-digital", "Dijital kadran", "FACE_DIGITAL"),
    ("watchface-activity", "Activity kadranı", "FACE_ACTIVITY"),
    ("watchface-io-control", "Çıkış kontrolü", "FACE_IO_CONTROL"),
    ("launcher", "Ana ekran", "3×3 launcher"),
    ("step-counter", "Adım sayar", "günlük hedef halkası"),
    ("weather", "Hava durumu", "çoklu şehir, canlı veri"),
    ("gallery", "Galeri", "fotoğraf tarayıcı"),
    ("alarm", "Alarmlar", "kurma ekranı"),
    ("notifications", "Bildirimler", "ANCS listesi"),
    ("notification-detail", "Bildirim detayı", "gövde + kapat"),
    ("store-apps", "Mağaza", "Apps sekmesi"),
    ("store-detail", "Mağaza detayı", "uygulama kartı"),
    ("settings", "Ayarlar", "kök menü"),
    ("power-rails", "Güç rayları", "9 toggle + kapat"),
    ("ota", "Yazılım güncelleme", "OTA akışı"),
]


def screens_gallery():
    cells = "\n".join(
        f'        <figure class="screen">\n'
        f'          <img src="assets/watch/{f}.png" alt="{t}" />\n'
        f'          <b>{t}</b><span>{s}</span>\n'
        f'        </figure>'
        for f, t, s in SCREENS
    )
    return (
        '      <div class="device-gifs">\n'
        '        <figure><img src="assets/gif/watchface-selector.gif" alt="Kadran seçici" />\n'
        '          <figcaption>Kadranlar arasında yatay kaydırma</figcaption></figure>\n'
        '        <figure><img src="assets/gif/app-launch.gif" alt="Uygulama açılışı" />\n'
        '          <figcaption>Ana ekrandan uygulamaya ve geri</figcaption></figure>\n'
        '      </div>\n'
        f'      <div class="screens">\n{cells}\n      </div>'
    )


SCREENS_240 = [
    ("watchface-digital", "Dijital kadran", "Rubik Black 100"),
    ("launcher", "Ana ekran", "hücre 88×92, ikon 64"),
    ("settings", "Ayarlar", "içerik 204 px, STEP 60"),
    ("power-rails", "Güç rayları", "STEP 50"),
    ("step-counter", "Adım sayar", "hedef halkası"),
    ("weather", "Hava durumu", "çoklu şehir"),
]


def screens_gallery_240():
    cells = "\n".join(
        f'        <figure class="screen round">\n'
        f'          <img src="assets/watch-240/{f}.png" alt="{t}" />\n'
        f'          <b>{t}</b><span>{s}</span>\n'
        f'        </figure>'
        for f, t, s in SCREENS_240
    )
    return (
        '      <h3>240×240 yuvarlak panelde aynı sistem</h3>\n'
        '      <p>Aynı kod tabanı, yalnızca derleme zamanı seçimiyle yuvarlak '
        'GC9A01A panelde çalışır. Yerleşim değerleri firmware\'den gelir: içerik sütunu '
        '<code>204 px</code> (kenarlardan 18 px içeride), ana listede <code>STEP 60</code>, '
        'güç raylarında <code>STEP 50</code>, launcher hücresi <code>88×92</code>.</p>\n'
        '      <div class="note warn">\n'
        '        <div class="note-t">Yuvarlak panelin kısıtı</div>\n'
        '        <p>204 px genişliğindeki satır ancak <strong>y ≈ 180</strong>\'e kadar daire '
        'içinde kalır; altına inen satırlar kırpılır. Bu yüzden ana listede aynı anda iki satır '
        'tam görünür, güç rayları satırları 180 px\'e daraltılır.</p>\n'
        '      </div>\n'
        '      <div class="device-gifs">\n'
        '        <figure><img src="assets/gif/lcd128-tour.gif" alt="240×240 panel turu" />\n'
        '          <figcaption>Yuvarlak panelde ekranlar arası geçiş</figcaption></figure>\n'
        '      </div>\n'
        f'      <div class="screens">\n{cells}\n      </div>'
    )


def screen_row(img, title, sub, body):
    return (
        '      <div class="screen-row">\n'
        f'        <figure class="screen"><img src="assets/watch/{img}.png" alt="{title}" />'
        f'<b>{title}</b><span>{sub}</span></figure>\n'
        f'        <div class="screen-txt">{body}</div>\n'
        "      </div>"
    )


def gif_row(gif, alt, caption, body):
    """Sol tarafta cihaz GIF'i, sağında açıklama."""
    return (
        '      <div class="screen-row">\n'
        f'        <figure class="gif-cell"><img src="assets/gif/{gif}.gif" alt="{alt}" />\n'
        f'          <figcaption>{caption}</figcaption></figure>\n'
        f'        <div class="screen-txt">{body}</div>\n'
        "      </div>"
    )


def terminal_gif(gif, alt, caption):
    """Başlarken adımlarının sonuna konan terminal animasyonu (760×420)."""
    return (
        f'      <figure class="terminal"><img src="assets/gif/{gif}.gif" alt="{alt}" />\n'
        f'        <figcaption>{caption}</figcaption></figure>'
    )


# Bir alt başlığın (h3) HEMEN ÖNCESİNE eklenen bloklar → bir önceki adımın sonuna
# düşer. Anahtar: (sayfa adı, h3 metninin başı).
ONCESI = {
    "Başlarken": {
        "2. Python kurulu mu, kontrol edin": (
            '      <figure><img src="assets/img/spotlight-terminal.png" alt="Spotlight\'ta Terminal" />\n'
            '        <figcaption>macOS: Spotlight\'a <code>Terminal</code> yazın; ilk sonuç Terminal uygulamasıdır.</figcaption></figure>\n'
            '      <figure><img src="assets/img/windows-start-powershell.png" alt="Windows Başlat aramasında PowerShell" />\n'
            '        <figcaption>Windows: Başlat menüsüne <code>PowerShell</code> yazın; ilk sonuç Windows PowerShell (çizim).</figcaption></figure>'),
        "4. USB sürücüsünü kurun": terminal_gif(
            "terminal-setup", "Python kontrolü ve PlatformIO kurulumu",
            "2. ve 3. adım terminalde: Python sürümü, PlatformIO kurulumu ve doğrulama. "
            "Sürüm numaraları sizde farklı olabilir."),
        "7. Derleyin ve karta yükleyin": terminal_gif(
            "terminal-device-list", "pio device list çıktısı",
            "Kart görünüyorsa <code>pio device list</code> böyle bir satır verir. "
            "Adres (<code>/dev/cu.usbserial-…</code>, <code>COM3</code>) sizde farklı olur."),
        "8. Çalıştığını görün": terminal_gif(
            "terminal-build", "Derleme ve karta yükleme",
            "Derleme ve yükleme akışı. Uzun satırlar akar, sonda <code>SUCCESS</code> "
            "görünür ve kart yeniden başlar."),
        "İlk denemede en sık takılınan yerler": terminal_gif(
            "terminal-monitor", "Seri monitörde açılış kaydı",
            "Seri monitörde açılış kaydı. Son satır arayüzün başladığını gösterir; "
            "çıkmak için Ctrl + C."),
    },
    "Donanım ve Board Varyantları": {
        "Pin haritası (varsayılan board)": (
            '      <figure><img src="assets/img/boot-selftest.svg" alt="Açılış donanım self-test\'i seri monitör çıktısı" />\n'
            '        <figcaption>Soğuk açılışta seri monitöre basılan self-test tablosu — sağlam bir kartta tüm satırlar PASS döner (eksik/derlenmemiş çevrebirim olursa o satır FAIL yerine ATLA/UYARI).</figcaption></figure>'),
    },
}


# Bölüm başlıklarından sonra eklenen görsel bloklar.
EXTRAS = {
    "Mimari": (
        '      <figure><img src="assets/img/architecture-layers.png" alt="Katmanlı mimari" />\n'
        '        <figcaption>Beş katman — yukarı katman aşağıyı çağırır, aşağıdan yukarı '
        'iletişim olay veriyolu ile olur</figcaption></figure>\n'
        '      <figure><img src="assets/img/dual-core.png" alt="Çift çekirdekli görev dağılımı" />\n'
        '        <figcaption>Görevlerin çekirdeklere ve önceliklere dağılımı</figcaption></figure>'
    ),
    "Kullanıcı Arayüzü": screens_gallery(),
    "Donanım ve Board Varyantları": screens_gallery_240(),
    "Çekirdek ve Servisler": gif_row(
        "power-rails", "Güç rayları toggle kontrolü", "Ayarlar → Power Rails",
        "<p><code>power_service</code>, AXP2101 PMIC raylarını çalışma anında tek tek "
        "açıp kapatabilir. Ayarlardaki bu sayfa <strong>dokuz rayı</strong> ayrı anahtarlara "
        "bağlar; en altta saati tamamen kapatan <em>Soft Power Off</em> eylemi onay "
        "dialog'u arkasındadır.</p>"
        "<p>Etiketler cihaz üstünde fiziksel testle doğrulanmıştır — örneğin ALDO1 ses "
        "değil <strong>sensör</strong> rayıdır (kapatınca dokunmatik ölür), ALDO2 ve DCDC2 "
        "ekranı besler.</p>"
        "<div class=\"note warn\"><div class=\"note-t\">DCDC1 listelenmez</div>"
        "<p>Sistem 3.3V rayıdır; kapatılması ESP32'yi öldürür — bunu zaten Soft Power Off "
        "yapar, iki ayrı kontrol kafa karıştırıcı olurdu.</p></div>",
    ),
    "OTA Güncelleme": screen_row(
        "ota", "Yazılım güncelleme", "Ayarlar → Software Update",
        "<p>Cihaz manifest'i sorgular, yeni sürüm varsa boyutuyla birlikte gösterir. "
        "İndirme yedek partisyona yazılır; doğrulama başarılıysa bir sonraki açılışta "
        "yeni imaj devreye girer.</p>"
        '<figure><img src="assets/img/02-admin-devices.png" alt="Admin paneli cihaz listesi" />'
        "<figcaption>Panelden hedef sürüm atama — <em>Push Update</em> ile cihaza "
        "zorunlu sürüm işaretlenir</figcaption></figure>",
    ),
}


def slug(text):
    t = unicodedata.normalize("NFKD", text.lower())
    tr = {"ı": "i", "ğ": "g", "ü": "u", "ş": "s", "ö": "o", "ç": "c"}
    t = "".join(tr.get(c, c) for c in t)
    t = "".join(c for c in t if not unicodedata.combining(c))
    t = re.sub(r"[^a-z0-9]+", "-", t).strip("-")
    return t or "bolum"


def inline(s):
    """Satır içi markdown → HTML. Kaçış önce, etiketler sonra."""
    s = html.escape(s)
    s = re.sub(r"`([^`]+)`", r"<code>\1</code>", s)
    s = re.sub(r"\*\*([^*]+)\*\*", r"<strong>\1</strong>", s)
    s = re.sub(r"(?<!\w)\*([^*\n]+)\*(?!\w)", r"<em>\1</em>", s)
    # [metin](hedef) — wiki içi hedefler düz metne düşer, dış bağlantılar link olur
    def link(m):
        text, href = m.group(1), m.group(2)
        if href.startswith(("http://", "https://", "#")):
            return f'<a href="{href}">{text}</a>'
        return text
    s = re.sub(r"\[([^\]]+)\]\(([^)]+)\)", link, s)
    # [[Sayfa Adı]] — GitLab wiki söz dizimi; burada hedefi yok, düz metne indir
    s = re.sub(r"\[\[([^\]|]+)(?:\|[^\]]+)?\]\]", r"\1", s)   # [[Başlık|dosya]] → Başlık
    return s


def highlight(code):
    """Kod bloğu için hafif renklendirme — yorum ve dizeler."""
    out = []
    for line in html.escape(code).split("\n"):
        m = re.match(r"^(\s*)(#|//)(.*)$", line)
        if m:
            out.append(f'{m.group(1)}<span class="c-cmt">{m.group(2)}{m.group(3)}</span>')
            continue
        line = re.sub(r"(&quot;[^&]*?&quot;)", r'<span class="c-str">\1</span>', line)
        out.append(line)
    return "\n".join(out)


MADDE = {"ul": re.compile(r"^\s*[-*] "), "ol": re.compile(r"^\s*\d+\. ")}


def liste_topla(lines, i, isaret):
    """Maddeyi ve ARDINDAN GELEN girintili devam satırlarını tek maddede topla.

    Devam satırını maddeye bağlamazsak liste orada biter, bir sonraki madde yeni
    bir <ol> açar ve numaralandırma her maddede 1'den başlar — kaynakta 1,2,3,4
    yazan liste sayfada "1. 1. 1. 1." görünüyordu.

    Girintili ``` bloğu devam satırı sayılmaz: liste maddesinin altına
    iliştirilen kod örnekleri (bkz. Başlarken › PlatformIO Core) kendi <pre>
    bloğu olarak kalmalı.
    """
    maddeler = []
    while i < len(lines) and isaret.match(lines[i]):
        maddeler.append(isaret.sub("", lines[i], count=1)); i += 1
        while (i < len(lines) and lines[i].strip()
               and lines[i][:1].isspace()
               and not lines[i].lstrip().startswith("```")
               and not MADDE["ul"].match(lines[i])
               and not MADDE["ol"].match(lines[i])):
            maddeler[-1] += " " + lines[i].strip(); i += 1
    return maddeler, i


def tablo_hucreleri(satir):
    """Tablo satırını hücrelere böl. `\\|` hücre içeriğidir, ayraç değildir —
    ör. `app launch <isim\\|id>`. Kaçışı görmezsek satır fazladan hücreye
    bölünür ve açıklama sütunları kayar."""
    s = satir.strip()
    if s.startswith("|"):
        s = s[1:]
    if s.endswith("|") and not s.endswith("\\|"):
        s = s[:-1]
    return [c.strip().replace("\\|", "|") for c in re.split(r"(?<!\\)\|", s)]


def convert(md):
    """Markdown gövdesi → HTML parçaları listesi."""
    lines = md.split("\n")
    out, i = [], 0
    while i < len(lines):
        ln = lines[i]

        # kod bloğu — liste içinde girintili de olabilir ("  ```bash")
        if ln.lstrip().startswith("```"):
            girinti = len(ln) - len(ln.lstrip())
            i += 1
            buf = []
            while i < len(lines) and not lines[i].lstrip().startswith("```"):
                buf.append(lines[i][girinti:] if lines[i][:girinti].isspace() else lines[i])
                i += 1
            i += 1
            out.append(f"<pre><code>{highlight(chr(10).join(buf))}</code></pre>")
            continue

        # tablo
        if ln.startswith("|") and i + 1 < len(lines) and re.match(r"^\|[\s:|-]+\|$", lines[i + 1]):
            head = tablo_hucreleri(ln)
            i += 2
            rows = []
            while i < len(lines) and lines[i].startswith("|"):
                rows.append(tablo_hucreleri(lines[i])); i += 1
            th = "".join(f"<th>{inline(c)}</th>" for c in head)
            tb = "".join("<tr>" + "".join(f"<td>{inline(c)}</td>" for c in r) + "</tr>" for r in rows)
            out.append(f'<div class="tablewrap"><table><thead><tr>{th}</tr></thead><tbody>{tb}</tbody></table></div>')
            continue

        # alıntı → bilgi kutusu
        if ln.startswith(">"):
            buf = []
            while i < len(lines) and lines[i].startswith(">"):
                buf.append(lines[i].lstrip(">").strip()); i += 1
            text = " ".join(x for x in buf if x)
            title = ""
            m = re.match(r"^\*\*(.+?)[:：]?\*\*[:：]?\s*(.*)$", text)
            if m:
                title, text = m.group(1), m.group(2)
            cls = "note"
            low = (title + " " + text).lower()
            if any(w in low for w in ("dikkat", "uyarı", "tuzak", "asla", "yasak")):
                cls = "note warn"
            head = f'<div class="note-t">{inline(title)}</div>' if title else ""
            out.append(f'<div class="{cls}">{head}<p>{inline(text)}</p></div>')
            continue

        # listeler
        if MADDE["ul"].match(ln):
            buf, i = liste_topla(lines, i, MADDE["ul"])
            out.append("<ul>" + "".join(f"<li>{inline(x)}</li>" for x in buf) + "</ul>")
            continue
        if MADDE["ol"].match(ln):
            buf, i = liste_topla(lines, i, MADDE["ol"])
            out.append("<ol>" + "".join(f"<li>{inline(x)}</li>" for x in buf) + "</ol>")
            continue

        # başlıklar (sayfa H1'i çağıran taraf basar)
        if ln.startswith("### "):
            out.append(f"<h4>{inline(ln[4:])}</h4>"); i += 1; continue
        if ln.startswith("## "):
            out.append(f"<h3>{inline(ln[3:])}</h3>"); i += 1; continue
        if ln.startswith("# "):
            i += 1; continue
        if ln.strip() == "---":
            out.append("<hr />"); i += 1; continue

        # paragraf — blok başlangıçlarında durur. Desenler yukarıdaki blok
        # testleriyle birebir aynı olmalı; aksi halde hiçbir dala girmeyen
        # satırda `i` ilerlemez ve döngü kilitlenir (ör. rakamla başlayan
        # düz cümle: "60 saniyede bir rapor gönderilir").
        if ln.strip():
            buf = [lines[i].strip()]
            i += 1
            while (i < len(lines) and lines[i].strip()
                   and not BLOCK_START.match(lines[i])):
                buf.append(lines[i].strip()); i += 1
            out.append(f"<p>{inline(' '.join(buf))}</p>")
            continue
        i += 1
    return out


def main():
    parts, toc_groups, last_group = [], [], None
    for name, group in PAGES:
        if name == "__DERIN__":
            parts.append(DERIN_BLOK)
            toc_groups.append(("Telemetri, Konum ve Sunucu", "derinlemesine"))
            continue
        path = WIKI / f"{TITLE_FILE.get(name, name)}.md"
        if not path.exists():
            print(f"!! bulunamadı: {path}")
            continue
        body = convert(path.read_text(encoding="utf-8"))
        for baslik, blok in ONCESI.get(name, {}).items():
            idx = next((i for i, b in enumerate(body)
                        if re.match(r"\s*<h3[^>]*>" + re.escape(baslik), b)), None)
            if idx is None:
                print(f"!! ONCESI: '{baslik}' başlığı {name} içinde bulunamadı"); continue
            body.insert(idx, blok)
        sid = slug(name)
        block = (
            f'      <h2 id="{sid}">{html.escape(name)}</h2>\n      '
            + "\n      ".join(body)
        )
        if name in EXTRAS:
            block += "\n" + EXTRAS[name]
        parts.append(block)
        toc_groups.append((name, sid))
        last_group = group

    src = OUT.read_text(encoding="utf-8")
    block = "\n\n".join(parts)
    new = re.sub(
        r"(<!-- WIKI:BASLA -->).*?(<!-- WIKI:BITIS -->)",
        lambda m: m.group(1) + "\n" + block + "\n      " + m.group(2),
        src, flags=re.S,
    )
    OUT.write_text(new, encoding="utf-8")
    print(f"✓ {len(toc_groups)} bölüm yazıldı → {OUT.name}")
    for t, s in toc_groups:
        print(f"   #{s}  {t}")


if __name__ == "__main__":
    main()
