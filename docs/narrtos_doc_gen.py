# -*- coding: utf-8 -*-
"""NarRTOS — Açık Kaynak Gömülü İşletim Sistemi: Temel Dokümantasyon üretici.

Kullanım:
    python3 -m venv .docenv && .docenv/bin/pip install reportlab
    .docenv/bin/python docs/narrtos_doc_gen.py
Çıktı: docs/narrtos-dokumantasyonu.pdf

Not: Font yolları macOS'a göredir (Arial / Courier New — Türkçe glif desteği).
"""
import os
from reportlab.lib.pagesizes import A4
from reportlab.lib.units import mm
from reportlab.lib.colors import HexColor
from reportlab.lib.styles import ParagraphStyle
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
from reportlab.platypus import (
    SimpleDocTemplate, Paragraph, Spacer, PageBreak, Table, TableStyle, HRFlowable,
)

pdfmetrics.registerFont(TTFont("Ar", "/System/Library/Fonts/Supplemental/Arial.ttf"))
pdfmetrics.registerFont(TTFont("ArB", "/System/Library/Fonts/Supplemental/Arial Bold.ttf"))
pdfmetrics.registerFont(TTFont("Mono", "/System/Library/Fonts/Supplemental/Courier New.ttf"))

INK    = HexColor("#1a1d29")
ACCENT = HexColor("#0f766e")
MUTED  = HexColor("#6b7280")
CODEBG = HexColor("#f4f4f5")
LINEC  = HexColor("#e4e4e7")

S = {}
S["title"]    = ParagraphStyle("title", fontName="ArB", fontSize=32, leading=38, textColor=INK)
S["subtitle"] = ParagraphStyle("subtitle", fontName="Ar", fontSize=13, leading=19, textColor=MUTED)
S["h1"]  = ParagraphStyle("h1", fontName="ArB", fontSize=17, leading=22, textColor=INK,
                          spaceBefore=18, spaceAfter=8)
S["h2"]  = ParagraphStyle("h2", fontName="ArB", fontSize=12.5, leading=17, textColor=ACCENT,
                          spaceBefore=12, spaceAfter=5)
S["body"] = ParagraphStyle("body", fontName="Ar", fontSize=10, leading=15.5, textColor=INK,
                           spaceAfter=6)
S["bull"] = ParagraphStyle("bull", parent=S["body"], leftIndent=14, bulletIndent=4)
S["code"] = ParagraphStyle("code", fontName="Mono", fontSize=8.2, leading=11.6,
                           textColor=INK, backColor=CODEBG, borderPadding=6,
                           leftIndent=4, rightIndent=4, spaceBefore=4, spaceAfter=8)
S["cap"]  = ParagraphStyle("cap", fontName="Ar", fontSize=8.5, leading=12, textColor=MUTED,
                           spaceAfter=8)
S["toc"]  = ParagraphStyle("toc", fontName="Ar", fontSize=10.5, leading=19, textColor=INK)

def H1(t): return Paragraph(t, S["h1"])
def H2(t): return Paragraph(t, S["h2"])
def P(t):  return Paragraph(t, S["body"])
def B(t):  return Paragraph(t, S["bull"], bulletText="•")
def C(t):  return Paragraph(t.replace("\n", "<br/>").replace(" ", "&nbsp;"), S["code"])
def CAP(t): return Paragraph(t, S["cap"])
def RULE(): return HRFlowable(width="100%", thickness=0.7, color=LINEC, spaceBefore=4, spaceAfter=10)

def TBL(head, rows, widths):
    data = [[Paragraph(f"<b>{h}</b>", ParagraphStyle("th", fontName="ArB", fontSize=8.8,
             leading=12, textColor=HexColor("#ffffff"))) for h in head]]
    cell = ParagraphStyle("td", fontName="Ar", fontSize=8.8, leading=12.5, textColor=INK)
    for r in rows:
        data.append([Paragraph(str(c), cell) for c in r])
    t = Table(data, colWidths=widths, repeatRows=1)
    t.setStyle(TableStyle([
        ("BACKGROUND", (0, 0), (-1, 0), INK),
        ("ROWBACKGROUNDS", (0, 1), (-1, -1), [HexColor("#ffffff"), HexColor("#fafafa")]),
        ("GRID", (0, 0), (-1, -1), 0.4, LINEC),
        ("VALIGN", (0, 0), (-1, -1), "TOP"),
        ("LEFTPADDING", (0, 0), (-1, -1), 6),
        ("RIGHTPADDING", (0, 0), (-1, -1), 6),
        ("TOPPADDING", (0, 0), (-1, -1), 4.5),
        ("BOTTOMPADDING", (0, 0), (-1, -1), 4.5),
    ]))
    return t

def footer(canv, doc):
    canv.saveState()
    canv.setFont("Ar", 8)
    canv.setFillColor(MUTED)
    canv.drawString(20 * mm, 12 * mm, "NarRTOS — Temel Dokümantasyon — v1.3 (29 Temmuz 2026)")
    canv.drawRightString(190 * mm, 12 * mm, f"Sayfa {doc.page}")
    canv.restoreState()

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "narrtos-dokumantasyonu.pdf")
doc = SimpleDocTemplate(OUT, pagesize=A4,
                        leftMargin=20 * mm, rightMargin=20 * mm,
                        topMargin=18 * mm, bottomMargin=20 * mm,
                        title="NarRTOS — Açık Kaynak Gömülü İşletim Sistemi Dokümantasyonu",
                        author="NarRTOS Project")
E = []

# ═══ KAPAK ═══
E.append(Spacer(1, 55 * mm))
E.append(Paragraph("NarRTOS", S["title"]))
E.append(Spacer(1, 4))
E.append(Paragraph("Açık Kaynak Gömülü İşletim Sistemi<br/>Temel Dokümantasyon", S["subtitle"]))
E.append(Spacer(1, 14))
E.append(HRFlowable(width="40%", thickness=2, color=ACCENT, hAlign="LEFT"))
E.append(Spacer(1, 14))
E.append(Paragraph("Sürüm 1.3 — 29 Temmuz 2026", S["subtitle"]))
E.append(Paragraph("Hedef platform: ESP32-S3 — FreeRTOS + LVGL 9", S["subtitle"]))
E.append(Paragraph("Bu doküman işletim sisteminin çekirdeğini, kullanıcı etkileşim modelini ve<br/>"
                   "yerleşik uygulamaları anlatır: sürücüler, servisler, derleme ve çalıştırma.",
                   S["subtitle"]))
E.append(PageBreak())

# ═══ İÇİNDEKİLER ═══
E.append(H1("İçindekiler"))
E.append(RULE())
for no, baslik in [
    ("1", "NarRTOS Nedir?"),
    ("2", "Desteklenen Donanım ve Pin Haritası"),
    ("3", "Sistem Mimarisi"),
    ("4", "Boot (Açılış) Akışı"),
    ("5", "Proje Yapısı"),
    ("6", "Çekirdek Bileşenler (core/)"),
    ("7", "Servis Katmanı (services/)"),
    ("8", "Sürücü Katmanı (drivers/)"),
    ("9", "Derleme, Yükleme ve Çalıştırma"),
    ("10", "Yapılandırma (os_config.h ve Board Varyantları)"),
    ("11", "UI Etkileşim Modeli"),
    ("12", "Yerleşik Uygulamalar"),
    ("13", "Uygulama Ekleme Rehberi (Kod ile)"),
    ("14", "Servis Eklentileri: OTA, Bildirim Köprüsü, Telemetri"),
    ("15", "Board Taşıma (Porting) Rehberi"),
    ("16", "Sorun Giderme"),
]:
    E.append(Paragraph(f"<b>{no}.</b>&nbsp;&nbsp;{baslik}", S["toc"]))
E.append(PageBreak())

# ═══ 1. NEDİR ═══
E.append(H1("1. NarRTOS Nedir?"))
E.append(RULE())
E.append(P("<b>NarRTOS</b>, ESP32-S3 için geliştirilmiş açık kaynak bir gömülü işletim sistemidir. "
           "FreeRTOS çekirdeği üzerinde, LVGL 9 grafik kütüphanesiyle dokunmatik ekranlı akıllı "
           "cihazlar (giyilebilir cihazlar, kontrol panelleri, izleme cihazları) için eksiksiz bir "
           "çalışma ortamı sunar."))
