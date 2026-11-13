# -*- coding: utf-8 -*-
"""NarRTOS tanıtım videosu üreticisi — görseller + Türkçe seslendirme + montaj.

Kullanım:
    python3 -m venv .venv && .venv/bin/pip install pillow
    .venv/bin/python docs/narrtos_video_gen.py

Gereksinimler: macOS `say` (Yelda tr_TR sesi), ffmpeg.
Çıktı: docs/narrtos-tanitim.mp4  (+ ara dosyalar docs/video-assets/)
"""
import os, subprocess, json
from PIL import Image, ImageDraw, ImageFont, ImageFilter

HERE   = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.join(HERE, "video-assets")
BUILD  = os.path.join(ASSETS, "build")
OUT_MP4 = os.path.join(HERE, "narrtos-tanitim.mp4")
os.makedirs(BUILD, exist_ok=True)

W, H = 1920, 1080
BG, BG2 = (7, 30, 25), (10, 38, 32)
PANEL, PANEL2 = (13, 47, 40), (17, 58, 49)
LINEC  = (30, 80, 68)
INK    = (230, 244, 239)
MUTED  = (123, 169, 155)
GREEN  = (52, 211, 153)
GREEN2 = (74, 222, 128)
BLUE   = (96, 165, 250)
GRAY   = (140, 160, 152)
AMBER  = (251, 191, 36)

AR    = "/System/Library/Fonts/Supplemental/Arial.ttf"
ARB   = "/System/Library/Fonts/Supplemental/Arial Bold.ttf"
MONO  = "/System/Library/Fonts/Supplemental/Courier New.ttf"
MONOB = "/System/Library/Fonts/Supplemental/Courier New Bold.ttf"
F = lambda p, s: ImageFont.truetype(p, s)


def canvas():
    im = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(im)
    for i in range(H):
        t = i / H
        d.line([(0, i), (W, i)], fill=tuple(int(BG[k] + (BG2[k] - BG[k]) * t) for k in range(3)))
    return im


def rr(d, box, r, fill=None, outline=None, width=1):
    d.rounded_rectangle(box, radius=r, fill=fill, outline=outline, width=width)


def head(d, title, sub=None):
    d.text((96, 62), title, font=F(ARB, 62), fill=INK)
    if sub:
        d.text((96, 146), sub, font=F(AR, 34), fill=MUTED)


def shadow(im, box, radius=24, blur=16, alpha=150):
    sh = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    ImageDraw.Draw(sh).rounded_rectangle(
        (box[0] + 10, box[1] + 16, box[2] + 10, box[3] + 16), radius, fill=(0, 0, 0, alpha))
    return Image.alpha_composite(im.convert("RGBA"), sh.filter(ImageFilter.GaussianBlur(blur))).convert("RGB")


def code_block(d, box, lines, fs=30, lh=44, pad=34):
    rr(d, box, 20, fill=(6, 20, 17), outline=LINEC, width=3)
    y = box[1] + pad
    for parts in lines:
        x = box[0] + pad
        if isinstance(parts, str):
            parts = [(parts, MONO, INK)]
        for txt, fnt, col in parts:
            f = F(fnt, fs)
            d.text((x, y), txt, font=f, fill=col)
            x += d.textlength(txt, font=f)
        y += lh


# ═══════════════════════════ SAHNELER ═══════════════════════════
def s01_kapak():
    im = canvas(); d = ImageDraw.Draw(im)
    # arka plan devre motifi
    for i in range(9):
        y = 120 + i * 105
        d.line([(1150, y), (1830, y)], fill=(14, 52, 44), width=3)
        d.ellipse((1150 - 7, y - 7, 1150 + 7, y + 7), fill=(20, 70, 58))
    for i in range(6):
        x = 1230 + i * 115
        d.line([(x, 120), (x, 960)], fill=(14, 52, 44), width=3)
    d.ellipse((1480, 430, 1700, 650), outline=GREEN, width=5)
    d.ellipse((1530, 480, 1650, 600), fill=(15, 66, 52))

    d.text((110, 330), "NarRTOS", font=F(ARB, 150), fill=INK)
    d.text((118, 510), "Açık Kaynak Gömülü İşletim Sistemi", font=F(AR, 52), fill=GREEN)
    d.text((118, 590), "ESP32-S3  •  FreeRTOS  •  LVGL 9", font=F(AR, 38), fill=MUTED)
    rr(d, (118, 690, 470, 762), 36, fill=(15, 66, 52), outline=GREEN, width=3)
    d.text((294, 726), "sürüm 1.4.81", font=F(ARB, 32), fill=GREEN2, anchor="mm")
    im.save(f"{ASSETS}/v01.png")


