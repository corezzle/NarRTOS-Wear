# -*- coding: utf-8 -*-
"""HTML wiki → panele yapıştırılabilir Markdown bölümleri.

Panel (design-frontend) bölüm içeriğini MarkDownEditor ile saklıyor; HTML
kabul etmiyor. Bu betik narrtos-wiki'nin SON HALİNİ kaynak alır (md dosyaları
değil — Telemetri yeniden yazıldı, Sunucu Kurulumu'nun md'si hiç yok) ve her
panel başlığı için bir .md dosyası üretir.

Çıktı:  docs/panel-content/NN-<slug>.md  +  00-IMAGE-LIST.md

Kullanım:  python3 docs/wiki-html/to-panel.py
"""
import html as htmllib
import re
import unicodedata
from html.parser import HTMLParser
from pathlib import Path

HERE = Path(__file__).resolve().parent
OUT = HERE.parent / "panel-content"

# Wiki alt sayfası → panelde karşılık gelen dökümanın adresi. Panel TR'yi
# öneksiz, EN'i /en/ önekiyle sunuyor; burada TR üretiliyor, en/ dosyaları
# elle aynı adresin /en/'li hâlini taşır.
# Adresi olmayan sayfa düz metin/kutu olarak kalır (döküman henüz açılmadı).
INSTALL_URL = None   # tarayıcıdan yükleme sayfasının yayın adresi; None iken düz metin

ALT_SAYFALAR = {
    "telemetry-and-positioning": "/document-content/narrtos-telemetri-ve-konumlandirma-7",
    "server-setup": "/document-content/narrtos-sunucu-kurulumu-8",
    # Panelde döküman açılınca adresini yaz; None iken bağlantı düz metin kalır,
    # sayfa sonu kartı ise hiç basılmaz.
    "vscode-setup": "/document-content/narrtos-wear-vs-code-kurulumu-11",
    "vs-code": "/document-content/vs-code-kurulumu-10",   # genel VS Code Kurulumu
    "imager-install": "/document-content/narrtos-wear-corezzle-imager-ile-yukleme-12",
    "index": "/document-content/narrtos-wear-6",   # Wear dökümanının kendisi (alt sayfalardan geri dönüş)
}

BLOCK = {"p", "div", "section", "figure", "figcaption", "ul", "ol", "li", "pre",
         "table", "thead", "tbody", "tr", "th", "td", "h1", "h2", "h3", "h4",
         "details", "summary", "hr", "br", "nav", "aside", "main", "footer"}