E.append(H2("Temel Özellikler"))
E.append(B("<b>Çift çekirdekli görev mimarisi:</b> UI render'ı ve sensör/servis işleri ayrı "
           "çekirdeklerde, çakışmadan çalışır."))
E.append(B("<b>Modüler uygulama çatısı:</b> Uygulamalar bir vtable (yaşam döngüsü arayüzü) ile "
           "sisteme kaydolur; tek seferde tek uygulama ekranın sahibidir."))
E.append(B("<b>Olay tabanlı iletişim:</b> Görevler arası haberleşme hafif bir pub/sub event bus "
           "üzerinden yapılır — servisler birbirini doğrudan çağırmaz."))
E.append(B("<b>Board varyant sistemi:</b> Aynı kod tabanı, derleme zamanında seçilen farklı ekran/"
           "dokunmatik donanımlarını destekler; uygulama kodu değişmez."))
E.append(B("<b>Servis katmanı:</b> Güç yönetimi, zaman (RTC+NTP), WiFi, BLE, ses, sensör, OTA "
           "güncelleme gibi yetenekler bağımsız servisler olarak sunulur."))
E.append(B("<b>OTA güncelleme:</b> Cihaz, sürüm manifest'ini sorgulayıp kendini alandan "
           "güncelleyebilir (A/B partisyon düzeni)."))
E.append(H2("Sürümleme"))
E.append(P("Sürüm bilgisi <font face='Mono'>core/os_config.h</font> içinde tanımlıdır ve tek bir tam "
           "sayı koduna indirgenir — böylece sürüm karşılaştırmaları string ayrıştırma tuzaklarına "
           "düşmez:"))
E.append(C("""#define OS_VERSION_MAJOR 1
#define OS_VERSION_MINOR 4
#define OS_VERSION_PATCH 81
#define OS_VERSION_STR   "1.4.81"
// OS_VERSION_CODE = MAJOR*10000 + MINOR*100 + PATCH   (1.4.81 -> 10481)
// Manifest karşılaştırması bu integer ile yapılır ("0.10" < "0.2" tuzağı yok)"""))
E.append(PageBreak())

# ═══ 2. DONANIM ═══
E.append(H1("2. Desteklenen Donanım ve Pin Haritası"))
E.append(RULE())
E.append(H2("Referans Board'lar"))
E.append(TBL(["Özellik", "Varsayılan board (esp32s3_wearable)", "LCD varyantı (esp32s3_lcd128)"],
    [["MCU", "ESP32-S3R8 (çift çekirdek, 240 MHz)", "ESP32-S3 (240×240 yuvarlak LCD kartı)"],
     ["Flash / PSRAM", "8 MB QIO / 8 MB Octal PSRAM", "16 MB / 2 MB Quad PSRAM"],
     ["Ekran", "CO5300 AMOLED, QSPI, 410×502", "GC9A01A LCD, SPI, 240×240 yuvarlak"],
     ["Dokunmatik", "FT3168 (I2C)", "CST816S (I2C)"],
     ["PMIC", "AXP2101", "yok (3V3 doğrudan)"],
     ["RTC / IMU", "PCF85063 / ICM42670", "board'a göre"],
    ],
    [30 * mm, 72 * mm, 68 * mm]))
E.append(H2("Varsayılan Board Pin Haritası (core/os_config.h)"))
E.append(TBL(["İşlev", "Pinler"],
    [["Ekran (QSPI)", "CS=12, SCLK=11, D0..D3=4/5/6/7, RST=8, TE=—"],
     ["I2C veriyolu (dokunmatik, PMIC, RTC, IMU)", "SDA=15, SCL=14 (400 kHz)"],
     ["Dokunmatik", "RST=9, INT=38"],
     ["Butonlar (aktif LOW)", "Üst=0 (boot butonu), Alt=10"],
     ["SD kart (SDMMC)", "D0=3, CMD=1, CLK=2"],
     ["Buzzer", "21"]],
    [78 * mm, 92 * mm]))
E.append(CAP("LCD varyantının pinleri ayrı dosyadadır: core/os_config_lcd128_pins.h — varyant "
             "derlemesinde otomatik seçilir."))
E.append(P("Pin tanımları kodda tek yerde yaşar ve tüm sürücüler buradan okur:"))
E.append(C("""// core/os_config.h — GPIO Pin Map (varsayılan board)
#define PIN_DISP_CS     12      // QSPI ekran
#define PIN_DISP_SCLK   11
#define PIN_DISP_D0      4      // D0..D3 = 4/5/6/7
#define PIN_DISP_RST     8
#define PIN_I2C_SDA     15      // paylaşımlı I2C: dokunmatik+PMIC+RTC+IMU
#define PIN_I2C_SCL     14
#define PIN_TOUCH_RST    9
#define PIN_TOUCH_INT   38
#define PIN_BTN_TOP      0      // aktif LOW (boot butonu)
#define PIN_BTN_BOTTOM  10
#define PIN_BUZZER      21"""))
E.append(PageBreak())

# ═══ 3. MİMARİ ═══
E.append(H1("3. Sistem Mimarisi"))
E.append(RULE())
E.append(H2("Katmanlar"))
E.append(C("""+--------------------------------------------------------------+
|  UYGULAMALAR (apps/)     — bkz. Bölüm 12                     |
+--------------------------------------------------------------+
|  ÇEKİRDEK (core/)        app_manager, event_bus,             |
|                          button_handler, os_config           |
+--------------------------------------------------------------+
|  SERVİSLER (services/)   ui, power, time, wifi, ble, audio,  |
|                          sensor, ota, ...                    |
+--------------------------------------------------------------+
|  SÜRÜCÜLER (drivers/)    display, touch, power(PMIC), rtc,   |
|                          imu, audio                          |
+--------------------------------------------------------------+
|  FreeRTOS + ESP-IDF + LVGL 9                                 |
+--------------------------------------------------------------+"""))
E.append(P("Yukarı katman aşağıyı çağırır; aşağıdan yukarıya iletişim <b>event bus</b> ile olur. "
           "Servisler birbirine doğrudan bağımlı değildir — bu, tek bir servisin çıkarılıp "
           "değiştirilebilmesini sağlar."))
E.append(H2("Görev (Task) Dağılımı"))
E.append(TBL(["Görev", "Çekirdek", "Öncelik", "İş"],
    [["UI görevi", "Core 0", "5", "LVGL render döngüsü (ui_service_tick)"],
     ["Sensör görevi", "Core 1", "4", "IMU/adım sayar örnekleme, event üretimi"],
     ["Güç görevi", "—", "6", "Batarya izleme, ekran uyku/uyandırma akışı"],
     ["Servis görevleri", "Core 1", "3", "WiFi bağlantı, tarama, OTA gibi arka plan işleri"]],
    [40 * mm, 22 * mm, 20 * mm, 88 * mm]))
E.append(H2("LVGL Eşzamanlılık Modeli"))
E.append(P("LVGL thread-safe değildir. NarRTOS'ta LVGL mutex'inin sahibi <b>ui_service</b>'tir; "
           "UI görevinin dışından yapılacak her <font face='Mono'>lv_*</font> çağrısı kilit altında "
           "yapılmak zorundadır:"))
E.append(C("""ui_service_lock();          // LVGL mutex'ini al
lv_label_set_text(lbl, "merhaba");
ui_service_unlock();        // bırak
// UI görevi kendi döngüsünde: uint32_t next = ui_service_tick();"""))
E.append(PageBreak())

# ═══ 4. BOOT ═══
E.append(H1("4. Boot (Açılış) Akışı"))
E.append(RULE())
E.append(P("<font face='Mono'>src/main.cpp</font> içindeki <font face='Mono'>app_main()</font>, "
           "sistemi katman katman ayağa kaldırır. Sıra önemlidir — her adım bir öncekinin "
           "kurduğu altyapıya dayanır:"))