def s02_giris():
    im = canvas(); d = ImageDraw.Draw(im)
    head(d, "NarRTOS nedir?", "Akıllı cihazlar için eksiksiz bir çalışma ortamı")
    cards = [
        ("ESP32-S3", "çift çekirdek, 240 MHz"),
        ("FreeRTOS", "gerçek zamanlı çekirdek"),
        ("LVGL 9", "dokunmatik grafik arayüz"),
        ("Açık kaynak", "tam dokümantasyon"),
    ]
    for i, (t, s) in enumerate(cards):
        x = 96 + i * 442
        box = (x, 300, x + 400, 560)
        rr(d, box, 26, fill=PANEL, outline=LINEC, width=3)
        d.ellipse((x + 40, 340, x + 84, 384), fill=GREEN)
        d.text((x + 40, 412), t, font=F(ARB, 40), fill=INK)
        d.text((x + 40, 470), s, font=F(AR, 28), fill=MUTED)
    # cihaz siluetleri
    d.rounded_rectangle((700, 640, 900, 890), 44, outline=GREEN, width=5)
    d.text((800, 765), "410×502", font=F(ARB, 30), fill=GREEN2, anchor="mm")
    d.ellipse((1010, 640, 1260, 890), outline=GREEN, width=5)
    d.text((1135, 765), "240×240", font=F(ARB, 30), fill=GREEN2, anchor="mm")
    d.text((W // 2, 950), "giyilebilir cihazlar  •  izleme cihazları  •  kontrol panelleri",
           font=F(AR, 34), fill=MUTED, anchor="mm")
    im.save(f"{ASSETS}/v02.png")


def s03_cift_cekirdek():
    im = canvas(); d = ImageDraw.Draw(im)
    head(d, "Çift Çekirdekli Görev Mimarisi", "ESP32-S3  •  240 MHz  •  FreeRTOS")
    c0, c1 = (96, 250, 936, 800), (984, 250, 1824, 800)
    for box, title, sub in [(c0, "CORE 0", "Kullanıcı Arayüzü"), (c1, "CORE 1", "Sensörler ve Servisler")]:
        rr(d, box, 28, fill=PANEL, outline=LINEC, width=3)
        d.text((box[0] + 40, box[1] + 34), title, font=F(ARB, 46), fill=GREEN)
        d.text((box[0] + 40, box[1] + 96), sub, font=F(AR, 32), fill=MUTED)

    def task(box, y, name, detail, prio):
        rr(d, (box[0] + 40, y, box[2] - 40, y + 116), 18, fill=PANEL2, outline=LINEC, width=2)
        d.text((box[0] + 70, y + 20), name, font=F(ARB, 34), fill=INK)
        d.text((box[0] + 70, y + 66), detail, font=F(AR, 26), fill=MUTED)
        pf = F(ARB, 24); pt = f"öncelik {prio}"
        pw = d.textlength(pt, font=pf)
        rr(d, (box[2] - 70 - pw - 36, y + 36, box[2] - 70, y + 80), 22, fill=(20, 70, 58))
        d.text((box[2] - 70 - pw - 18, y + 44), pt, font=pf, fill=GREEN2)

    task(c0, 410, "UI Görevi", "LVGL 9 render döngüsü — ekranın tek sahibi", 5)
    task(c0, 550, "Olay Dağıtımı", "jest ve buton yönlendirme", 5)
    task(c1, 396, "Sensör Görevi", "IMU örnekleme — adım ve jest olayları", 4)
    task(c1, 526, "Servis Görevleri", "WiFi • OTA • Telemetri • Komut", 3)
    task(c1, 656, "Güç Yönetimi", "batarya izleme — uyku/uyandırma", 6)

    bus = (96, 870, 1824, 980)
    rr(d, bus, 24, fill=(15, 66, 52), outline=GREEN, width=3)
    d.text((W // 2, 925), "OLAY VERİYOLU — yayınla / abone ol (görevler birbirini doğrudan çağırmaz)",
           font=F(ARB, 34), fill=INK, anchor="mm")
    for x in (516, 1404):
        d.line([(x, 800), (x, 870)], fill=GREEN, width=5)
        d.polygon([(x - 12, 862), (x + 12, 862), (x, 880)], fill=GREEN)
    im.save(f"{ASSETS}/v03.png")


def s04_uygulama_catisi():
    im = canvas(); d = ImageDraw.Draw(im)
    head(d, "Modüler Uygulama Çatısı", "Her uygulama bir yaşam döngüsü arayüzüyle kaydolur")
    code_block(d, (96, 270, 1000, 800), [
        [("typedef struct", MONOB, GREEN2), (" {", MONO, INK)],
        [("    void ", MONO, BLUE), ("(*on_start)", MONOB, INK), ("(lv_obj_t* screen);", MONO, INK)],
        [("    void ", MONO, BLUE), ("(*on_resume)", MONOB, INK), ("(void);", MONO, INK)],
        [("    void ", MONO, BLUE), ("(*on_pause)", MONOB, INK), ("(void);", MONO, INK)],
        [("    void ", MONO, BLUE), ("(*on_stop)", MONOB, INK), ("(void);", MONO, INK)],
        [("    bool ", MONO, BLUE), ("(*on_event)", MONOB, INK), ("(const os_event_t*);", MONO, INK)],
        [("    const char* name;", MONO, MUTED)],
        [("} ", MONO, INK), ("app_interface_t", MONOB, GREEN2), (";", MONO, INK)],
        [("", MONO, INK)],
        [("app_manager_register", MONOB, GREEN), ("(APP_ID_MYAPP, get_iface());", MONO, INK)],
        [("app_manager_launch", MONOB, GREEN), ("(APP_ID_MYAPP);", MONO, INK)],
    ], fs=29, lh=46)

    x0 = 1080
    d.text((x0, 300), "Yaşam döngüsü", font=F(ARB, 40), fill=GREEN)
    steps = [("on_start", "ekran verilir, arayüz kurulur"),
             ("on_pause", "başka uygulama devraldı"),
             ("on_resume", "arka plandan öne geldi"),
             ("on_stop", "kaynaklar bırakılır")]
    for i, (t, s) in enumerate(steps):
        y = 380 + i * 118
        d.ellipse((x0, y + 8, x0 + 40, y + 48), fill=GREEN if i == 0 else PANEL2, outline=GREEN, width=3)
        d.text((x0 + 70, y), t, font=F(MONOB, 36), fill=INK)
        d.text((x0 + 70, y + 50), s, font=F(AR, 27), fill=MUTED)
        if i < 3:
            d.line([(x0 + 20, y + 52), (x0 + 20, y + 118)], fill=LINEC, width=3)
    rr(d, (x0, 860, 1824, 970), 20, fill=(15, 66, 52), outline=GREEN, width=3)
    d.text((x0 + 40, 895), "Ekranın sahibi her an TEK uygulamadır", font=F(ARB, 34), fill=INK)
    im.save(f"{ASSETS}/v04.png")


def s05_olay_veriyolu():
    im = canvas(); d = ImageDraw.Draw(im)
    head(d, "Olay Tabanlı Servisler", "Yayınla / abone ol — servisler birbirini doğrudan çağırmaz")
    pubs = [("Sensör", "adım, jest"), ("Güç", "batarya, şarj"), ("Zaman", "saat güncel"), ("Dokunmatik", "girdi")]
    for i, (t, s) in enumerate(pubs):
        x = 96 + i * 442
        rr(d, (x, 280, x + 400, 420), 22, fill=PANEL, outline=LINEC, width=3)
        d.text((x + 32, 306), t, font=F(ARB, 36), fill=INK)
        d.text((x + 32, 356), s, font=F(AR, 26), fill=MUTED)
        d.line([(x + 200, 420), (x + 200, 500)], fill=GREEN, width=4)
        d.polygon([(x + 188, 492), (x + 212, 492), (x + 200, 512)], fill=GREEN)

    rr(d, (96, 520, 1824, 660), 26, fill=(15, 66, 52), outline=GREEN, width=4)
    d.text((W // 2, 566), "OLAY VERİYOLU", font=F(ARB, 44), fill=GREEN2, anchor="mm")
    d.text((W // 2, 622), "EVENT_STEP_COUNT · EVENT_BATTERY_LEVEL · EVENT_TIME_UPDATED · EVENT_APP_SWITCH",
           font=F(MONO, 26), fill=MUTED, anchor="mm")

    subs = [("Saat kadranı", "adımı gösterir"), ("Uygulamalar", "olayı tüketir"),
            ("Güç servisi", "ekranı uyandırır"), ("Telemetri", "rapora ekler")]
    for i, (t, s) in enumerate(subs):
        x = 96 + i * 442
        d.line([(x + 200, 660), (x + 200, 740)], fill=GREEN, width=4)
        d.polygon([(x + 188, 732), (x + 212, 732), (x + 200, 752)], fill=GREEN)
        rr(d, (x, 760, x + 400, 900), 22, fill=PANEL, outline=LINEC, width=3)
        d.text((x + 32, 786), t, font=F(ARB, 34), fill=INK)
        d.text((x + 32, 836), s, font=F(AR, 26), fill=MUTED)
    d.text((W // 2, 970), "Yeni özellik eklemek = veriyoluna bir abone daha", font=F(AR, 32), fill=GREEN, anchor="mm")
    im.save(f"{ASSETS}/v05.png")


def s06_board_varyant():
    im = canvas(); d = ImageDraw.Draw(im)
    head(d, "Board Varyant Sistemi", "Aynı kod tabanı, farklı donanım — uygulama kodu değişmez")
    rr(d, (660, 270, 1260, 390), 24, fill=(15, 66, 52), outline=GREEN, width=4)
    d.text((960, 330), "TEK KOD TABANI", font=F(ARB, 44), fill=GREEN2, anchor="mm")
    for x in (500, 1420):
        d.line([(960, 390), (960, 440)], fill=GREEN, width=5)
        d.line([(960, 440), (x, 440)], fill=GREEN, width=5)
        d.line([(x, 440), (x, 500)], fill=GREEN, width=5)
        d.polygon([(x - 12, 492), (x + 12, 492), (x, 512)], fill=GREEN)

    # AMOLED (dikdörtgen)
    rr(d, (330, 520, 670, 960), 28, fill=PANEL, outline=LINEC, width=3)
    d.rounded_rectangle((420, 570, 580, 830), 34, outline=GREEN, width=5)
    d.text((500, 700), "410×502", font=F(ARB, 28), fill=GREEN2, anchor="mm")
    d.text((500, 872), "AMOLED  •  QSPI", font=F(ARB, 32), fill=INK, anchor="mm")
    d.text((500, 916), "CO5300 + FT3168 + PMIC", font=F(AR, 25), fill=MUTED, anchor="mm")
    # LCD (yuvarlak)
    rr(d, (1250, 520, 1590, 960), 28, fill=PANEL, outline=LINEC, width=3)
    d.ellipse((1300, 590, 1540, 830), outline=GREEN, width=5)
    d.text((1420, 710), "240×240", font=F(ARB, 28), fill=GREEN2, anchor="mm")
    d.text((1420, 872), "LCD  •  SPI", font=F(ARB, 32), fill=INK, anchor="mm")
    d.text((1420, 916), "GC9A01A + CST816S", font=F(AR, 25), fill=MUTED, anchor="mm")

    code_block(d, (700, 560, 1220, 800), [
        [("# derleme zamanında seçilir", MONO, MUTED)],
        [("pio run -e ", MONO, INK), ("esp32s3_wearable", MONOB, GREEN2)],
        [("pio run -e ", MONO, INK), ("esp32s3_lcd128", MONOB, GREEN2)],
        [("", MONO, INK)],
        [("-DDISPLAY_VARIANT_LCD128", MONO, BLUE)],
        [("-DCO5300_H_RES=240", MONO, BLUE)],
    ], fs=26, lh=38, pad=28)
    im.save(f"{ASSETS}/v06.png")


def s07_uygulamalar():
    im = canvas(); d = ImageDraw.Draw(im)
    head(d, "Kutudan Çıkan Uygulamalar", "Temel bir akıllı cihaz deneyimi için gereken her şey")
    apps = [("Saat Kadranı", "saat, tarih, batarya, adım"), ("Uygulama Menüsü", "3×3 grid launcher"),
            ("Ayarlar", "WiFi, ekran, güncelleme"), ("Alarmlar", "kurma ekranı + ses"),
            ("Hava Durumu", "çoklu şehir, canlı veri"), ("Galeri", "cihaz depolamasındaki görseller"),
            ("Bildirimler", "iPhone / BLE köprüsü"), ("Adım Sayar", "IMU tabanlı"),
            ("Mağaza", "uygulama kataloğu")]
    for i, (t, s) in enumerate(apps):
        col, row = i % 3, i // 3
        x, y = 96 + col * 588, 290 + row * 236
        rr(d, (x, y, x + 546, y + 200), 24, fill=PANEL, outline=LINEC, width=3)
        d.ellipse((x + 34, y + 52, x + 130, y + 148), fill=(20, 70, 58), outline=GREEN, width=3)
        d.text((x + 82, y + 100), str(i + 1), font=F(ARB, 40), fill=GREEN2, anchor="mm")
        d.text((x + 166, y + 62), t, font=F(ARB, 36), fill=INK)
        d.text((x + 166, y + 116), s, font=F(AR, 26), fill=MUTED)
    im.save(f"{ASSETS}/v07.png")


def s08_gelistirici():
    im = canvas(); d = ImageDraw.Draw(im)
    head(d, "Geliştirici Dostu", "PlatformIO ile derle, yükle, çalıştır")
    code_block(d, (96, 280, 960, 700), [
        [("# derle ve cihaza yükle", MONO, MUTED)],
        [("$ ", MONOB, GREEN2), ("pio run -e esp32s3_wearable -t upload", MONOB, INK)],
        [("", MONO, INK)],
        [("# seri monitör", MONO, MUTED)],
        [("$ ", MONOB, GREEN2), ("pio device monitor", MONOB, INK)],
        [("", MONO, INK)],
        [("Linking .pio/build/.../firmware.elf", MONO, MUTED)],
        [("SUCCESS", MONOB, GREEN), ("   Took 12.69 seconds", MONO, MUTED)],
    ], fs=28, lh=44)
    x0 = 1040
    d.text((x0, 300), "4 adımda yeni uygulama", font=F(ARB, 42), fill=GREEN)
    steps = ["Uygulama dosyalarını yaz", "Kimlik (APP_ID) tanımla",
             "Merkezî kayda ekle", "Menüye kutucuk koy (ops.)"]
    for i, s in enumerate(steps):
        y = 400 + i * 108
        d.ellipse((x0, y, x0 + 56, y + 56), fill=(20, 70, 58), outline=GREEN, width=3)
        d.text((x0 + 28, y + 28), str(i + 1), font=F(ARB, 32), fill=GREEN2, anchor="mm")
        d.text((x0 + 86, y + 10), s, font=F(AR, 34), fill=INK)
    rr(d, (96, 760, 1824, 940), 22, fill=PANEL, outline=LINEC, width=3)
    d.text((136, 796), "Yeni donanıma taşıma", font=F(ARB, 36), fill=GREEN)
    d.text((136, 856), "Sürücü dosyası + pin haritası + build ortamı  →  uygulama koduna hiç dokunmadan",
           font=F(AR, 32), fill=INK)
    im.save(f"{ASSETS}/v08.png")


def s09_ota():
    im = canvas(); d = ImageDraw.Draw(im)
    head(d, "Kablosuz Güncelleme (OTA)", "Cihaz kendini alandan günceller")
    stages = [("Manifest", "sürüm sorgusu"), ("İndirme", "WiFi üzerinden imaj"),
              ("Doğrulama", "bütünlük kontrolü"), ("Yeniden başlat", "yeni sürüm aktif")]
    for i, (t, s) in enumerate(stages):
        x = 96 + i * 442
        rr(d, (x, 290, x + 400, 470), 24, fill=PANEL, outline=LINEC, width=3)
        d.ellipse((x + 32, 322, x + 84, 374), fill=GREEN)
        d.text((x + 58, 348), str(i + 1), font=F(ARB, 30), fill=(7, 30, 25), anchor="mm")
        d.text((x + 104, 322), t, font=F(ARB, 36), fill=INK)
        d.text((x + 32, 400), s, font=F(AR, 27), fill=MUTED)
        if i < 3:
            d.polygon([(x + 410, 370), (x + 410, 394), (x + 436, 382)], fill=GREEN)

    code_block(d, (96, 530, 1000, 830), [
        [("{", MONO, INK)],
        [('  "version"', MONO, BLUE), (':      "1.5.0",', MONO, INK)],
        [('  "version_code"', MONO, BLUE), (': 10500,', MONO, INK)],
        [('  "url"', MONO, BLUE), (':          "https://.../fw-1.5.0.bin",', MONO, INK)],
        [('  "size"', MONO, BLUE), (":         2480000", MONO, INK)],
        [("}", MONO, INK)],
    ], fs=27, lh=42)

    x0 = 1060
    d.text((x0, 540), "A/B partisyon düzeni", font=F(ARB, 38), fill=GREEN)
    for i, (n, c, note) in enumerate([("app0", GREEN, "çalışan sürüm"), ("app1", AMBER, "yeni imaj buraya")]):
        y = 620 + i * 110
        rr(d, (x0, y, 1824, y + 88), 18, fill=PANEL2, outline=c, width=3)
        d.text((x0 + 30, y + 24), n, font=F(MONOB, 34), fill=c)
        d.text((x0 + 160, y + 28), note, font=F(AR, 28), fill=MUTED)
    d.text((x0, 850), "Güncelleme başarısız olursa cihaz eski sürüme döner.",
           font=F(AR, 29), fill=INK)
    im.save(f"{ASSETS}/v09.png")


def s10_telemetri():
    im = canvas(); d = ImageDraw.Draw(im)
    head(d, "Telemetri ve Konumlandırma", "Cihaz sağlığı + WiFi tabanlı konum (GPS'siz)")
    box = (96, 250, 1824, 1000)
    rr(d, box, 24, fill=PANEL, outline=LINEC, width=3)
    cols = [(140, "DEVICE ID"), (470, "ZAMAN"), (760, "WiFi RSSI"), (1000, "KONUM"), (1470, "DURUM"), (1650, "BOYUT")]
    d.rectangle((box[0] + 2, 280, box[2] - 2, 350), fill=(10, 34, 29))
    for x, t in cols:
        d.text((x, 315), t, font=F(ARB, 28), fill=MUTED, anchor="lm")
    rows = [("12:41:03", "-52 dBm", "41.01034, 28.96821", 19, True),
            ("12:40:03", "-53 dBm", "41.01034, 28.96821", 19, True),
            ("12:39:02", "-51 dBm", "41.01035, 28.96818", 21, True),
            ("12:38:02", "-55 dBm", "41.01034, 28.96821", 19, True),
            ("12:37:01", "-71 dBm", "40.18850, 29.06100 (IP)", None, False)]
    y, RH = 350, 108
    for i, (ts, rssi, loc, acc, real) in enumerate(rows):
        if i % 2 == 1:
            d.rectangle((box[0] + 2, y, box[2] - 2, y + RH), fill=(15, 52, 44))
        cy = y + RH // 2
        d.text((140, cy), "E4B063A764CC", font=F(MONO, 30), fill=INK, anchor="lm")
        d.text((470, cy), ts, font=F(AR, 30), fill=INK, anchor="lm")
        d.text((760, cy), rssi, font=F(MONO, 30), fill=INK, anchor="lm")
        if real:
            d.text((1000, cy - 16), loc, font=F(MONOB, 30), fill=BLUE, anchor="lm")
            lw = d.textlength(loc, font=F(MONOB, 30))
            d.line([(1000, cy + 4), (1000 + lw, cy + 4)], fill=BLUE, width=2)
            d.text((1000, cy + 28), f"±{acc} m — WiFi çözümü", font=F(AR, 24), fill=MUTED, anchor="lm")
        else:
            d.text((1000, cy), loc, font=F(MONO, 27), fill=GRAY, anchor="lm")
        bt = "sent" if real else "dry-run"
        bc, tc = ((17, 74, 52), GREEN) if real else ((46, 58, 54), GRAY)
        bw = d.textlength(bt, font=F(ARB, 26)) + 44
        rr(d, (1470, cy - 26, 1470 + bw, cy + 26), 26, fill=bc)
        d.text((1470 + bw / 2, cy), bt, font=F(ARB, 26), fill=tc, anchor="mm")
        d.text((1650, cy), "540 B" if real else "85 B", font=F(MONO, 30), fill=INK, anchor="lm")
        y += RH
    d.text((box[0] + 44, 950), "● canlı — 60 saniyede bir rapor", font=F(AR, 27), fill=GREEN, anchor="lm")
    im.save(f"{ASSETS}/v10.png")


def s11_komut():
    im = canvas(); d = ImageDraw.Draw(im)
    head(d, "Komut Arayüzü", "USB seri veya telefondan (BLE) — aynı komutlar")
    tbox = (200, 240, 1720, 1010)
    im = shadow(im, tbox, 24, 14, 140); d = ImageDraw.Draw(im)
    rr(d, tbox, 24, fill=(6, 20, 17), outline=LINEC, width=3)
    rr(d, (tbox[0], tbox[1], tbox[2], tbox[1] + 64), 24, fill=(12, 40, 34))
    d.rectangle((tbox[0], tbox[1] + 32, tbox[2], tbox[1] + 64), fill=(12, 40, 34))
    for i, c in enumerate([(255, 96, 92), (255, 189, 68), (0, 202, 78)]):
        d.ellipse((tbox[0] + 28 + i * 40, tbox[1] + 20, tbox[0] + 52 + i * 40, tbox[1] + 44), fill=c)
    d.text(((tbox[0] + tbox[2]) // 2, tbox[1] + 32), "NarRTOS — komut oturumu", font=F(AR, 26), fill=MUTED, anchor="mm")
    mf, mfb = F(MONO, 30), F(MONOB, 30)
    x0, y, LH = tbox[0] + 44, tbox[1] + 105, 45
    def line(parts, dy=LH):
        nonlocal y
        x = x0
        for txt, fnt, col in parts:
            d.text((x, y), txt, font=fnt, fill=col); x += d.textlength(txt, font=fnt)
        y += dy
    line([("> ", mfb, GREEN2), ("help", mfb, INK)])
    for t in ["[cmd]   app launch <name|id> | app back | app current | app list",
              "[cmd]   swipe left|right|up|down   |   brightness <0-255>",
              "[cmd]   set wifi|bt on|off  |  get wifi|bt  |  get rails"]:
        line([(t, mf, MUTED)])
    y += 14
    for cmd, res in [("app launch weather", "[cmd] OK: launch weather (id=6)"),
                     ("brightness 30", "[cmd] OK: brightness=30"),
                     ("get wifi", "[cmd] OK: wifi connected=yes pref=on"),
                     ("swipe right", "[cmd] OK: swipe right (dir=-1)")]:
        line([("> ", mfb, GREEN2), (cmd, mfb, INK)])
        line([(res, mfb, GREEN)], dy=LH + 12)
    d.text((x0, y), "> ", font=mfb, fill=GREEN2)
    d.rectangle((x0 + d.textlength("> ", font=mfb), y + 6, x0 + d.textlength("> ", font=mfb) + 18, y + 38), fill=GREEN2)
    im.save(f"{ASSETS}/v11.png")


def s12_acik_kaynak():
    im = canvas()
    raw = Image.open(f"{ASSETS}/cover_raw.png").convert("RGBA")
    cover = Image.new("RGB", raw.size, (255, 255, 255)); cover.paste(raw, (0, 0), raw)
    ch = 880; cw = int(cover.width * ch / cover.height)
    cover = cover.resize((cw, ch), Image.LANCZOS)
    cx, cy = 250, (H - ch) // 2
    im = shadow(im, (cx, cy, cx + cw, cy + ch), 8, 18, 160)
    im.paste(cover, (cx, cy))
    d = ImageDraw.Draw(im)
    d.rectangle((cx, cy, cx + cw, cy + ch), outline=LINEC, width=3)
    tx = cx + cw + 120
    d.text((tx, 300), "Açık Kaynak", font=F(ARB, 66), fill=GREEN)
    d.text((tx, 396), "Tüm sistem belgelenmiş durumda:", font=F(AR, 36), fill=INK)
    for i, t in enumerate(["Mimari ve boot akışı", "Sürücüler ve servisler",
                           "Uygulama ekleme rehberi", "Board taşıma rehberi",
                           "Komut arayüzü (USB + BLE)"]):
        yy = 480 + i * 64
        d.ellipse((tx, yy + 12, tx + 18, yy + 30), fill=GREEN)
        d.text((tx + 40, yy), t, font=F(AR, 34), fill=INK)
    d.text((tx, 840), "Dokümantasyonu inceleyin —", font=F(ARB, 38), fill=INK)
    d.text((tx, 894), "NarRTOS ile bugün geliştirmeye başlayın.", font=F(ARB, 38), fill=GREEN2)
    im.save(f"{ASSETS}/v12.png")


def s13_kapanis():
    im = canvas(); d = ImageDraw.Draw(im)
    d.ellipse((1420, 240, 1800, 620), outline=(20, 70, 58), width=4)
    d.ellipse((1490, 310, 1730, 550), outline=(20, 70, 58), width=4)
    d.text((110, 330), "NarRTOS", font=F(ARB, 130), fill=INK)
    d.text((118, 500), "Açık kaynak gömülü işletim sistemi", font=F(AR, 46), fill=GREEN)
    rr(d, (118, 610, 1180, 740), 24, fill=PANEL, outline=GREEN, width=3)
    d.text((150, 648), "github.com/corezzle/NarRTOS-Wear", font=F(MONOB, 38), fill=GREEN2)
    d.text((118, 810), "Kendi akıllı cihazınızı geliştirmeye bugün başlayın.", font=F(ARB, 44), fill=INK)
    im.save(f"{ASSETS}/v13.png")


# ═══════════════════════════ SESLENDİRME ═══════════════════════════
NARRATION = [
 ("v01", "NarRTOS. Açık kaynak gömülü işletim sistemi. FreeRTOS tabanlı bu platformun; "
         "çift çekirdekli performansı, zengin grafikleri ve modüler servisleri bir araya getirerek "
         "akıllı cihaz geliştirmeyi nasıl hızlandırdığını keşfedin."),
 ("v02", "NarRTOS; akıllı saatler ve takip cihazları dahil, ESP32-S3 tabanlı akıllı cihazlar için "
         "tasarlanmış açık kaynak bir gömülü işletim sistemidir. FreeRTOS üzerine kuruludur ve "
         "dokunmatik ekranlarda gelişmiş grafik arayüzler için LVGL 9 kütüphanesini kullanır. "
         "Bu videoda temel özelliklerini, kutudan çıkan yeteneklerini ve geliştiricilere neler "
         "sunduğunu ele alacağız."),
 ("v03", "NarRTOS çift çekirdekli bir görev mimarisi kullanır. Kullanıcı arayüzü çizimi ile sensör "
         "ve servis görevleri iki çekirdek arasında ayrı ayrı yönetilir; böylece çakışmalar önlenir "
         "ve akıcı bir performans sağlanır. Bu mimari, hızlı ve verimli bir akıllı cihaz deneyiminin "
         "temelidir."),
 ("v04", "NarRTOS'un kalbinde modüler bir uygulama çatısı bulunur. Uygulamalar bir yaşam döngüsü "
         "arayüzüyle sisteme kaydolur. Ekranın sahibi her an tek bir uygulamadır; bu da temiz ve "
         "düzenli bir arayüz yönetimi sağlar."),
 ("v05", "Sistem içindeki iletişim, olay tabanlı bir modelle sağlanır. Hafif bir yayınla ve abone ol "
         "veriyolu, görevlerin birbirini doğrudan çağırmadan haberleşmesine imkân verir. Bu yapı, "
         "yeni özellikler eklemeyi ve mevcutları değiştirmeyi kolaylaştırır."),
 ("v06", "NarRTOS bir board varyant sistemi destekler. Aynı kod tabanı, dört yüz ona beş yüz iki "
         "piksellik AMOLED ekranda da, iki yüz kırka iki yüz kırk piksellik yuvarlak LCD ekranda da "
         "çalışabilir. Ekran ve dokunmatik donanımı derleme zamanında seçilir; uygulama koduna hiç "
         "dokunulmaz."),
 ("v07", "NarRTOS kutudan çıkar çıkmaz temel uygulamalarla gelir: saat kadranı, uygulama menüsü, "
         "ayarlar, alarmlar, hava durumu, galeri, telefon bildirimleri, adım sayar ve uygulama mağazası. "
         "Bu hazır uygulamalar, cihaz kullanıma girer girmez temel işlevselliğin hazır olmasını sağlar."),
 ("v08", "NarRTOS geliştirme kolaylığı için tasarlandı. PlatformIO ile projeler hızla derlenip cihaza "
         "yüklenir. Yeni bir uygulama eklemek dört adıma indirgenmiştir. Yeni donanımlara taşımak ise "
         "uygulama koduna hiç dokunmadan, yalnızca sürücü ve yapılandırma eklenerek yapılır."),
 ("v09", "NarRTOS, cihazların yeni yazılımı kablosuz olarak indirdiği kablosuz güncellemeyi destekler. "
         "Cihaz sürüm bilgisini sorgular, yeni imajı indirir ve yedek bölüme yazar. Bu sayede cihazlar "
         "fiziksel müdahale olmadan, uzaktan güncel tutulabilir."),
 ("v10", "NarRTOS'un öne çıkan özelliklerinden biri, WiFi tabanlı konumlandırma içeren telemetri "
         "altyapısıdır. Cihazlar sağlık verilerini ve çevredeki WiFi ağlarına dayanan konumlarını, "
         "GPS donanımı olmadan raporlayabilir. Konum yaklaşık yirmi metre hassasiyetle çözülür. "
         "Bu özellik, takip uygulamaları ve varlık izleme için idealdir."),
 ("v11", "Komut arayüzü, cihazı USB seri bağlantı üzerinden ya da telefondan Bluetooth ile uzaktan "
         "sürmeyi sağlar. Uygulama açabilir, ekran parlaklığını ayarlayabilir, WiFi'yi açıp kapatabilir "
         "ve cihaz durumunu okuyabilirsiniz. Test, demo ve otomasyon senaryoları için güçlü bir araçtır."),
 ("v12", "Açık kaynak bir platform olarak NarRTOS; mimarisinden sürücülerine, uygulama geliştirmeden "
         "board taşımaya kadar tüm sistemi belgelenmiş halde sunar. Geliştiricileri dokümantasyonu "
         "keşfetmeye ve sistemi kendi ihtiyaçlarına göre denemeye davet ediyoruz."),
 ("v13", "NarRTOS ile kendi akıllı cihazınızı geliştirmeye bugün başlayın."),
]


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, capture_output=True, text=True, **kw)


def build_audio():
    for tag, text in NARRATION:
        txt = os.path.join(BUILD, f"{tag}.txt")
        aiff = os.path.join(BUILD, f"{tag}.aiff")
        with open(txt, "w") as f:
            f.write(text)
        run(["say", "-v", "Yelda", "-r", "168", "-f", txt, "-o", aiff])
    print("  ses dosyaları hazır")


def duration(path):
    out = run(["ffprobe", "-v", "quiet", "-print_format", "json", "-show_format", path]).stdout
    return float(json.loads(out)["format"]["duration"])


def build_video():
    parts = []
    for tag, _ in NARRATION:
        png  = os.path.join(ASSETS, f"{tag}.png")
        aiff = os.path.join(BUILD, f"{tag}.aiff")
        mp4  = os.path.join(BUILD, f"{tag}.mp4")
        dur  = duration(aiff) + 1.0          # anlatım bitince 1 sn nefes payı
        fade = f"fade=t=in:st=0:d=0.5,fade=t=out:st={dur-0.5:.2f}:d=0.5"
        run(["ffmpeg", "-y", "-loop", "1", "-i", png, "-i", aiff,
             "-t", f"{dur:.2f}", "-vf", f"{fade},format=yuv420p", "-r", "30",
             "-c:v", "libx264", "-preset", "medium", "-crf", "20", "-tune", "stillimage",
             "-af", "apad", "-c:a", "aac", "-b:a", "192k", "-ar", "48000", "-ac", "2",
             "-shortest", mp4])
        parts.append(mp4)
        print(f"  {tag}: {dur:.1f} sn")

    lst = os.path.join(BUILD, "parts.txt")
    with open(lst, "w") as f:
        for p in parts:
            f.write(f"file '{p}'\n")
    run(["ffmpeg", "-y", "-f", "concat", "-safe", "0", "-i", lst, "-c", "copy", OUT_MP4])
    return duration(OUT_MP4)


if __name__ == "__main__":
    print("1/3  Sahne görselleri üretiliyor...")
    for fn in (s01_kapak, s02_giris, s03_cift_cekirdek, s04_uygulama_catisi, s05_olay_veriyolu,
               s06_board_varyant, s07_uygulamalar, s08_gelistirici, s09_ota, s10_telemetri,
               s11_komut, s12_acik_kaynak, s13_kapanis):
        fn()
    print("2/3  Türkçe seslendirme (Yelda) üretiliyor...")
    build_audio()
    print("3/3  Video montajlanıyor...")
    total = build_video()
    print(f"\nTAMAM → {OUT_MP4}  ({int(total//60)}:{int(total%60):02d})")