class ToMarkdown(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.out = []          # tamamlanmış markdown blokları
        self.inline = []       # işlenen satır içi metin
        self.stack = []        # açık etiketler (tag, attrs)
        self.pre = None        # kod bloğu tamponu
        self.row = None        # tablo hücreleri
        self.tbl = None        # tablo satırları
        self.list_stack = []   # ("ul"|"ol", sayaç)
        self.images = []       # (src, alt) — görsel listesi için
        self.skip_depth = 0    # atlanan ağaçlar (kopyala butonu vb.)
        self.divs = []         # (class, çıktı işareti) — not/kart ağacını izler

    def ancestor(self, name):
        return any(name in c.split() for c, _, _ in self.divs)

    # ── yardımcılar ────────────────────────────────────────────────
    def cls(self, attrs):
        return dict(attrs).get("class", "") or ""

    def flush(self):
        t = "".join(self.inline)
        # Kaynak HTML'de paragraflar birden çok satıra yazılı; satır sonlarını
        # da boşluğa indirgemezsek markdown'da paragraf ortasında kırılma olur.
        t = re.sub(r"\s+", " ", t.replace("  \n", "\x00")).replace("\x00", "  \n").strip()
        self.inline = []
        return t

    def emit(self, text):
        if text:
            self.out.append(text)

    # ── etiket başlangıcı ──────────────────────────────────────────
    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        c = self.cls(attrs)

        if self.skip_depth:
            # void etiketlerin kapanışı gelmez; sayacı artırırsak bir daha sıfıra
            # dönmez ve sayfanın kalanı sessizce yutulur
            if tag not in ("img", "br", "hr", "input", "meta", "link"):
                self.skip_depth += 1
            return
        # panelde karşılığı olmayan süs öğeleri
        if tag == "button" or "copy" in c.split() or c in ("toc", "toc-list", "topbar", "topbar-in"):
            self.skip_depth = 1
            return
        # Sayfa altbilgisi ve "sonraki sayfa" kartı siteye ait; panelde bölüm
        # ağacı bu işi zaten yapıyor. Atlanmazsa bölüm metnine sızıyorlar.
        if tag == "footer":
            self.skip_depth = 1
            return
        # JS ile dolan etkileşimli demo: kod bloğu HTML'de BOŞ, yalnız ✓
        # işaretleri var. Panelde çalışmayacağı için statik karşılığını basıyoruz.
        if "demo" in c.split():
            self.emit(self.flush())
            self.emit(html_demo())
            self.skip_depth = 1
            return

        if tag == "pre":
            self.pre = []
            return
        if self.pre is not None:
            return

        if tag == "img":
            src, alt = a.get("src", ""), a.get("alt", "")
            self.images.append((src, alt))
            name = src.split("/")[-1]
            self.emit(f"![{alt}](GORSEL: {name})")
            return

        if tag == "table":
            self.tbl = []
            return
        if tag == "tr":
            self.row = []
            return
        if tag in ("td", "th"):
            self.inline = []
            return

        if tag == "div":
            self.emit(self.flush())
            self.divs.append((c, len(self.out), a))
            return

        if tag in ("ul", "ol"):
            self.emit(self.flush())
            self.list_stack.append([tag, 0])
            return
        if tag == "li":
            self.inline = []
            return

        if tag == "span" and "k" in c.split():
            self.inline.append("**")
            self.stack.append(("span-k", a))
            return
        if tag in ("strong", "b"):
            self.inline.append("**")
        elif tag in ("em", "i"):
            self.inline.append("*")
        elif tag == "code":
            self.inline.append("`")
        elif tag == "a":
            self.inline.append("[")
            self.stack.append(("a", a))
            return
        elif tag == "br":
            self.inline.append("  \n")
        elif tag in BLOCK:
            self.emit(self.flush())

        self.stack.append((tag, a))

    # ── metin ──────────────────────────────────────────────────────
    def handle_data(self, data):
        if self.skip_depth:
            return
        if self.pre is not None:
            self.pre.append(data)
            return
        self.inline.append(data)

    # ── etiket sonu ────────────────────────────────────────────────
    def handle_endtag(self, tag):
        if self.skip_depth:
            self.skip_depth -= 1
            return

        if tag == "pre":
            code = "".join(self.pre or []).strip("\n")
            self.pre = None
            self.emit("```\n" + code + "\n```")
            return
        if self.pre is not None:
            return

        if tag in ("td", "th"):
            if self.row is not None:
                self.row.append(self.flush())
            return
        if tag == "tr":
            if self.tbl is not None and self.row:
                self.tbl.append(self.row)
            self.row = None
            return
        if tag == "table":
            if self.tbl:
                head, *body = self.tbl
                # Markdown tablosu bitişik satır ister; her satırı ayrı blok
                # olarak basarsak araya boş satır girer ve tablo çözülmez.
                # Hücre içindeki boru karakteri (ör. `set wifi|bt on|off`)
                # kaçırılmazsa satır fazladan sütuna bölünür.
                def hucre(x):
                    return x.replace("|", "\\|")
                satirlar = ["| " + " | ".join(hucre(h) for h in head) + " |",
                            "|" + "|".join(["---"] * len(head)) + "|"]
                for r in body:
                    r = r + [""] * (len(head) - len(r))
                    satirlar.append("| " + " | ".join(hucre(x) for x in r[:len(head)]) + " |")
                self.emit("\n".join(satirlar))
            self.tbl = None
            return

        if tag == "li":
            txt = self.flush()
            if txt and self.list_stack:
                kind, n = self.list_stack[-1]
                if kind == "ol":
                    n += 1
                    self.list_stack[-1][1] = n
                    self.emit(f"{n}. {txt}")
                else:
                    self.emit(f"- {txt}")
            return
        if tag in ("ul", "ol"):
            if self.list_stack:
                self.list_stack.pop()
            return

        if tag == "span" and self.stack and self.stack[-1][0] == "span-k":
            self.stack.pop()
            self.inline.append("** · ")
            return
        if tag in ("strong", "b"):
            self.inline.append("**")
            return
        if tag in ("em", "i"):
            self.inline.append("*")
            return
        if tag == "code":
            self.inline.append("`")
            return
        if tag == "a":
            href = ""
            for i in range(len(self.stack) - 1, -1, -1):
                if self.stack[i][0] == "a":
                    href = self.stack[i][1].get("href", "")
                    self.stack.pop(i)
                    break
            # Alt sayfa bağlantıları panel dökümanına yönlenir. Sayfa içi çapa
            # (#dongu gibi) düşürülür — panel bölümlere o id'leri vermiyor.
            if "web-install/" in href:
                if INSTALL_URL:
                    self.inline.append(f"]({INSTALL_URL})"); return
                href = "#"   # adres yok → düz metne indir
            sayfa = re.match(r"([\w-]+)\.html(#.*)?$", href)
            if sayfa and ALT_SAYFALAR.get(sayfa.group(1)):
                self.inline.append(f"]({ALT_SAYFALAR[sayfa.group(1)]})")
                return
            # belge içi çapalar panelde çalışmaz → düz metne indir
            if href.startswith("#") or href.endswith(".html") or ".html#" in href:
                self.inline.append("")
                # köşeli parantezi geri al
                s = "".join(self.inline)
                i = s.rfind("[")
                if i >= 0:
                    self.inline = [s[:i] + s[i + 1:]]
            else:
                self.inline.append(f"]({href})")
            return

        if tag == "div":
            self.emit(self.flush())
            if self.divs:
                c, mark, dattr = self.divs.pop()
                cs = c.split()
                govde = [b.strip() for b in self.out[mark:] if b.strip()]

                if "nextcard" in cs:
                    # Sayfa sonundaki "Sonraki adım" kartı. Alt sayfalar ayrı
                    # döküman olduğu için sitedeki gibi bir sonraki dökümana
                    # götürür; hedefi yoksa hiç basılmaz (döküman açılmamış).
                    del self.out[mark:]
                    hedef = dattr.get("data-href")
                    metin = [b for b in govde if b.strip() and b.strip() != "→"]
                    if hedef and len(metin) >= 2:
                        self.out.append(html_sonraki(metin[0], metin[1],
                                                     metin[2] if len(metin) > 2 else "", hedef))
                elif "note-t" in cs:
                    # Not kutusunun başlığı. İşaretleyip bırakıyoruz ki dıştaki
                    # "note" dalı başlığı gövdeden ayırt edebilsin.
                    del self.out[mark:]
                    self.out.append("\x03" + " ".join(govde))
                elif "note" in cs:
                    del self.out[mark:]
                    tur = next((x for x in cs if x in ("warn", "stop")), "")
                    # Başlıksız kutularda ilk paragrafı başlık sanıp aynı metni
                    # hem başlığa hem gövdeye basıyorduk.
                    if govde and govde[0].startswith("\x03"):
                        bas, govde = ic_html(govde[0][1:]), govde[1:]
                    else:
                        bas = ""
                    self.out.append(html_not(tur, bas, "<br><br>".join(ic_html(b) for b in govde)))
                elif "stat" in cs:
                    # "**2**çekirdek\n240 MHz" → (sayı, etiket)
                    del self.out[mark:]
                    t = " ".join(govde)
                    m = re.match(r"^\*\*(.+?)\*\*\s*(.*)$", t, re.S)
                    say, alt = (m.group(1), m.group(2)) if m else (t, "")
                    self.out.append("\x02STAT\x02" + say + "\x02" + alt.replace("  \n", "<br>").replace("\n", "<br>"))
                elif "card" in cs:
                    del self.out[mark:]
                    duz = [re.sub(r"\*\*(.+?)\*\*", r"\1", b).strip(" ·") for b in govde]
                    etiket = duz[0] if len(duz) > 2 else ""
                    baslik = duz[1] if len(duz) > 2 else (duz[0] if duz else "")
                    aciklama = ic_html(govde[-1]) if len(duz) > 1 else ""
                    self.out.append("\x02CARD\x02" + etiket + "\x02" + baslik + "\x02"
                                    + aciklama + "\x02" + (dattr.get("data-href") or ""))
                elif "screen-row" in cs:
                    del self.out[mark:]
                    # İlk görsel sol sütuna, hemen ardındaki italik satır onun
                    # altyazısı olur. Sonraki görseller (ör. OTA'daki panel
                    # ekran görüntüsü) metin sütununda kalır — aksi halde
                    # sonuncu ilkinin yerini alıp birini kaybediyorduk.
                    #
                    # figure.screen'de altyazı italik değil, <b>Ad</b><span>Ölçü</span>
                    # çiftidir ve resmin ALTINDA iki satır durur. Tanımazsak metin
                    # sütununa düşüp "Yazılım güncellemeAyarlar → Software Update"
                    # diye bitişik çıkıyordu.
                    gorsel, altyazi, metin = "", "", []
                    for b in govde:
                        resim = b.startswith("![")
                        italik = b.startswith("*") and b.endswith("*")
                        etiket = re.match(r"^\*\*(.+?)\*\*\s*(.*)$", b, re.S)
                        if resim and not gorsel:
                            gorsel = b[b.index("(") + 1:b.rindex(")")].replace("GORSEL: ", "GORSEL:")
                        elif italik and gorsel and not altyazi and not metin:
                            altyazi = b.strip("*")
                        elif etiket and gorsel and not altyazi and not metin:
                            altyazi = (etiket.group(1), etiket.group(2))
                        elif resim:
                            src = b[b.index("(") + 1:b.rindex(")")].replace("GORSEL: ", "GORSEL:")
                            metin.append(f'<img src="{src}" style="width:100%;display:block;'
                                         f'border-radius:10px;margin:14px 0" />')
                        elif italik:
                            metin.append(f'<div style="font-size:13px;{SOLUK};margin:-6px 0 12px">'
                                         f'{ic_html(b.strip("*"))}</div>')
                        elif b.startswith("<div"):
                            metin.append(b)
                        else:
                            metin.append("<p>" + ic_html(b) + "</p>")
                    self.out.append(html_yan_yana(gorsel, altyazi, "".join(metin)))
                elif "screens" in cs:
                    # Ekran küçük resimleri ızgarası. İşlemezsek her görsel ayrı
                    # blok olup alt alta dizilir ve "**Ad**Ölçü" tek satıra yapışır.
                    del self.out[mark:]
                    self.out.append(html_ekran_izgarasi(gorsel_hucreleri(govde)))
                elif "device-gifs" in cs:
                    del self.out[mark:]
                    self.out.append(html_gif_satiri(gorsel_hucreleri(govde)))
                elif "stats" in cs or "cards" in cs:
                    kutular = [b for b in self.out[mark:] if b.startswith("\x02")]
                    digerleri = [b for b in self.out[mark:] if not b.startswith("\x02")]
                    del self.out[mark:]
                    if kutular and kutular[0].startswith("\x02STAT"):
                        self.out.append(html_kutular([tuple(b.split("\x02")[2:4]) for b in kutular]))
                    elif kutular:
                        self.out.append(html_kartlar([tuple(b.split("\x02")[2:6]) for b in kutular]))
                    self.out.extend(digerleri)
            return

        if tag in ("h2", "h3", "h4", "summary"):
            lvl = {"h2": 2, "h3": 3, "h4": 4, "summary": 3}[tag]
            txt = self.flush()
            if txt:
                # Kart içindeki h4 başlık değil, kartın adı → kalın metin
                if self.ancestor("card"):
                    self.emit("**" + txt + "**")
                else:
                    self.emit("\x01%d\x01" % lvl + txt)
            return
        if tag == "figcaption":
            txt = self.flush()
            if txt:
                # Bağlantılı altyazıyı markdown olarak basınca panelde
                # tıklanabilir olmuyor. Satır içi HTML burada zaten çalışıyor
                # (kutular onunla basılıyor), altyazıyı da ona çeviriyoruz.
                if re.search(r"\]\([^)]+\)", txt):
                    self.emit(f'<div style="font-size:12.5px;{SOLUK};text-align:center;'
                              f'margin:-6px 0 14px"><em>{ic_html(txt)}</em></div>')
                else:
                    self.emit("*" + txt + "*")
            return
        if tag == "hr":
            self.emit("---")
            return

        if tag in BLOCK:
            self.emit(self.flush())

        for i in range(len(self.stack) - 1, -1, -1):
            if self.stack[i][0] == tag:
                self.stack.pop(i)
                break

    def result(self):
        self.emit(self.flush())
        md = "\n\n".join(x for x in self.out if x.strip())

        # Başlık seviyelerini normalize et: dosyadaki en üst başlık ## olsun
        seviyeler = [int(m) for m in re.findall(r"\x01(\d)\x01", md)]
        kaydir = (2 - min(seviyeler)) if seviyeler else 0
        md = re.sub(r"\x01(\d)\x01",
                    lambda m: "#" * min(6, int(m.group(1)) + kaydir) + " ", md)

        md = re.sub(r"\n{3,}", "\n\n", md)
        md = re.sub(r"[ \t]+\n", "\n", md)
        md = re.sub(r"\*\*\s*\*\*", "", md)          # boş kalın işaretleri
        return md.strip()



# ── Panelde HTML çalışıyor (ContentEleven.vue markdown-it html:true) ──────
# Kutu/kart/bilgi kutusu gibi görsel bloklar markdown'da karşılıksız kalıyordu;
# bunları satır içi stilli HTML olarak basıyoruz. Açık temaya göre renklendi.
# Panelin açık ve koyu teması var. Sabit renk verirsek biri bozuluyordu
# (koyu temada beyaz kart üstünde açık başlık = okunmuyordu). Bu yüzden:
#   zemin/kenar → yarı saydam gri (iki temada da doğru tonu alır)
#   metin       → color:inherit (sayfanın metin rengini miras alır)
#   ikincil     → opacity ile soluklaştırılır, sabit gri değil
KUTU = ("background:rgba(128,128,128,.10);border:1px solid rgba(128,128,128,.28);"
        "border-radius:12px")
KART = ("background:rgba(128,128,128,.08);border:1px solid rgba(128,128,128,.24);"
        "border-radius:14px;padding:20px")
TRQ = "#12A0AD"          # iki zeminde de okunan orta ton turkuaz
SOLUK = "color:inherit;opacity:.72"
NOT_RENK = {
    "":     ("#12A0AD", "rgba(18,160,173,.10)"),
    "warn": ("#D9922F", "rgba(217,146,47,.12)"),
    "stop": ("#D95757", "rgba(217,87,87,.12)"),
}


def html_kutular(parcalar):
    """<div class="stat"> grubu → yan yana kutular (tek blok, boş satırsız)."""
    hucre = ("<div style=\"flex:1 1 120px;max-width:190px;{k};padding:16px 10px;text-align:center\">"
             "<div style=\"font-size:24px;font-weight:800;color:{t};line-height:1.1\">{s}</div>"
             "<div style=\"font-size:12.5px;{g};margin-top:6px;line-height:1.35\">{a}</div></div>")
    ic = "".join(hucre.format(k=KUTU, t=TRQ, g=SOLUK, s=say, a=alt) for say, alt in parcalar)
    return f'<div style="display:flex;gap:10px;flex-wrap:wrap;margin:18px 0">{ic}</div>'


def html_kartlar(parcalar):
    """<div class="card"> grubu → ızgara kartlar.

    Parça (etiket, başlık, açıklama) ya da (etiket, başlık, açıklama, hedef).
    Hedefi olan kart tıklanabilir olur — köprü bölümdeki iki kart alt sayfa
    dökümanlarına gider."""
    hucre = ("<div style=\"{k}\">"
             "<div style=\"font-size:11.5px;font-weight:800;letter-spacing:1.2px;"
             "text-transform:uppercase;color:{t}\">{e}</div>"
             "<div style=\"font-size:17px;font-weight:700;margin:8px 0 6px;color:inherit\">{b}</div>"
             "<div style=\"font-size:14px;{g};line-height:1.5\">{a}</div></div>")
    parcalar = [p if len(p) == 4 else (*p, "") for p in parcalar]
    ic = "".join(
        (f'<a href="{h}" style="text-decoration:none;color:inherit;display:block">{kutu}</a>'
         if h else kutu)
        for e, b, a, h in parcalar
        for kutu in [hucre.format(k=KART, t=TRQ, g=SOLUK, e=e, b=b, a=a)])
    return ('<div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(250px,1fr));'
            f'gap:14px;margin:18px 0">{ic}</div>')


def html_yan_yana(gorsel, altyazi, metin):
    """Solda cihaz görseli, sağında açıklama — HTML wiki'deki .screen-row.
    src alanı yer tutucudur; panelde görseli yükleyip URL'i buraya koyacaksın.

    altyazi ya düz metin (gif altyazısı) ya da (ad, ölçü) çiftidir; çift olduğunda
    ekran ızgarasındaki gibi kalın ad + soluk ölçü olarak alt alta basılır."""
    if isinstance(altyazi, tuple):
        ad, olcu = altyazi
        alt_html = (f'<b style="display:block;font-size:14px;font-weight:700;margin-top:12px;'
                    f'color:inherit">{ic_html(ad)}</b>'
                    f'<span style="display:block;font-size:12.5px;{SOLUK};margin-top:2px">'
                    f'{ic_html(olcu)}</span>')
    elif altyazi:
        alt_html = f'<figcaption style="font-size:12.5px;{SOLUK};margin-top:8px">{altyazi}</figcaption>'
    else:
        alt_html = ""
    fig = (f'<figure style="flex:0 0 190px;margin:0;text-align:center">'
           f'<img src="{gorsel}" style="width:100%;display:block;border-radius:10px" />'
           + alt_html
           + "</figure>")
    return ('<div style="display:flex;gap:22px;align-items:flex-start;flex-wrap:wrap;margin:18px 0">'
            + fig
            + f'<div style="flex:1 1 320px;min-width:280px">{metin}</div></div>')


def gorsel_hucreleri(govde):
    """Blok listesini (görsel, [ardından gelen metinler]) hücrelerine böl.
    Görsel kendi bloğunu, figure'ın kalan içeriği ayrı bloğu üretiyor."""
    hucreler = []
    for b in govde:
        if b.startswith("!["):
            src = b[b.index("(") + 1:b.rindex(")")].replace("GORSEL: ", "GORSEL:")
            alt = b[2:b.index("](")]
            hucreler.append([src, alt, []])
        elif hucreler and b.strip():
            hucreler[-1][2].append(b)
    return hucreler


def html_ekran_izgarasi(hucreler):
    """<div class="screens"> → yan yana akan ekran kartları.
    Wiki'deki ızgara: auto-fill, minmax(148px, 1fr). Panelde de aynı davranış."""
    hucre = ('<figure style="margin:0;text-align:center">'
             '<img src="{s}" alt="{alt}" style="width:100%;display:block;border-radius:0" />'
             '<b style="display:block;font-size:14px;font-weight:700;margin-top:12px;'
             'color:inherit">{a}</b>'
             '<span style="display:block;font-size:12.5px;{g};margin-top:2px">{o}</span>'
             "</figure>")
    parcalar = []
    for src, alt, metinler in hucreler:
        t = " ".join(metinler)
        m = re.match(r"^\*\*(.+?)\*\*\s*(.*)$", t, re.S)
        ad, olcu = (m.group(1), m.group(2)) if m else (t, "")
        parcalar.append(hucre.format(s=src, alt=htmllib.escape(alt, quote=True),
                                     a=ic_html(ad), o=ic_html(olcu), g=SOLUK))
    return ('<div style="display:grid;grid-template-columns:repeat(auto-fill,minmax(148px,1fr));'
            f'gap:22px 18px;margin:28px 0">{"".join(parcalar)}</div>')


def html_gif_satiri(hucreler):
    """<div class="device-gifs"> → ortalanmış, yan yana cihaz GIF'leri."""
    hucre = ('<figure style="margin:0;max-width:260px;flex:0 1 auto;text-align:center">'
             '<img src="{s}" alt="{alt}" style="width:100%;display:block;border-radius:0" />'
             '<figcaption style="font-size:12.5px;{g};margin-top:4px">{c}</figcaption>'
             "</figure>")
    parcalar = [hucre.format(s=src, alt=htmllib.escape(alt, quote=True),
                             c=ic_html(" ".join(m).strip("*")), g=SOLUK)
                for src, alt, m in hucreler]
    return ('<div style="display:flex;gap:30px;justify-content:center;flex-wrap:wrap;'
            f'margin:30px 0">{"".join(parcalar)}</div>')


def html_sonraki(etiket, baslik, aciklama, hedef):
    """Sayfa sonu "Sonraki adım" kartı — sitedeki .next bloğunun karşılığı.

    Dıştaki <div> şart: markdown-it yalnız blok düzeyi etiketlerde ham HTML
    bloğu açar, <a> satır içi sayılır. Kartı doğrudan <a> ile başlatınca
    paragraf gibi ayrıştırılıp içindeki div'ler alt alta dağılıyordu."""
    return ('<div style="margin:40px 0 0">'
            f'<a href="{hedef}" style="display:flex;align-items:center;gap:22px;'
            'flex-wrap:wrap;border:1px solid #12A0AD;background:rgba(18,160,173,.07);'
            'border-radius:14px;padding:26px 28px;text-decoration:none;'
            'color:inherit"><div>'
            f'<div style="font-size:12.5px;font-weight:800;letter-spacing:1.4px;'
            f'text-transform:uppercase;color:{TRQ}">{ic_html(etiket)}</div>'
            f'<div style="font-size:22px;font-weight:800;margin-top:4px;color:inherit">'
            f'{ic_html(baslik)}</div>'
            f'<div style="font-size:15.5px;{SOLUK};margin-top:4px">{ic_html(aciklama)}</div>'
            '</div>'
            f'<div style="margin-left:auto;font-size:32px;color:{TRQ}">→</div></a></div>')


def html_demo():
    """Sitedeki etkileşimli 'veri dürüstlüğü' demosunun statik karşılığı.
    Panelde JS çalışmadığı için iki durumu yan yana tablo olarak gösteriyoruz."""
    tuketiciler = [("Koordinat alanları", "latitude / longitude"),
                   ("location-data-queue", "konum tüketen sistemler"),
                   ("Admin paneli", "konum olarak, OSM bağlantısıyla")]
    satir = ('<div style="display:grid;grid-template-columns:2fr 1fr 1fr;gap:8px;'
             'align-items:center;padding:10px 0;border-top:1px solid rgba(128,128,128,.20)">'
             '<div><div style="font-weight:700;color:inherit">{n}</div>'
             '<div style="font-size:12.5px;{g}">{s}</div></div>'
             '<div style="text-align:center;color:#12A0AD;font-weight:800">✓</div>'
             '<div style="text-align:center;{g};font-weight:800">✕</div></div>')
    basliklar = ('<div style="display:grid;grid-template-columns:2fr 1fr 1fr;gap:8px;'
                 'font-size:11.5px;font-weight:800;letter-spacing:.6px;text-transform:uppercase;'
                 f'{SOLUK}"><div>Tüketici</div>'
                 '<div style="text-align:center">fallback: null</div>'
                 '<div style="text-align:center">fallback: "ipf"</div></div>')
    ic = "".join(satir.format(n=n, s=s, g=SOLUK) for n, s in tuketiciler)
    return (f'<div style="{KUTU};padding:16px 18px;margin:18px 0">'
            '<div style="font-weight:800;margin-bottom:10px;color:inherit">'
            'Sonuç değiştiğinde tüketicilere ne gider</div>'
            f'{basliklar}{ic}</div>')


def html_not(tur, baslik, govde):
    kenar, zemin = NOT_RENK.get(tur, NOT_RENK[""])
    bas = (f'<div style="font-weight:800;margin-bottom:6px;color:inherit">{baslik}</div>' if baslik else "")
    return (f'<div style="border-left:4px solid {kenar};background:{zemin};'
            f'border-radius:0 10px 10px 0;padding:14px 18px;margin:18px 0">{bas}'
            f'<div style="line-height:1.6;color:inherit">{govde}</div></div>')


def ic_html(t):
    """HTML bloğunun içinde markdown işlenmez; temel işaretleri etikete çevir."""
    t = re.sub(r"\*\*(.+?)\*\*", r"<strong>\1</strong>", t)
    t = re.sub(r"`([^`]+)`", r"<code>\1</code>", t)
    t = re.sub(r"(?<![*\w])\*([^*\n]+)\*(?![*\w])", r"<em>\1</em>", t)
    # Bağlantı da işlenmez; BAGLANTI: yer tutucusu href'te durur, kullanıcı
    # panelin verdiği adresle değiştirir.
    t = re.sub(r"\[([^\]]+)\]\(([^)]+)\)", r'<a href="\2">\1</a>', t)
    return t.replace("  \n", "<br>")


def slug(text):
    tr = {"ı": "i", "ğ": "g", "ü": "u", "ş": "s", "ö": "o", "ç": "c", "İ": "i"}
    t = "".join(tr.get(c, c) for c in text.lower())
    t = unicodedata.normalize("NFKD", t)
    t = "".join(c for c in t if not unicodedata.combining(c))
    return re.sub(r"[^a-z0-9]+", "-", t).strip("-")


SLUG_EN = {  # Türkçe slug → İngilizce dosya adı (dosya adları İngilizce tutulur)
    "ailenin-diger-urunuyle-iliskisi": "relation-to-wear",
    "sinirlar-ve-kaynak-kullanimi": "limits-and-resources",
    "donanim-ve-board-varyantlari": "hardware-and-board-variants",
    "corezzle-imager-ile-yukleme": "corezzle-imager-install",
    "telemetri-ve-konumlandirma": "telemetry-and-positioning",
    "telemetri-konum-ve-sunucu": "telemetry-location-and-server",
    "imager-ile-yukleme-senaryo": "imager-install-script",
    "windows-baslat-powershell": "windows-start-powershell",
    "terminal-hizli-baslangic": "terminal-quick-start",
    "terminal-cihaz-listesi": "terminal-device-list",
    "durum-ve-yol-haritasi": "status-and-roadmap",
    "vs-code-kurulumu-genel": "vs-code-setup-general",
    "donanim-ve-baglantilar": "hardware-and-wiring",
    "baglanti-ve-gonderim": "connectivity-and-upload",
    "tampon-zaman-cizgisi": "buffer-timeline",
    "vscode-eklenti-onerisi": "vscode-extension-prompt",
    "test-ve-dogrulama": "testing-and-verification",
    "tarama-rapor-dongusu": "scan-report-cycle",
    "sebeke-yokken-tampon": "offline-buffer",
    "telemetri-boru-hatti": "telemetry-pipeline",
    "vscode-dil-degistir": "vscode-change-language",
    "cekirdek-ve-servisler": "core-and-services",
    "bu-cihaz-ne-yapiyor": "what-it-does",
    "zaman-damgasi-sorunu": "timestamp-problem",
    "kadran-analog-son": "watchface-analog-final",
    "kadran-dijital-son": "watchface-digital-final",
    "kadran-io-control": "watchface-io-control",
    "uygulama-gelistirme": "app-development",
    "kablosuz-guncelleme": "ota-updates",
    "mimari-katmanlar": "architecture-layers",
    "kullanici-arayuzu": "user-interface",
    "bildirim-detay": "notification-detail",
    "nasil-kurgulandi": "design-rationale",
    "tunel-olu-hesap": "tunnel-dead-reckoning",
    "imager-ile-yukleme": "imager-install",
    "vs-code-kurulumu": "vs-code-setup",
    "maliyet-korumasi": "cost-protection",
    "guvenlik-durumu": "security-status",
    "kadran-activity": "watchface-activity",
    "terminal-derleme": "terminal-build",
    "terminal-kurulum": "terminal-setup",
    "veri-durustlugu": "data-integrity",
    "vscode-kurulumu": "vscode-setup",
    "acilis-selftest": "boot-selftest",
    "arac-verisi-can": "vehicle-data-can",
    "bssid-ile-konum": "bssid-positioning",
    "temel-kavramlar": "basic-concepts",
    "uygulama-acilis": "app-launch",
    "hizli-baslangic": "quick-start",
    "sunucu-kurulumu": "server-setup",
    "rapor-anatomisi": "report-anatomy",
    "konsol-teshis": "console-diagnostics",
    "kadran-dijital": "watchface-digital",
    "admin-telemetri": "admin-telemetry",
    "diyagram-kaynak": "diagram-src",
    "admin-cihazlar": "admin-devices",
    "admin-surumler": "admin-releases",
    "rapor-icerigi": "report-contents",
    "kadran-analog": "watchface-analog",
    "kadran-secici": "watchface-selector",
    "komut-arayuzu": "command-interface",
    "tarama-dongusu": "scan-cycle",
    "vscode-indirme": "vscode-download",
    "konsol-kurulum": "console-setup",
    "wifi-konumlama": "wifi-positioning",
    "zaman-damgasi": "timestamp",
    "ota-guncelleme": "ota-updates",
    "sorun-giderme": "troubleshooting",
    "araca-montaj": "vehicle-installation",
    "arac-baglanti": "vehicle-wiring",
    "sunucu-tarafi": "server-side",
    "vscode-pencere": "vscode-window",
    "hero-cihazlar": "hero-devices",
    "ota-geri-alma": "ota-rollback",
    "led-desenleri": "led-patterns",
    "guc-yonetimi": "power-management",
    "lcd128-kaynak": "lcd128-src",
    "cift-cekirdek": "dual-core",
    "durum-led-i": "status-led",
    "panele-cevir": "to-panel",
    "panel-icerik": "panel-content",
    "guc-raylari": "power-rails",
    "hava-durumu": "weather",
    "guc-dongusu": "power-cycle",
    "store-detay": "store-detail",
    "bildirimler": "notifications",
    "genel-bakis": "overview",
    "boru-hatti": "pipeline",
    "lcd128-tur": "lcd128-tour",
    "veri-akisi": "data-flow",
    "adim-sayar": "step-counter",
    "gif-kaynak": "gif-src",
    "baslarken": "getting-started",
    "gorseller": "images",
    "gif-saat": "gif-watch",
    "saat-240": "watch-240",
    "telemetri": "telemetry",
    "paketle": "package",
    "ayarlar": "settings",
    "donanim": "hardware",
    "mimari": "architecture",
    "galeri": "gallery",
    "sozluk": "glossary",
    "dongu": "cycle",
    "konum": "positioning",
    "saat": "watch",
}


def slug_en(text):
    s = slug(text)
    for a, b in sorted(SLUG_EN.items(), key=lambda kv: -len(kv[0])):
        s = re.sub(r"(?<![a-z])" + re.escape(a) + r"(?![a-z])", b, s)
    return s


def to_md(fragment):
    p = ToMarkdown()
    p.feed(fragment)
    p.close()
    return p.result(), p.images


def temizle_hero(frag, lede_kalsin=False):
    """Sayfa başlığı panelde ayrı alanda duruyor; gövdede tekrar etmesin.

    lede (başlık altı giriş paragrafı) başlık değil, içeriktir — alt sayfalarda
    korunur. Ana sayfada hero metniyle aynı olduğu için atılır."""
    frag = re.sub(r'<div class="eyebrow">.*?</div>', "", frag, flags=re.S)
    frag = re.sub(r"<h1>.*?</h1>", "", frag, count=1, flags=re.S)
    if not lede_kalsin:
        frag = re.sub(r'<p class="lede">.*?</p>', "", frag, count=1, flags=re.S)
    return frag


def sonraki_adim(frag):
    """<a class="next" href="X"> → hedefi data-href'te taşıyan div."""
    def cevir(m):
        hedef = ALT_SAYFALAR.get(re.sub(r"\.html.*$", "", m.group(1)))
        if not hedef:
            return ""   # adresi olmayan hedefe kart basmanın anlamı yok
        return f'<div class="nextcard" data-href="{hedef}">{m.group(2)}</div>'
    return re.sub(r'<a class="next" href="([^"]+)"[^>]*>(.*?)</a>', cevir, frag, flags=re.S)


def kart_baglantilari(frag):
    """Köprü bölümdeki kartlar <a class="card" href="...html"> — panelde o
    adresler yok, gezinme ağaçtan yapılıyor. <div>'e çevirmezsek kart dalına
    hiç girmez, içerik düz metin olarak dökülür."""
    def cevir(m):
        hedef = ALT_SAYFALAR.get(re.sub(r"\.html.*$", "", m.group(1)))
        nitelik = f' data-href="{hedef}"' if hedef else ""
        return f'<div class="card"{nitelik}>{m.group(2)}</div>'
    return re.sub(r'<a class="card" href="([^"]+)"[^>]*>(.*?)</a>', cevir, frag, flags=re.S)


def main_html(path):
    s = (HERE / path).read_text(encoding="utf-8")
    m = re.search(r"<main>(.*?)</main>", s, re.S)
    return m.group(1) if m else s


def main():
    OUT.mkdir(exist_ok=True)
    for old in OUT.glob("*.md"):
        old.unlink()

    bolumler = []   # (sıra, başlık, markdown, görseller, üst_bölüm)

    # ── index.html: h2 sınırlarından böl ───────────────────────────
    idx = main_html("index.html")
    idx = temizle_hero(idx)
    parts = re.split(r'<h2 id="([^"]+)">(.*?)</h2>', idx)
    # parts[0] = giriş (hero sonrası kartlar/sayılar) — "Genel Bakış" olur
    giris_md, giris_img = to_md(parts[0])
    if giris_md:
        bolumler.append(("Genel Bakış", giris_md, giris_img, None))

    for i in range(1, len(parts), 3):
        baslik = re.sub(r"<[^>]+>", "", parts[i + 1]).strip()
        govde = parts[i + 2]
        # Köprü bölüm de girilir: sitede Komut Arayüzü'nden sonra kısa bir
        # tanıtım + iki kart var, ayrıntı alt sayfalarda. Panelde de aynı
        # yapı kurulsun diye atlamıyoruz; iki alt sayfa bunun çocuğu olur.
        if parts[i] == "derinlemesine":
            govde = kart_baglantilari(govde)
        md, imgs = to_md(govde)
        bolumler.append((baslik, md, imgs, None))

    # ── alt sayfalar ───────────────────────────────────────────────
    for dosya, baslik, ust in [
        ("telemetry-and-positioning.html", "Telemetri ve Konumlandırma",
         "Telemetri, Konum ve Sunucu"),
        ("server-setup.html", "Sunucu Kurulumu", "Telemetri, Konum ve Sunucu"),
        ("vscode-setup.html", "VS Code Kurulumu", "Hızlı Başlangıç"),
        ("vs-code.html", "VS Code Kurulumu (genel)", None),
        ("imager-install.html", "Corezzle Imager ile Yükleme", "Hızlı Başlangıç"),
    ]:
        frag = main_html(dosya)
        frag = temizle_hero(frag, lede_kalsin=True)
        frag = sonraki_adim(frag)
        md, imgs = to_md(frag)
        bolumler.append((baslik, md, imgs, ust))

    # ── yaz ────────────────────────────────────────────────────────
    listeler = ["# Panel içeriği — bölüm ve görsel listesi", "",
                "Her dosya panelde bir **başlık** olur. `GORSEL:` işaretli yerlere",
                "editörün *Resim Yükle* butonuyla ilgili dosyayı yükleyip URL'i oraya koy.", ""]
    for n, (baslik, md, imgs, ust) in enumerate(bolumler, start=1):
        fname = f"{n:02d}-{slug_en(baslik)}.md"
        (OUT / fname).write_text(md + "\n", encoding="utf-8")
        ustbilgi = f"  ↳ üst bölüm: **{ust}**" if ust else ""
        listeler.append(f"### {n:02d}. {baslik}{ustbilgi}")
        listeler.append(f"Dosya: `panel-content/{fname}` · {len(md)} karakter · {len(imgs)} görsel")
        for src, alt in imgs:
            listeler.append(f"- `{src.split('/')[-1]}` — {alt or '(başlıksız)'}")
        listeler.append("")
        print(f"  ✓ {fname:38s} {len(md):>6} karakter, {len(imgs)} görsel")

    (OUT / "00-IMAGE-LIST.md").write_text("\n".join(listeler) + "\n", encoding="utf-8")
    print(f"\n✓ {len(bolumler)} bölüm → {OUT.relative_to(HERE.parent.parent)}")


if __name__ == "__main__":
    main()