E.append(C("""1.  nvs_flash_init()            // kalıcı ayar deposu (NVS)
2.  i2c_bus_init()              // paylaşımlı I2C veriyolu
3.  axp2101_init()              // PMIC: güç raylarını aç (varsa)
4.  event_bus_init()            // pub/sub altyapısı
5.  button_handler_init()       // fiziksel butonlar
6.  pcf85063_init()             // RTC oku -> sistem saati
    time_service_init()         // NTP senkron daemon'u
7.  wifi_service_init()         // WiFi (kullanıcı tercihi NVS'ten okunur;
                                //  OFF ise auto-connect atlanır)
8.  co5300_init()               // ekran denetleyicisi (parlaklık 0'da baslar)
9.  ui_service_init()           // LVGL + display driver + mutex
10. ft3168_init()               // dokunmatik -> LVGL input device
11. app_manager_init()          // uygulama çatısı
12. alarm_service_init()        // LVGL timer tabanlı zamanlayıcı
13. audio_service_init()        // buzzer/ses
14. app_manager_launch(...)     // ilk ekran devralınır
    -> ekran parlaklığı ramp ile açılır (görsel "açılış" anı)
15. icm42670_init()             // IMU
    sensor_service_init()       // sensör görevi (Core 1)
16. power_service_init()        // güç/uyku yönetimi
17. ota_service_init()          // güncelleme denetleyicisi"""))
E.append(P("<b>Hata toleransı ilkesi:</b> Çevre birim init'leri (dokunmatik, RTC, IMU, PMIC) "
           "başarısız olsa bile sistem boot'a devam eder — hata loglanır, ilgili özellik devre dışı "
           "kalır. Cihaz, tek bir çevre biriminin arızasıyla açılamaz duruma düşmez."))
E.append(PageBreak())

# ═══ 5. PROJE YAPISI ═══
E.append(H1("5. Proje Yapısı"))
E.append(RULE())
E.append(C("""embedded-wear-os/
|-- platformio.ini              // build ortamları (env'ler)
|-- partitions_custom.csv       // A/B OTA partisyon düzeni
|-- sdkconfig.*                 // ESP-IDF konfigürasyonları (env başına)
|-- src/
|   |-- main.cpp                // boot akışı (app_main)
|   |-- core/                   // ÇEKİRDEK
|   |   |-- os_config.h         //   sürüm, pin haritası, görev ayarları
|   |   |-- app_manager.{h,cpp} //   uygulama yaşam döngüsü
|   |   |-- event_bus.{h,cpp}   //   pub/sub olay veriyolu
|   |   |-- button_handler.*    //   fiziksel buton yönetimi
|   |-- services/               // SERVİSLER (ui, power, time, wifi,
|   |                           //   ble, audio, sensor, ota, ...)
|   |-- drivers/                // SÜRÜCÜLER
|   |   |-- display/            //   co5300 (AMOLED), gc9a01 (LCD varyanti)
|   |   |-- touch/              //   ft3168, cst816s
|   |   |-- power/  rtc/  imu/  audio/
|   |-- hal/                    // donanım soyutlama yardımcıları (I2C bus)
|   |-- apps/                   // uygulama katmanı (bkz. Bölüm 12)
|   |-- assets/  fonts/  utils/
|-- docs/                       // dokümantasyon"""))

# ═══ 6. ÇEKİRDEK ═══
E.append(H1("6. Çekirdek Bileşenler (core/)"))
E.append(RULE())
E.append(H2("event_bus — Pub/Sub Olay Veriyolu"))
E.append(P("Görevler arası iletişimin tek yolu. Her abone kendi FreeRTOS kuyruğuna sahiptir; "
           "yayınlanan olay tüm abone kuyruklarına dağıtılır ve <b>abonenin kendi görev "
           "bağlamında</b> işlenir (ISR-güvenli yayın)."))
E.append(C("""// Olay grupları (event_bus.h):
EVENT_BATTERY_LEVEL / EVENT_CHARGING_STATE / EVENT_LOW_BATTERY   // güç
EVENT_STEP_COUNT / EVENT_GESTURE_WRIST_UP / EVENT_SHAKE          // sensör
EVENT_TOUCH_DOWN / EVENT_BTN_TOP / EVENT_BTN_BOTTOM              // girdi
EVENT_TIME_UPDATED / EVENT_APP_SWITCH / EVENT_WEATHER_UPDATE     // sistem"""))
E.append(H2("app_manager — Uygulama Yaşam Döngüsü"))
E.append(P("Her uygulama bir <font face='Mono'>app_interface_t</font> vtable'ı uygular; aynı anda "
           "yalnızca bir uygulama aktiftir (LVGL ekranının sahibi odur):"))
E.append(C("""typedef struct {
    void (*on_start)(lv_obj_t* screen); // ilk açılış: UI'yi bu ekrana kur
    void (*on_resume)(void);            // arka plandan öne gelme
    void (*on_pause)(void);             // başka uygulama devraldı
    void (*on_stop)(void);              // tamamen kapat, kaynakları bırak
    bool (*on_event)(const os_event_t*);// aktif uygulamaya olay teslimi
    const char* name;                   // log için isim
} app_interface_t;

void app_manager_register(app_id_t id, const app_interface_t* app);
void app_manager_launch(app_id_t id);   // geçiş animasyonlu
void app_manager_back(void);            // önceki uygulamaya dön"""))
E.append(H2("button_handler"))
E.append(P("İki fiziksel butonu (üst/alt, aktif LOW) debounce edip kısa/uzun basış ayrımıyla "
           "event bus'a <font face='Mono'>EVENT_BTN_TOP/BOTTOM</font> olarak yayınlar."))
E.append(PageBreak())

# ═══ 7. SERVİSLER ═══
E.append(H1("7. Servis Katmanı (services/)"))
E.append(RULE())
E.append(P("Temel işletim sisteminin çekirdek servisleri şunlardır:"))
E.append(TBL(["Servis", "Görev"],
    [["ui_service", "LVGL başlatma, render döngüsü, LVGL mutex sahipliği, dokunmatik girdi kaydı"],
     ["power_service", "Batarya izleme, ekran uyku/uyandırma, aktivite takibi (dokunma/hareket)"],
     ["time_service", "RTC'den saat okuma + WiFi varken NTP senkronu; EVENT_TIME_UPDATED yayını"],
     ["wifi_service", "Bağlantı yönetimi, asenkron ağ taraması, kullanıcı tercihi (NVS'te kalıcı)"],
     ["ble_service", "BLE altyapısı (GATT sunucu; bildirim köprüsü gibi profillerin temeli)"],
     ["audio_service", "Buzzer/ses çıkışı (alarm ve bildirim sesleri)"],
     ["sensor_service", "IMU örnekleme görevi (Core 1); adım/jest olayları üretir"],
     ["ota_service", "Sürüm manifest'i sorgulama, imzalı imaj indirme, A/B güncelleme"],
     ["alarm_service", "Zamanlayıcı/alarm çekirdeği (LVGL timer tabanlı)"]],
    [38 * mm, 132 * mm]))
E.append(P("Servisler <b>opsiyoneldir ve genişletilebilir</b>: yeni bir servis eklemek, "
           "<font face='Mono'>services/</font> altına bir çift dosya ekleyip boot akışında init "
           "çağırmaktan ibarettir. Uygulama katmanına dönük servisler (hava durumu, galeri, mağaza, "
           "telemetri vb.) aynı kalıpla eklenir."))
E.append(H2("ui_service API'si (en sık kullanılan)"))
E.append(C("""void     ui_service_init(void);      // LVGL + display driver + mutex
void     ui_service_register_touch();// dokunmatigi LVGL'e bagla
uint32_t ui_service_tick(void);      // UI görev döngüsü (mutex içinde
                                     //   lv_timer_handler)
void     ui_service_lock(void);      // görevler arası UI güncellemesi için
void     ui_service_unlock(void);"""))
E.append(PageBreak())

# ═══ 8. SÜRÜCÜLER ═══
E.append(H1("8. Sürücü Katmanı (drivers/)"))
E.append(RULE())
E.append(TBL(["Sürücü", "Çip", "Arayüz", "Not"],
    [["display/co5300", "CO5300 AMOLED", "QSPI", "Varsayılan ekran; parlaklık panel komutuyla"],
     ["display/gc9a01_128", "GC9A01A LCD", "SPI + DC", "1.28\" varyant; parlaklık LEDC PWM (arka ışık)"],
     ["touch/ft3168", "FT3168", "I2C", "Varsayılan dokunmatik"],
     ["touch/cst816s_128", "CST816S", "I2C", "1.28\" varyant dokunmatiği"],
     ["power/axp2101", "AXP2101 PMIC", "I2C", "Güç rayları, batarya ölçümü, şarj"],
     ["rtc/pcf85063", "PCF85063", "I2C", "Gerçek zaman saati (güç kesilse de saat korunur)"],
     ["imu/icm42670", "ICM42670", "I2C", "İvme/jiroskop — adım sayar ve jestler"]],
    [40 * mm, 33 * mm, 24 * mm, 73 * mm]))
E.append(H2("Varyant Kalıbı: Aynı Sembol Seti"))
E.append(P("Alternatif ekran sürücüleri, varsayılan sürücüyle <b>birebir aynı fonksiyon imzalarını</b> "
           "(<font face='Mono'>co5300_*</font>) dışa verir ve tamamı kendi derleme guard'ı içindedir:"))
E.append(C("""// gc9a01_128_driver.cpp
#ifdef DISPLAY_VARIANT_LCD128        // yalnız LCD varyantında derlenir
...  co5300_init(), co5300_lvgl_flush_cb(), ...  // aynı isimler
#endif
// co5300_driver.cpp'deki guard, LCD build'ında onu bosaltır ->
// linker her ortamda TEK sembol seti görür; main.cpp / ui_service DEĞİŞMEZ."""))
E.append(P("Yeni bir ekran/board eklemek = yeni bir sürücü dosyası + pin haritası başlığı + "
           "platformio.ini'de yeni bir env. Uygulama ve servis kodu değişmez."))

# ═══ 9. DERLEME ═══
E.append(H1("9. Derleme, Yükleme ve Çalıştırma"))
E.append(RULE())
E.append(H2("Gereksinimler"))
E.append(B("<b>PlatformIO Core</b> (CLI) — ESP-IDF araç zincirini otomatik kurar"))
E.append(B("USB-seri sürücüsü (board'daki köprüye göre; ör. CH34x)"))
E.append(H2("Komutlar"))
E.append(C("""# Derle (varsayılan board)
pio run -e esp32s3_wearable

# Derle + cihaza yükle
pio run -e esp32s3_wearable -t upload
#   gerekirse port belirt: --upload-port /dev/cu.usbserial-XXXX

# Seri monitör (log izleme, 115200 baud)
pio device monitor

# LCD 1.28" varyantı için:
pio run -e esp32s3_lcd128 -t upload"""))
E.append(H2("Build Ortamları (platformio.ini)"))
E.append(TBL(["Env", "Board", "Önemli ayarlar"],
    [["esp32s3_wearable", "ESP32-S3R8 + AMOLED", "8 MB flash QIO, Octal PSRAM, upload 921600, "
      "partitions_custom.csv (A/B OTA)"],
     ["esp32s3_lcd128", "240×240 yuvarlak LCD kartı", "extends: esp32s3_wearable; 16 MB flash, Quad "
      "PSRAM, upload 460800, CO5300_H/V_RES=240, DISPLAY/BOARD_VARIANT_LCD128"]],
    [36 * mm, 46 * mm, 88 * mm]))
E.append(P("Her ortamın kendi <font face='Mono'>sdkconfig.&lt;env&gt;</font> dosyası vardır (PSRAM tipi, "
           "flash boyutu gibi ESP-IDF ayarları). Partisyon düzeni A/B OTA'ya göre kurgulanmıştır: "
           "iki uygulama bölümü (app0/app1) + NVS + varlık (assets) bölümü."))
E.append(H2("İlk Açılış"))
E.append(P("Yükleme sonrası cihaz otomatik reset atar; seri monitörde boot logları görülür "
           "(Bölüm 4'teki sıra). Sistem, saat kadranıyla (WatchFace) açılır ve dokunmatik/butonlar "
           "aktiftir."))
E.append(PageBreak())

# ═══ 10. YAPILANDIRMA ═══
E.append(H1("10. Yapılandırma (os_config.h ve Board Varyantları)"))
E.append(RULE())
E.append(P("<font face='Mono'>core/os_config.h</font> sistemin tek merkezi yapılandırma dosyasıdır: "
           "sürüm, pin haritası, görev çekirdeği/öncelikleri, ekran tamponu ve dış servis adresleri "
           "burada tanımlıdır. Tüm tanımlar <font face='Mono'>#ifndef</font> korumalıdır — yani "
           "derleme bayraklarıyla (env üzerinden) ezilebilir:"))
E.append(C("""// os_config.h (varsayılanlar)
#define UI_TASK_CORE        0     // LVGL render çekirdeği
#define SENSOR_TASK_CORE    1
#define UI_TASK_PRIORITY    5
#define POWER_TASK_PRIORITY 6

#ifndef OTA_BACKEND_HOST                     // env ile ezilebilir:
#define OTA_BACKEND_HOST "http://..."        // platformio.ini ->
#endif                                       // '-DOTA_BACKEND_HOST="..."'"""))
E.append(H2("Board Varyantı Nasıl Çalışır?"))
E.append(B("<font face='Mono'>-DDISPLAY_VARIANT_LCD128</font> → alternatif ekran/dokunmatik "
           "sürücüleri derlenir, varsayılanlar boşalır (Bölüm 8'deki sembol kalıbı)."))
E.append(B("<font face='Mono'>-DBOARD_VARIANT_LCD128</font> → pin haritası "
           "<font face='Mono'>os_config_lcd128_pins.h</font>'tan gelir; board'a özgü servis "
           "davranışları bu guard ile ayrılır."))
E.append(B("Çözünürlük <font face='Mono'>-DCO5300_H_RES / V_RES</font> bayraklarıyla verilir; "
           "UI ölçü sabitleri varyanta göre otomatik küçük değerlerini alır."))
E.append(H2("Farklı Çözünürlükler: UI Ölçü Sabitleri Kalıbı"))
E.append(P("Uygulamaların iki çözünürlükte de (410×502 / 240×240) yeniden yazılmadan çalışmasını "
           "sağlayan kalıp: her uygulama, ekrana bağlı ölçülerini <b>tek bir varyant-korumalı sabit "
           "bloğunda</b> tanımlar; UI kodu yalnızca bu sabitleri kullanır. Launcher'dan birebir örnek:"))
E.append(C("""// launcher_app.cpp — ölçü sabitleri (tek blok, uygulamanın başında)
#ifdef BOARD_VARIANT_LCD128          // 240×240 yuvarlak LCD
#define L_SCR_W         240
#define L_CELL_W        88           // grid hücresi
#define L_CELL_H        92
#define L_ICON          64           // uygulama ikonu
#define L_SYM_FONT      (&amp;lv_font_montserrat_28)
#define L_MODAL_W       220
#else                                // 410×502 AMOLED (varsayılan)
#define L_SCR_W         410
#define L_CELL_W        120
#define L_CELL_H        128
#define L_ICON          95
#define L_SYM_FONT      (&amp;lv_font_montserrat_36)
#define L_MODAL_W       340
#endif
// UI kodu her yerde SADECE sabiti kullanır:
lv_obj_set_size(cell, L_CELL_W, L_CELL_H);"""))
E.append(P("Basit yükseklik/konum farkları için aynı kalıbın satır-içi hali de kullanılır "
           "(settings_app.cpp'den):"))
E.append(C("""#ifdef BOARD_VARIANT_LCD128
#define VIEW_SCR_H     240           // scroll edilebilir görünüm yüksekliği
#else
#define VIEW_SCR_H     502
#endif
lv_obj_set_size(sbox, LV_PCT(100), VIEW_SCR_H);"""))
E.append(P("<b>Değişmezlik kuralı:</b> Varsayılan (AMOLED) dal, <font face='Mono'>#else</font> "
           "tarafında <b>orijinal değerlerin birebir aynısını</b> taşır — böylece varyant eklemek, "
           "preprocessor sonrası varsayılan build'ın binary'sini <b>tek byte değiştirmez</b>. Yeni "
           "bir çözünürlük desteklemek = her uygulamanın sabit bloğuna bir dal eklemek; UI mantığına "
           "dokunulmaz."))
E.append(PageBreak())

# ═══ 11. UI ETKİLEŞİM MODELİ ═══
E.append(H1("11. UI Etkileşim Modeli"))
E.append(RULE())
E.append(P("NarRTOS'un kullanıcı etkileşimi üç girdiye dayanır: <b>dokunmatik jestler</b>, "
           "<b>iki fiziksel buton</b> ve <b>hareket</b> (bilek kaldırma). Bu bölüm, cihazın "
           "\"nasıl kullanıldığını\" ve bu davranışın kodda nerede yaşadığını anlatır."))
E.append(H2("Jest (Swipe) Sistemi"))
E.append(P("Dokunmatik denetleyici donanımsal jest verisi göndermediği için swipe algılama "
           "<b>yazılımda</b> yapılır: dokunma okuma geri çağrısı (ui_service) parmak izini takip "
           "edip yön çıkarır ve global <font face='Mono'>g_swipe_dir</font> değişkenine yazar "
           "(-1 sol, +1 sağ, -2 yukarı, +2 aşağı). app_manager'ın 50 ms'lik olay zamanlayıcısı bu "
           "değeri okuyup navigasyona çevirir."))
E.append(TBL(["Bağlam", "Jest", "Davranış"],
    [["WatchFace (kadran)", "Sağa swipe", "Launcher'ı açar (uygulama menüsü)"],
     ["WatchFace", "Sola / yukarı / aşağı", "Bilinçli olarak atanmamıştır (yanlışlıkla ekran "
      "değişimini önlemek için)"],
     ["Diğer tüm ekranlar", "Sağa veya sola swipe", "Geri (back) — tek elle kullanım için iki yön "
      "de geri çalışır; Settings gibi çok sayfalı uygulamalar önce kendi iç 'back'ini tüketir"],
     ["Üst kenar (y ≤ 40 px)", "Aşağı çekme", "Control Center'ı açar (yalnızca kenardan — ekran "
      "ortasından aşağı sürükleme kaydırma olarak kalır)"],
     ["Control Center açıkken", "Yukarı/sağa/sola swipe veya karartılmış zemine dokunma", "Paneli kapatır"]],
    [42 * mm, 38 * mm, 90 * mm]))
E.append(P("Tablodaki davranışın kod karşılığı (app_manager.cpp — sadeleştirilmiş):"))
E.append(C("""static void check_swipe(void)         // 50 ms'lik olay zamanlayıcısında
{
    if (ui_service_is_swipe_locked()) { g_swipe_dir = 0; return; }
    if (g_swipe_dir == 0) return;
    int8_t dir = g_swipe_dir;  g_swipe_dir = 0;   // okundu, sıfırla

    if (control_center_is_open()) { control_center_close(); return; }

    switch (dir) {
        case -1:  // sağa swipe
            if (s_current == APP_ID_WATCHFACE)
                app_manager_launch(APP_ID_LAUNCHER);   // kadran -> menü
            else
                app_manager_back();                    // diğerlerinde: geri
            break;
        case +1:  // sola swipe: kadranda no-op, diğerlerinde geri
            if (s_current != APP_ID_WATCHFACE) app_manager_back();
            break;
        // -2/+2 (dikey): kadranda atanmadı; CC yalnız üst kenardan açılır
    }
}"""))
E.append(P("<b>Swipe kilidi:</b> Sürükleme tabanlı widget'lar (ör. parlaklık çubuğu) sürükleme "
           "boyunca swipe navigasyonunu kilitler (<font face='Mono'>ui_service_is_swipe_locked</font>) "
           "— slider kullanırken yanlışlıkla \"geri\" tetiklenmez."))
E.append(H2("Control Center"))
E.append(P("Üstten aşağı çekilen hızlı ayarlar paneli. Aktif uygulamanın <b>üzerine</b> çizilir "
           "(<font face='Mono'>lv_layer_top()</font>) — uygulama ekranı bozulmaz. İçerik: "
           "<b>WiFi | Bluetooth | Uçak modu | El feneri</b> hızlı anahtarları."))
E.append(H2("Fiziksel Butonlar"))
E.append(TBL(["Buton", "Pin", "Rol"],
    [["Üst buton", "GPIO 0", "Uyandırma / ana ekrana dönüş (kısa-uzun basış event bus'tan uygulamaya iletilir)"],
     ["Alt buton", "GPIO 10", "Uyandırma / uygulamaya özgü işlev"]],
    [30 * mm, 22 * mm, 118 * mm]))
E.append(H2("Güç Durumları ve Uyandırma"))
E.append(P("power_service dört durumlu bir makine işletir; her kullanıcı etkileşimi sayacı sıfırlar:"))
E.append(C("""ACTIVE --(30 sn hareketsizlik)--> DIM --(5 sn)--> DISPLAY_OFF
   ^                                                  |
   +---- dokunma / buton / bilek kaldırma (WOM) ------+
DISPLAY_OFF --(10 dk hareketsizlik)--> DEEP_SLEEP
  (derin uykudan uyandırma: buton, RTC alarmı)"""))
E.append(P("Uyandırma kaynakları loglarda ayırt edilir (dokunma, üst/alt buton, bilek kaldırma) — "
           "sahte uyandırmaların teşhisi için. Bilek kaldırma, IMU'nun hareket-ile-uyandırma (WOM) "
           "kesmesiyle gelir."))
E.append(PageBreak())

# ═══ 12. YERLEŞİK UYGULAMALAR ═══
E.append(H1("12. Yerleşik Uygulamalar"))
E.append(RULE())
E.append(P("NarRTOS, temel bir akıllı cihaz deneyimi için gereken uygulama setiyle gelir. Her biri "
           "Bölüm 6'daki <font face='Mono'>app_interface_t</font> çatısını kullanır; kimlikleri "
           "<font face='Mono'>os_config.h</font>'taki <font face='Mono'>app_id_t</font> enum'unda tanımlıdır."))
E.append(TBL(["Uygulama", "APP_ID", "Ne yapar / Kullandığı servisler"],
    [["WatchFace", "0", "Varsayılan ana ekran: saat, tarih, batarya, adım sayısı. "
      "EVENT_BATTERY_LEVEL / EVENT_STEP_COUNT / EVENT_TIME_UPDATED olaylarıyla canlı güncellenir."],
     ["Launcher", "3", "3×3 grid uygulama menüsü (WatchFace'ten sağa swipe ile). Her hücre "
      "120×128 px; gradient ikonlar. Uygulamaları app_manager_launch ile açar."],
     ["Settings", "2", "Çok sayfalı ayarlar: WiFi (ağ tarama, el yazısıyla veya klavyeyle şifre "
      "girişi), ekran/parlaklık, Software Updates (OTA arayüzü), sistem bilgisi. Kendi iç geri "
      "yığınını yönetir (settings_app_handle_back)."],
     ["Alarm", "10", "Alarm listesi + kurma ekranı (kadran/dial ile saat seçimi). alarm_service + "
      "audio_service kullanır; alarm anında EVENT_APP_SWITCH ile öne gelir."],
     ["Weather", "6", "Detaylı hava ekranı: şehir + büyük sıcaklık + UV/Rüzgar/Nem halkaları. "
      "Veri: weather_service (Open-Meteo, çoklu şehir); WiFi gerektirir."],
     ["Gallery", "9", "Cihaz depolamasındaki (SPIFFS assets bölümü) görselleri ızgara halinde "
      "gösterir. Görseller WiFi üzerinden cihazın yükleme sayfasıyla aktarılır "
      "(face_upload_service)."],
     ["Store", "11", "Uygulama mağazası: yüklü uygulamaların tarayıcısı (Apps) + backend "
      "katalogundan kurulabilir içerik (Store). Metadata-only mimari — ikili kod cihazda derlidir, "
      "backend yalnızca katalog sunar."],
     ["Notifications", "8", "Telefon bildirimleri (BLE/ANCS köprüsü — iPhone): liste + detay + "
      "dismiss. ble_service + notification_store kullanır; banner servisi anlık bildirim düşürür."],
     ["StepCounter", "1", "Adım sayar ekranı — sensor_service'in IMU tabanlı adım verisini gösterir."],
     ["Placeholder'lar", "5/7", "HeartRate, Timer: gelecek uygulamalar için iskelet — çatının "
      "boş şablonu olarak da kullanılabilir."]],
    [26 * mm, 16 * mm, 128 * mm]))
E.append(P("<b>Uygulama–servis iletişimine gerçek örnek:</b> WatchFace, hava durumu ikonunu "
           "weather_service'in yayınladığı olayla tazeler — servis çağrılmaz, olay dinlenir "
           "(watchface_app.cpp'den birebir):"))
E.append(C("""static bool on_event(const os_event_t* event)
{
    if (!event) return false;
    if (event->type == EVENT_WEATHER_UPDATE) {
        // Hava durumu arka plan görevi yeni veri yayınladı -> ikonu yenile.
        // UI task bağlamındayız; doğrudan render güvenli.
        if (s_activity_icon_cloud) {
            weather_icon_render(s_activity_icon_cloud,
                                (weather_condition_t)event->data);
        }
        return true;    // olay tüketildi
    }
    return false;       // ilgilenmiyorum -> diğer dinleyicilere geçsin
}"""))
E.append(P("<b>Tasarım notu:</b> Uygulamalar ekran ölçülerini sabit yazmaz; ölçü sabitleri board "
           "varyantına göre seçilir (410×502 AMOLED / 240×240 LCD). Böylece aynı uygulama iki "
           "donanımda da yeniden yazılmadan çalışır (bkz. Bölüm 10)."))
E.append(PageBreak())

# ═══ 13. UYGULAMA EKLEME REHBERİ ═══
E.append(H1("13. Uygulama Ekleme Rehberi (Kod ile)"))
E.append(RULE())
E.append(P("Sisteme yeni bir uygulama eklemek <b>4 adımdır</b>: uygulama dosyalarını yaz, kimlik "
           "(APP_ID) tanımla, merkezî kayda ekle, (istenirse) Launcher'a kutucuk koy. Aşağıdaki "
           "örnek 'MyApp' adında boş bir uygulamayı uçtan uca ekler."))
E.append(H2("Adım 1 — Uygulama dosyalarını yaz: src/apps/myapp/"))
E.append(P("Her uygulama iki dosyadan oluşur ve dışarıya tek bir fonksiyon verir: "
           "<font face='Mono'>*_get_interface()</font> (yerleşik uygulamaların tamamı bu kalıptadır)."))
E.append(C("""// src/apps/myapp/myapp_app.h
#pragma once
#include "../../core/app_manager.h"
const app_interface_t* myapp_get_interface(void);

// src/apps/myapp/myapp_app.cpp
#include "myapp_app.h"

static void my_on_start(lv_obj_t* screen) {
    lv_obj_t* lbl = lv_label_create(screen);
    lv_label_set_text(lbl, "Merhaba NarRTOS");
    lv_obj_center(lbl);
    app_manager_enable_gestures(screen);   // swipe'ların ekrana ulasması için
}
static void my_on_stop(void) { /* on_start'ta ayrılan kaynakları bırak */ }
static bool my_on_event(const os_event_t* e) {
    // örn. EVENT_TIME_UPDATED geldiğinde etiketi tazele
    return false;                          // true = olayı tükettim
}

static const app_interface_t my_app = {
    .on_start  = my_on_start,
    .on_resume = NULL, .on_pause = NULL,
    .on_stop   = my_on_stop,
    .on_event  = my_on_event,
    .name      = "myapp",
};
const app_interface_t* myapp_get_interface(void) { return &amp;my_app; }"""))
E.append(H2("Adım 2 — Kimlik tanımla: core/os_config.h"))
E.append(C("""typedef enum {
    APP_ID_WATCHFACE    = 0,
    ...
    APP_ID_STORE        = 11,
    APP_ID_MYAPP        = 12,   // <-- YENİ: her zaman APP_ID_COUNT'tan önce
    APP_ID_COUNT
} app_id_t;"""))
E.append(H2("Adım 3 — Merkezî kayda ekle: core/app_manager.cpp"))
E.append(P("Tüm uygulamalar tek yerde, <font face='Mono'>app_manager_init()</font> içinde "
           "kaydedilir — main.cpp'ye dokunulmaz:"))
E.append(C("""#include "../apps/myapp/myapp_app.h"

void app_manager_init(void) {
    ...
    app_manager_register(APP_ID_WATCHFACE, watchface_app_get_interface());
    ...
    app_manager_register(APP_ID_MYAPP,     myapp_get_interface());   // <-- YENİ
}"""))
E.append(H2("Adım 4 — (İsteğe bağlı) Launcher'a kutucuk ekle"))
E.append(P("Kayıt, uygulamayı <b>çalıştırılabilir</b> yapar; menüde görünmesi için "
           "<font face='Mono'>src/apps/launcher/launcher_app.cpp</font>'deki grid tanımına bir "
           "kutucuk (isim + ikon sembolü + APP_ID) eklenir. Kutucuğa dokunma "
           "<font face='Mono'>app_manager_launch(APP_ID_MYAPP)</font> çağırır. Launcher'a "
           "eklemeden de uygulama başka bir yerden (buton olayı, alarm, başka uygulama) "
           "<font face='Mono'>app_manager_launch</font> ile açılabilir."))
E.append(H2("Derle ve çalıştır"))
E.append(C("""pio run -e esp32s3_wearable -t upload   # cihazda: Launcher'da yeni kutucuk"""))
E.append(H2("Uyulması Gereken Kurallar"))
E.append(B("UI yalnızca <font face='Mono'>on_start</font>'a verilen ekrana kurulur; ekran nesnesini "
           "saklayıp başka ekranlara dokunulmaz."))
E.append(B("Başka görevden (ör. bir servis callback'i) UI güncellemesi "
           "<font face='Mono'>ui_service_lock/unlock</font> ile sarılır (Bölüm 3)."))
E.append(B("<font face='Mono'>on_stop</font>, <font face='Mono'>on_start</font>'ta ayrılan her "
           "kaynağı (timer, buffer, servis aboneliği) bırakmalıdır — uygulamalar tekrar tekrar "
           "açılıp kapanır."))
E.append(B("Ekran ölçüleri sabit yazılmaz; board varyantına göre seçilen ölçü sabitleri kullanılır "
           "(410×502 / 240×240 — bkz. Bölüm 10 ve 12)."))

# ═══ 14. SERVİS EKLENTİLERİ ═══
E.append(PageBreak())
E.append(H1("14. Servis Eklentileri: OTA, Bildirim Köprüsü, Telemetri"))
E.append(RULE())
E.append(P("Çekirdek servislerin (Bölüm 7) üzerine kurulu üç büyük eklenti, NarRTOS'un 'bağlı cihaz' "
           "yeteneklerini oluşturur. Üçü de aynı ilkeyle yazılmıştır: <b>başarısızlık sistemi "
           "durdurmaz</b> — ağ yoksa/karşı taraf yoksa özellik sessizce bekler, cihaz çalışmaya "
           "devam eder."))

E.append(H2("14.1 OTA — Kablosuz Firmware Güncelleme (ota_service)"))
E.append(P("Cihaz kendini WiFi üzerinden günceller. Akış, Settings → Software Updates ekranından "
           "yönetilir:"))
E.append(C("""1. Kullanıcı "Software Updates"e girer -> ota_service_check_async()
2. Manifest HTTP GET edilir:
     OTA_BACKEND_HOST/api/wear-os/manifest?device_id=&lt;MAC&gt;&amp;current=&lt;sürüm&gt;
3. manifest.version_code > OS_VERSION_CODE ise "Update available"
4. "Install" -> ota_service_install_async():
     esp_https_ota ile PASİF partisyona indir + flash
     sha256 doğrulama -> esp_ota_set_boot_partition -> esp_restart"""))
E.append(P("<b>Manifest JSON formatı:</b>"))
E.append(C("""{ "version": "1.5.0",  "version_code": 10500,
  "url": "https://.../firmware-1.5.0.bin",
  "size": 2480000,  "notes": "Bug fixes" }"""))
E.append(P("<b>A/B partisyon düzeni</b> (partitions_custom.csv) — güncelleme her zaman pasif "
           "bölüme yazılır; başarısız olursa cihaz mevcut bölümden açılmaya devam eder:"))
E.append(C("""nvs      data  nvs      0x9000    0x5000     // kalıcı ayarlar
otadata  data  ota      0xe000    0x2000     // aktif bölüm seçimi
app0     app   ota_0    0x10000   0x300000   // uygulama A
app1     app   ota_1    0x310000  0x300000   // uygulama B
assets   data  spiffs   0x610000  0x1F0000   // görseller/varlıklar"""))
E.append(B("Tüm uzun işler ayrı FreeRTOS görevinde — UI hiç bloklanmaz; ilerleme, callback'lerle "
           "LVGL thread'ine <font face='Mono'>lv_async_call</font> üzerinden taşınır."))
E.append(B("Durum makinesi: IDLE → CHECKING → UP_TO_DATE / UPDATE_AVAILABLE → DOWNLOADING → "
           "INSTALLED (reboot) / ERROR."))
E.append(B("Güncelleme yaşam döngüsü olayları (başladı/başarılı/başarısız) backend'e best-effort "
           "raporlanır — erişilemezse OTA akışı bozulmaz."))

E.append(H2("14.2 Bildirim Köprüsü — BLE / ANCS (ancs_service)"))
E.append(P("Telefon bildirimlerini (iPhone — Apple Notification Center Service) cihaza yansıtır. "
           "Notifications uygulaması (Bölüm 12) bu köprünün UI'sidir:"))
E.append(C("""1. ble_service: BLE bağlantı + bonding -> ancs_service_on_connected()
2. iPhone'daki ANCS GATT servisi discover edilir
3. NotificationSource + DataSource karakteristiklerine subscribe
4. Bildirim gelir -> 8 baytlık olay (kategori, ekle/sil, ID)
5. Önemli kategoriler (Social/Email/Call) için ControlPoint'e
   GetNotificationAttributes yazılır -> başlık/mesaj/uygulama çekilir
6. Çok paketli yanıt parse edilir -> notification_store'a eklenir
   -> banner servisi anlık bildirim düşürür, uygulama listeler"""))
E.append(B("<b>notification_store:</b> sabit kapasiteli, mutex korumalı ring buffer — tek üretici "
           "(BLE görevi) + tek tüketici (UI). Doldukça en eski bildirim düşer (FIFO)."))
E.append(B("Bildirimler kalıcı saklanmaz — boot'ta liste boştur; kaynak zaten telefondur."))
E.append(B("Kullanıcı 'dismiss' ettiğinde hem yerel kayıt silinir hem ANCS üzerinden telefona "
           "iletilir."))

E.append(H2("14.3 Telemetri ve Konumlandırma (telemetry_service)"))
E.append(P("Cihaz, 60 sn'de bir sağlık telemetrisi (kimlik, sinyal gücü, uptime, boş RAM, sürüm) "
           "ve 5 dk'da bir yenilenen <b>çevre WiFi taraması</b> (BSSID+RSSI listesi) raporlar. "
           "Sunucu tarafı bu taramadan cihazın konumunu çözer (WiFi geolocation, ±20 m) ve veriyi "
           "mesaj kuyruğu (RabbitMQ) üzerinden tüketici sistemlere yayınlar — GPS donanımı olmadan "
           "genel izleme (giyilebilir, hayvan, yaşlı, eşya takibi) mümkün olur."))
E.append(B("Fire-and-forget tasarım: ağ/sunucu yoksa rapor atlanır, cihaz etkilenmez."))
E.append(B("Tarama/rapor ayrımı: aktif WiFi taraması bağlantıyı kısa süre böler; bu yüzden tarama "
           "seyrek yapılıp önbelleklenir, raporlar önbellekle temiz gider."))
E.append(P("Çekirdek döngü (telemetry_service.cpp — sadeleştirilmiş):"))
E.append(C("""#define TELEMETRY_PERIOD_MS         (60 * 1000)  // rapor periyodu
#define TELEMETRY_SCAN_EVERY_CYCLES 5            // 5 turda bir tarama

static void telemetry_task(void* arg) {
    uint32_t cycle = 0;
    for (;;) {
        if (wifi_service_is_connected()) {
            if (cycle % TELEMETRY_SCAN_EVERY_CYCLES == 0)
                refresh_wifi_scan();   // BSSID listesini önbelleğe al
            else                       //   (bu tur rapor YOK — bağlantı toparlansın)
                send_report();         // önbellekli listeyle temiz HTTP POST
        }
        cycle++;
        vTaskDelay(pdMS_TO_TICKS(TELEMETRY_PERIOD_MS));
    }
}

// send_report() gövdesi — cihazın gönderdiği JSON:
snprintf(body, sizeof(body),
    "{\\"device_id\\":\\"%s\\",\\"wifi_rssi\\":%d,\\"wifi\\":%s,"
    "\\"uptime_s\\":%lld,\\"firmware_version\\":\\"%s\\",\\"heap_free\\":%u}",
    s_device_id, rssi, s_wifi_json, uptime, OS_VERSION_STR, heap);
// s_wifi_json = [{"mac":"48:BD:4A:...","rssi":-53}, ...]  (çevre taraması)"""))
E.append(P("Bu alt sistemin tam dokümantasyonu (backend, geolocation servisi, kuyruk formatları, "
           "yönetim paneli) ayrı dokümandadır: <b>docs/telemetri-dokumantasyonu.pdf</b>."))
E.append(PageBreak())

# ── 14.4 KOMUT ARAYÜZÜ ──
E.append(H2("14.4 Komut Arayüzü: USB ve Telefondan Cihazı Sürme (command_service)"))
E.append(P("Cihaz, dışarıdan metin komutlarıyla yönetilebilir — test, demo, otomasyon ve uzaktan "
           "kontrol için. Tek bir <b>taşıyıcı-bağımsız parser çekirdeği</b> "
           "(<font face='Mono'>command_service_exec_ex</font>) iki kanaldan beslenir; komutlar yeni "
           "ekran kodu içermez, mevcut iç API'lere köprüdür:"))
E.append(C("""USB seri (UART0 RX) ──┐        yanıtlar reply-sink ile geri döner:
BLE "NarRTOS CMD"   ──┴─► parser ──► USB'den gelen -> seriye (printf)
                                     BLE'den gelen -> telefona (notify)"""))
E.append(H2("Komut Seti"))
E.append(TBL(["Komut", "Etkisi"],
    [["help / info", "Komut listesi / durum JSON'u (sürüm, uptime, heap, wifi, aktif uygulama)"],
     ["app launch &lt;isim|id&gt;", "Uygulama açar (EVENT_APP_SWITCH ile — LVGL task'ında, thread-safe)"],
     ["app back / current / list", "Geri • aktif uygulama • isim→id tablosu"],
     ["swipe left|right|up|down", "Jest enjeksiyonu (g_swipe_dir) — ekranlar arası gezinme"],
     ["brightness 0-255", "Ekran parlaklığı"],
     ["set wifi|bt on|off", "WiFi tercihi (NVS, kalıcı) + bağlan/kopar • BLE advertise/kes"],
     ["get wifi|bt / toggle wifi|bt", "Durum okuma / tersine çevirme"],
     ["set rail &lt;isim&gt; on|off / get rails", "AXP2101 güç rayları (aldo1-4, dcdc2-4, bldo1-2) — "
      "PMIC'siz board'da dürüst hata; DCDC1 (sistem rayı) bilinçli kapalı"],
     ["wake / reboot", "Ekranı uyandır / yeniden başlat"]],
    [52 * mm, 118 * mm]))
E.append(H2("USB Kullanımı"))
E.append(P("Seri terminale (115200) satır yaz: <font face='Mono'>pio device monitor</font> açıkken "
           "<font face='Mono'>help</font> yazmak yeterli. Yanıtlar <font face='Mono'>[cmd]</font> "
           "önekiyle döner. <b>Önemli:</b> macOS'ta CH34x köprüsü portu her açışta cihaza reset "
           "atar — portu bir kez açıp aynı oturumda çalışın (her komut için yeniden bağlanmayın)."))
E.append(H2("Telefon Kullanımı (BLE — özel uygulama gerekmez)"))
E.append(C("""GATT servisi "NarRTOS CMD":
  Service : 4E010063-6D64-004E-4152-52544F530001
  RX      : ...530002  (Write / WriteNoRsp)  <- UTF-8 komut satırı yaz
  TX      : ...530003  (Notify)              <- yanıtlar buraya düşer

nRF Connect ile: "NarRTOS Watch"a bağlan -> ...530003'te Notify aç ->
...530002'ye UTF-8 text yaz: "app launch weather" -> saatte uygulama açılır,
yanıt notify'da: [cmd] OK: launch weather (id=6)"""))
E.append(P("BLE'de USB'deki port-reset sorunu yoktur; cihaz koldayken bile komut gönderilebilir. "
           "Uyarı: <font face='Mono'>set bt off</font> komutu telefondan gönderilirse kendi "
           "bağlantısını da keser (komut yine de çalışır). Eşleştirme bozulursa (iOS 'Peer removed "
           "pairing information'): telefonda cihazı unut → yeniden bağlan."))
E.append(P("<b>Dosyalar:</b> <font face='Mono'>services/command_service.{h,cpp}</font> (çekirdek + "
           "USB), <font face='Mono'>services/ble_cmd_service.{h,cpp}</font> (GATT taşıyıcı). "
           "Şimdilik LCD128 varyantında etkin; kararlılık sonrası ortaklaştırılır. Yeni taşıyıcı "
           "(ör. HTTP) eklemek = satırı <font face='Mono'>command_service_exec_ex</font>'e besleyen "
           "küçük bir adaptör."))
E.append(PageBreak())

# ═══ 15. BOARD TAŞIMA REHBERİ ═══
E.append(H1("15. Board Taşıma (Porting) Rehberi"))
E.append(RULE())
E.append(P("NarRTOS'u yeni bir donanıma taşımak, varyant kalıbı (Bölüm 8/10) sayesinde uygulama ve "
           "servis koduna dokunmadan yapılır. Aşağıdaki adımlar, gerçekte yapılmış bir taşımanın "
           "(1.28\" yuvarlak LCD board'u — <font face='Mono'>esp32s3_lcd128</font>) izlediği yolun "
           "aynısıdır."))
E.append(H2("Adım 1 — Ekran sürücüsü: drivers/display/&lt;yeni&gt;_driver.cpp"))
E.append(P("Varsayılan sürücünün dışa verdiği <b>aynı fonksiyon setini</b> "
           "(<font face='Mono'>co5300_init, co5300_lvgl_flush_cb, ...</font>) uygula; dosyanın "
           "tamamını kendi guard'ına al (<font face='Mono'>#ifdef DISPLAY_VARIANT_X</font>). "
           "Çözünürlüğü sabit yazma — <font face='Mono'>CO5300_H_RES/V_RES</font> bayraklarından al."))
E.append(H2("Adım 2 — Dokunmatik sürücüsü: drivers/touch/&lt;yeni&gt;_driver.cpp"))
E.append(P("Aynı kalıp: varsayılan dokunmatik sürücüsünün fonksiyon imzaları + guard."))
E.append(H2("Adım 3 — Pin haritası: core/os_config_&lt;yeni&gt;_pins.h"))
E.append(P("Yeni board'un pinleri ayrı başlıkta tanımlanır; <font face='Mono'>os_config.h</font>, "
           "board guard'ına göre doğru başlığı seçer."))
E.append(H2("Adım 4 — Build ortamı: platformio.ini"))
E.append(C("""[env:esp32s3_yeniboard]
extends = env:esp32s3_wearable        ; ortak her şeyi miras al
board_build.flash_size = 16MB         ; donanıma göre
board_build.psram_type = qio          ; donanıma göre
upload_speed = 460800                 ; USB köprüsü kararsızsa düşür
build_flags =
    ${env:esp32s3_wearable.build_flags}
    -DCO5300_H_RES=240 -DCO5300_V_RES=240
    -DDISPLAY_VARIANT_YENI=1
    -DBOARD_VARIANT_YENI=1"""))
E.append(H2("Adım 5 — ESP-IDF konfigürasyonu: sdkconfig.esp32s3_yeniboard"))
E.append(P("PSRAM tipi (quad/octal), flash boyutu gibi çip-seviyesi ayarlar env'e özel sdkconfig "
           "dosyasında tutulur."))
E.append(H2("Adım 6 — UI ölçü sabitleri"))
E.append(P("Farklı çözünürlükte taşan ekranlar için ölçü sabitlerinin (font boyu, kart aralığı, "
           "buton ölçüsü) varyant değerleri tanımlanır — uygulama kodu değişmez, yalnızca sabitler "
           "board guard'ıyla seçilir."))
E.append(H2("Kontrol Listesi"))
E.append(B("[ 1 ] Ekran + dokunmatik sürücü çifti aynı sembol setiyle, kendi guard'ında"))
E.append(B("[ 2 ] Pin başlığı + platformio env + sdkconfig"))
E.append(B("[ 3 ] Her iki ortam da derleniyor; VARSAYILAN build'ın binary'si DEĞİŞMEDİ "
           "(taşıma, mevcut donanımı asla etkilememeli)"))
E.append(B("[ 4 ] Boot logları temiz; dokunmatik/ekran/servisler cihazda doğrulandı"))

# ═══ 16. SORUN GİDERME ═══
E.append(H1("16. Sorun Giderme"))
E.append(RULE())
E.append(TBL(["Belirti", "Neden / Çözüm"],
    [["Yükleme sırasında checksum hatası ('Failed to write to target RAM')",
      "Bazı USB-seri köprüler 921600'de kararsızdır → upload_speed'i 460800'e düşürün"],
     ["Seri monitörde hiç çıktı yok (0 bayt)",
      "Okuma aracı DTR/RTS hatlarını yanlış sürüyor olabilir → portu DTR=false ile açıp RTS ile "
      "reset darbesi verin (cihaz normal boot eder)"],
     ["Boot'ta dokunmatik I2C hatası",
      "Normal olabilir — cihaz geç hazırlanıyor; sürücü yine de kaydedilir ve ilk dokunuşla toparlar"],
     ["WiFi açılışta bağlanmıyor",
      "Kullanıcı tercihi NVS'te OFF olabilir (bilinçli davranış) — ayarlardan açılır"],
     ["Ekran açık ama görüntü yok",
      "Parlaklık ramp'i başlamadan hata olmuş olabilir; boot loglarında co5300/ui_service satırlarına bakın"]],
    [72 * mm, 98 * mm]))
E.append(P("Seri port susma sorununun çözümü — DTR/RTS'i doğru sürüp temiz reset atan okuma "
           "(pyserial):"))
E.append(C("""import serial, time
ser = serial.Serial()
ser.port, ser.baudrate, ser.timeout = "/dev/cu.usbserial-XXXX", 115200, 1
ser.dtr = False; ser.rts = False      # açılışta hatları ASSERT ETME
ser.open()
ser.setDTR(False)                     # GPIO0 = high -> normal boot
ser.setRTS(True);  time.sleep(0.15)   # EN = low  -> reset'te tut
ser.setRTS(False)                     # bırak -> cihaz app'i boot eder
while True:
    print(ser.readline().decode("utf-8", "replace"), end="")"""))
E.append(Spacer(1, 8))
E.append(RULE())
E.append(P("<b>Bu doküman temel sistemi, etkileşim modelini ve yerleşik uygulamaları kapsar.</b> "
           "Servis eklentileri (ör. telemetri ve konumlandırma) ve backend bileşenleri ayrı bölümler "
           "olarak bu dokümantasyonun üzerine eklenecektir."))

# ═══ SÜRÜM BİLGİSİ ═══
E.append(Spacer(1, 14))
E.append(H1("Sürüm Bilgisi"))
E.append(RULE())
E.append(TBL(["Alan", "Değer"],
    [["NarRTOS sürümü", "v1.4.81  (OS_VERSION_CODE: 10481)"],
     ["Sürüm kaynağı", "core/os_config.h — OS_VERSION_MAJOR/MINOR/PATCH"],
     ["Doküman sürümü", "1.3 — 29 Temmuz 2026 (+ komut arayüzü: USB ve BLE ile cihazı sürme)"],
     ["Doğrulanan ortamlar", "esp32s3_wearable (AMOLED) • esp32s3_lcd128 (LCD 1.28\")"],
     ["Araç zinciri", "PlatformIO Core 6.x — ESP-IDF 5.5.x — LVGL 9"]],
    [48 * mm, 122 * mm]))
E.append(CAP("Bu doküman NarRTOS v1.4.81 kod tabanından üretilmiştir; boot akışı, pin haritası, "
             "jest davranışları ve API imzaları bu sürümün kaynak kodundan birebir alınmıştır. "
             "Yeni sürümlerde değişen davranışlar için dokümanın güncel revizyonuna başvurun."))

doc.build(E, onFirstPage=footer, onLaterPages=footer)
print("OK:", OUT)
