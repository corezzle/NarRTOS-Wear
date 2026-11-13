#!/usr/bin/env python3
"""
nar_olcum.py: etiketli ölçüm seansı (kedi / köpek / insan). Her ölçümde:
  1) kart `mic nabiz <sn> 3 24 0 <tür> usb` ile kaydeder, KARTTA analiz eder, ham sesi USB'den de akıtır
     (kart takılıysa ham WAV ayrıca SD'ye yazılır),
  2) HAM kayıt (işlenmemiş, 48 kHz 24 bit) + dinleme kopyası (MP3) bilgisayara yazılır,
  3) aynı ham kayıt bilgisayarda referans algoritmayla (tools/nabiz_ref.py) yeniden hesaplanır; kartla karşılaştırılır
     (nabız, solunum; kedide mırlama, köpekte soluma tespiti kartta yapılır ama "durum" yine sorulur: doğrulama etiketi),
  4) tür, hayvan, kilo, yerleşim, durum ve VETERİNERİN SAYDIĞI referans nabız/solunum ile birlikte
     olcumler.csv dosyasına bir satır eklenir.
Port seans boyunca açık kalır: kart yalnız ilk açılışta yeniden başlar (açılış testi ~30-45 sn), sonraki ölçümler hemen.

Kullanım:
    python3 tools/nar_olcum.py                              sorarak (önerilen)
    python3 tools/nar_olcum.py --tur kedi --hayvan Pamuk --kilo 4.2 --yer sol-gogus --durum sakin --sure 30
    python3 tools/nar_olcum.py ... --sormadan --ref-nabiz 180 --ref-solunum 28     tek ölçüm, soru yok

Çıktı klasörü: $KAYIT_DIZINI ya da ~/Desktop/NAR-kayitlar/olcum/
    <tarih>_<saat>_<tür>_<hayvan>.wav   ham kayıt (algoritma geliştirme ve doğrulama bunun üzerinden yapılır; SİLMEYİN)
    <tarih>_<saat>_<tür>_<hayvan>.mp3   dinleme kopyası (ilk 1 sn atılmış, DC süzülmüş, seviyesi yükseltilmiş)
    olcumler.csv                        her ölçüm bir satır

Gerekenler: pyserial (zorunlu), numpy (referans hesap ve dinleme kopyası için), ffmpeg (MP3 için; yoksa WAV kalır).
Çerçeve biçimi (kart → USB): "NARA" + seq(u32) + len(u16) + tasma(u16) + toplam(u32) + veri; tools/mic_raw.py ile aynı.
"""
import argparse, csv, glob, os, re, struct, subprocess, sys, time, wave

FS = 48000
BIT = 24
TURLER = {"k": "kedi", "kedi": "kedi", "p": "kopek", "kopek": "kopek", "köpek": "kopek", "i": "insan", "insan": "insan"}
YERLER = ["sol-gogus", "sag-gogus", "boyun", "sirt", "diger"]
DURUMLAR = ["uyku", "sakin", "uyanik", "soluma", "mirlama", "huzursuz"]
SUTUNLAR = ["zaman", "tur", "hayvan", "kilo_kg", "yer", "durum_hayvan", "sure_sn", "ref_nabiz", "ref_solunum",
            "cihaz_durum", "cihaz_nabiz", "cihaz_r", "cihaz_r2", "cihaz_yontem", "cihaz_solunum", "cihaz_rs", "cihaz_sdurum",
            "cihaz_mirlama", "cihaz_soluma", "cihaz_belirgin", "mac_durum", "mac_nabiz", "mac_r", "mac_solunum", "mac_sdurum", "uyum",
            "tepe_db", "seviye_db", "sseviye_db", "doygun", "hareket_mg", "usb_kayip", "usb_bozuk", "tasma", "sd_dosya",
            "ham_wav", "dinleme", "alg", "not"]
_TR = str.maketrans("çğıöşüÇĞİÖŞÜ", "cgiosuCGIOSU")


def ascii_tr(s): return s.translate(_TR).encode("ascii", "ignore").decode()
def dosya_adi(s): return re.sub(r"[^A-Za-z0-9_-]+", "-", ascii_tr(s)).strip("-") or "x"


# ───────────────────────── seri port ─────────────────────────
def port_ac(bekle_acilis=60.0):
    """Kartı bulur, portu açar, uygulama komut almaya hazır olana kadar bekler. Dönüş: açık serial nesnesi."""
    import serial
    t0 = time.time(); ports = []
    while not ports and time.time() - t0 < 60:
        ports = glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/ttyACM*")
        if not ports: time.sleep(0.3)
    if not ports: sys.exit("USB portu yok: kart takılı mı? (Monitör/başka program portu tutuyorsa kapatın.)")
    p = serial.Serial(); p.port = ports[0]; p.baudrate = 115200; p.timeout = 0.05; p.dtr = False; p.rts = False; p.open()
    print(f"port {ports[0]}", flush=True)
    buf = b""; t0 = time.time(); acilis = False
    while time.time() - t0 < 6:                       # port açılınca kart çoğu zaman yeniden başlar
        buf += p.read(8192)
        if b"ESP-ROM" in buf or b"rst:" in buf: acilis = True; break
    bitti = False
    if acilis:
        print("kart yeniden başladı; açılış testi bekleniyor (30-45 sn)...", flush=True)
        t0 = time.time()
        while time.time() - t0 < bekle_acilis:
            buf += p.read(65536)
            # Yalnız YENİ açılıştan sonraki özet sayılır: port açılınca kartın tamponunda kalmış ESKİ açılışın
            # "PASS n FAIL n" satırı da gelir (2026-11-10: bu yüzden ölçüm açılış testinin ortasında başlamıştı).
            son = max(buf.rfind(b"ESP-ROM"), buf.rfind(b"rst:"))
            if re.search(rb"PASS\s+\d+\s+FAIL\s+\d+", buf[son:]): bitti = True; break
        if not bitti and b"CmdSvc" not in buf[max(buf.rfind(b"ESP-ROM"), 0):]:
            p.close(); sys.exit("kart açılmadı (CmdSvc görülmedi). USB'yi çıkarıp takın, tekrar deneyin.")
        time.sleep(2.0)
    # hazır mı: info. Açılış testi USB'ye metin basarken ölçüm bozulur; bitişi görülmediyse çalışma süresine göre bekle.
    for _ in range(4):
        satirlar = komut(p, "info", 3.0)
        m = next((re.search(r'"uptime_s":(\d+)', x) for x in satirlar if '"uptime_s"' in x), None)
        if m:
            up = int(m.group(1))
            if up < 50 and not bitti:
                print(f"kart {up} sn'dir açık; açılış testinin bitmesi bekleniyor ({50 - up} sn)...", flush=True)
                t0 = time.time(); buf = b""
                while time.time() - t0 < 50 - up:
                    buf += p.read(65536)
                    if re.search(rb"PASS\s+\d+\s+FAIL\s+\d+", buf): time.sleep(2.0); break
                satirlar = komut(p, "info", 3.0)
            print(f"kart hazır ({next((x for x in satirlar if 'uptime_s' in x), '')[:90]})", flush=True)
            return p
        time.sleep(2.0)
    p.close(); sys.exit("kart komutlara cevap vermiyor: ürün yazılımı yüklü mü? (pio run -e nar_esp32s3 + tools/nar_yukle.sh, sonra USB çıkar-tak)")


def komut(p, metin, sure=3.0):
    """Komut gönderir; '[cmd]' ile başlayan bir satır gelene ya da süre dolana kadar okur. Dönüş: [mic]/[cmd] satırları."""
    p.reset_input_buffer(); p.write((metin + "\r\n").encode()); p.flush()
    buf = b""; t0 = time.time()
    while time.time() - t0 < sure:
        buf += p.read(8192)
        if re.search(rb"\[cmd\][ -~]*\r?\n", buf): time.sleep(0.15); buf += p.read(8192); break
    return [m.decode("latin1") for m in re.findall(rb"\[(?:mic|cmd)\][ -~]*", buf)]


def kayit_al(p, sure, tur, kanal=3, varyant=0, sd=True):
    """Ölçümü başlatır, akışı toplar. Dönüş: (ham baytlar, bitti mi, hata metni ya da None)."""
    import serial
    p.reset_input_buffer()
    p.write(f"mic nabiz {sure} {kanal} {BIT} {varyant} {tur} usb{'' if sd else ' nosd'}\r\n".encode()); p.flush()
    raw = bytearray(); t0 = time.time(); son = t0; bitti = False; hata = None
    while time.time() - t0 < sure + 45:
        try: d = p.read(65536)
        except (serial.SerialException, OSError) as e:
            hata = f"kart USB'den düştü ({e})"; break
        if d: raw += d
        kuyruk = bytes(raw[-900:])
        if b"[cmd] OK: mic nabiz" in kuyruk or b"[cmd] ERR" in kuyruk: bitti = True; break
        if time.time() - son >= 5:
            print(f"   {int(time.time() - t0):3d} sn  {len(raw) // 1024} kB", flush=True); son = time.time()
    return bytes(raw), bitti, hata


# ───────────────────────── akış çözme ─────────────────────────
def coz(raw):
    """Çerçeveleri PCM'e çevirir. Dönüş: (pcm baytları, istatistik sözlüğü, cihaz metin satırları)."""
    w = BIT // 8; pcm = bytearray(); i = 0; beklenen = 0; ilk = True
    st = {"cerceve": 0, "kayip": 0, "bozuk": 0, "tasma": 0}
    while True:
        i = raw.find(b"NARA", i)
        if i < 0 or i + 16 > len(raw): break
        seq, ln, ov, top = struct.unpack_from("<IHHI", raw, i + 4)
        if ln > 960 or i + 16 + ln > len(raw): i += 4; continue
        v = raw[i + 16:i + 16 + ln]
        if sum(v) != top: st["bozuk"] += 1; i += 4; continue
        if ilk: beklenen = seq; ilk = False
        if seq > beklenen: st["kayip"] += seq - beklenen; pcm += bytes(960 * (seq - beklenen))   # eksik çerçeve = sessizlik
        pcm += v; beklenen = seq + 1; st["tasma"] = ov; st["cerceve"] += 1; i += 16 + ln
    satirlar = [m.decode("latin1") for m in re.findall(rb"\[(?:mic|cmd)\][ -~]*", raw)]
    return bytes(pcm[:len(pcm) // w * w]), st, satirlar


def sonuc_coz(satirlar):
    """'[mic] sonuc: a=b c=d ... etiket=serbest metin' → sözlük (yoksa boş)."""
    s = next((x for x in satirlar if x.startswith("[mic] sonuc:")), None)
    if not s: return {}
    govde = s[len("[mic] sonuc:"):].strip(); etiket = ""
    if " etiket=" in govde: govde, etiket = govde.split(" etiket=", 1)
    elif govde.startswith("etiket="): govde, etiket = "", govde[7:]
    d = dict(t.split("=", 1) for t in govde.split() if "=" in t); d["etiket"] = etiket.strip()
    return d


def eski_sonuc_coz(satirlar):
    """Özet ('[mic] sonuc:') satırı basmayan eski kart yazılımı için: '[mic] nabiz: ...' satırından sonuç çıkarır.
    Dönüş sözlüğünde alg='eski'. Eski yazılım tür profili bilmez (insan ayarı), solunum ve mırlama/soluma hesaplamaz."""
    for x in satirlar:
        m = re.search(r"\[mic\] nabiz: (\d+)/dk\s+guven r=(-?[\d.]+)", x)
        if m:
            r2 = re.search(r"yari-periyot r=(-?[\d.]+)", x)
            return {"alg": "eski", "durum": "OK", "nabiz": m.group(1), "r": m.group(2), "r2": r2.group(1) if r2 else "", "yontem": "ac"}
        m = re.search(r"\[mic\] nabiz: BELIRSIZ.*?r=(-?[\d.]+).*?aday (\d+)/dk", x)
        if m: return {"alg": "eski", "durum": "BELIRSIZ", "nabiz": m.group(2), "r": m.group(1), "r2": "", "yontem": "ac"}
    return {}


# ───────────────────────── dosyalar ─────────────────────────
def wav_yaz(yol, pcm, bit=BIT):
    with wave.open(yol, "wb") as f:
        f.setnchannels(1); f.setsampwidth(bit // 8); f.setframerate(FS); f.writeframes(pcm)


def dinleme_kopyasi(pcm, taban):
    """İlk 1 sn atılır, 15 Hz altı (DC) süzülür, tepe -1 dBFS'e çekilir. MP3 (ffmpeg) ya da WAV yolu; numpy yoksa None."""
    try: import numpy as np
    except ImportError: return None
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import nabiz_ref
    x = nabiz_ref.pcm_coz(pcm, BIT).astype(np.float64)[FS:]
    if len(x) < FS: return None
    X = np.fft.rfft(x); F = np.fft.rfftfreq(len(x), 1.0 / FS); X[F < 15.0] = 0; y = np.fft.irfft(X, len(x))
    tepe = float(np.abs(y).max()) or 1.0
    b = np.round(y * (0.891 * (2 ** 23 - 1) / tepe)).astype(np.int32)
    o = np.zeros((len(b), 3), dtype=np.uint8); o[:, 0] = b & 255; o[:, 1] = (b >> 8) & 255; o[:, 2] = (b >> 16) & 255
    gecici = taban + "_dinleme.wav"; wav_yaz(gecici, o.tobytes())
    mp3 = taban + ".mp3"
    try:
        r = subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", gecici, "-ac", "1",
                            "-c:a", "libmp3lame", "-b:a", "320k", mp3])
        if r.returncode == 0: os.remove(gecici); return mp3
    except FileNotFoundError: pass
    return gecici


def referans(pcm, tur):
    """Aynı ham kayıt üzerinde bilgisayar referansı (nabiz_ref). numpy yoksa None."""
    try: import numpy  # noqa: F401
    except ImportError: return None
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import nabiz_ref
    return nabiz_ref.analiz(nabiz_ref.pcm_coz(pcm, BIT), BIT, tur)


def csv_ekle(yol, satir):
    """Satırı ekler. Dosya eski sütun düzenindeyse (önceki alg sürümü) önce yeni düzene taşır; eski satırlar korunur."""
    eski = []; sutun = list(SUTUNLAR)
    if os.path.exists(yol):
        with open(yol, newline="", encoding="utf-8-sig") as f:
            r = csv.DictReader(f); baslik = r.fieldnames or []
            if baslik != SUTUNLAR:
                eski = list(r); sutun += [k for k in baslik if k not in sutun]
            else: sutun = None
    if sutun is None:
        with open(yol, "a", newline="", encoding="utf-8") as f:
            csv.DictWriter(f, fieldnames=SUTUNLAR).writerow({k: satir.get(k, "") for k in SUTUNLAR})
        return
    gecici = yol + ".yeni"
    with open(gecici, "w", newline="", encoding="utf-8-sig") as f:
        w = csv.DictWriter(f, fieldnames=sutun); w.writeheader()
        for e in eski: w.writerow({k: e.get(k, "") for k in sutun})
        w.writerow({k: satir.get(k, "") for k in sutun})
    os.replace(gecici, yol)


# ───────────────────────── etkileşim ─────────────────────────
def sor(soru, varsayilan="", secenekler=None):
    ek = f" [{varsayilan}]" if varsayilan != "" else ""
    while True:
        try: c = input(f"{soru}{ek}: ").strip()
        except EOFError: c = ""
        if not c: c = str(varsayilan)
        if secenekler is None: return c
        if c.isdigit() and 1 <= int(c) <= len(secenekler): return secenekler[int(c) - 1]
        if c in secenekler: return c
        print("   seçenekler: " + ", ".join(f"{i + 1}={s}" for i, s in enumerate(secenekler)))


def sayi_ya_da_bos(s):
    s = (s or "").replace(",", ".").strip()
    try: float(s); return s
    except ValueError: return ""


def olcum(p, a, dizin):
    """Tek ölçüm: kayıt, çözme, dosyalar, karşılaştırma, CSV. a: ölçüm parametreleri sözlüğü. Dönüş: CSV satırı."""
    tur = a["tur"]; sure = int(a["sure"])
    etiket = ascii_tr(f"{tur} {a['hayvan']} {a['kilo']}kg {a['yer']} {a['durum']}")[:63]
    komut(p, f"mic etiket {etiket}", 2.0)
    zaman = time.strftime("%Y-%m-%d %H:%M:%S")
    taban = os.path.join(dizin, f"{time.strftime('%Y-%m-%d_%H-%M-%S')}_{tur}_{dosya_adi(a['hayvan'])}")
    print(f"\n>>> KAYIT BAŞLADI: {sure} sn, profil {tur}. Mikrofonu sabit tutun, konuşmayın.", flush=True)
    raw, bitti, hata = kayit_al(p, sure, tur, sd=not a.get("nosd"))
    pcm, st, satirlar = coz(raw)
    n = len(pcm) // 3
    print(f">>> KAYIT BİTTİ: çerçeve {st['cerceve']}, kayıp {st['kayip']}, bozuk {st['bozuk']}, tampon taşması {st['tasma']}, "
          f"{n / FS:.1f} sn", flush=True)
    if hata: print("!!! " + hata, flush=True)
    for s in satirlar:
        if s.startswith(("[mic] nabiz", "[mic] pencereler", "[mic] solunum", "[mic] kalite", "[mic] SD", "[mic] I2S", "[mic] bellek yok", "[cmd] ERR")):
            print("cihaz: " + s, flush=True)
    c = sonuc_coz(satirlar); eski = False
    if not c:
        c = eski_sonuc_coz(satirlar); eski = bool(c)
    if eski:
        print("not: karttaki yazılım eski sürüm. Kartın sayısı insan ayarıyla hesaplandı; tür profili, solunum ve "
              "mırlama/soluma bu bilgisayarda hesaplanıyor (aşağıdaki sonuç).", flush=True)
    if not c:                                           # sonuç yok: kartın söylediği her şeyi göster (tanı)
        for x in satirlar[-12:]:
            if not x.startswith(("[mic] nabiz", "[mic] pencereler", "[mic] solunum", "[mic] kalite", "[mic] SD", "[mic] I2S", "[mic] bellek yok", "[cmd] ERR")):
                print("cihaz: " + x[:200], flush=True)
    if not bitti and not hata: print("!!! kart ölçümü bitirmedi (zaman aşımı)", flush=True)
    if not c: print("!!! karttan hiçbir nabız satırı gelmedi (kart ölçümü tamamlayamadı). Ham kayıt alındıysa bilgisayarda hesaplanacak.", flush=True)

    satir = {"zaman": zaman, "tur": tur, "hayvan": a["hayvan"], "kilo_kg": a["kilo"], "yer": a["yer"], "durum_hayvan": a["durum"],
             "sure_sn": sure, "cihaz_durum": c.get("durum", "YOK"), "cihaz_nabiz": c.get("nabiz", ""), "cihaz_r": c.get("r", ""),
             "cihaz_r2": c.get("r2", ""), "cihaz_yontem": c.get("yontem", ""), "cihaz_solunum": c.get("solunum", ""), "cihaz_rs": c.get("rs", ""),
             "cihaz_sdurum": c.get("sdurum", ""), "cihaz_mirlama": c.get("mirlama", ""), "cihaz_soluma": c.get("soluma", ""),
             "cihaz_belirgin": c.get("belirgin", ""), "sseviye_db": c.get("sseviye", ""),
             "tepe_db": c.get("tepe", ""), "seviye_db": c.get("seviye", ""), "doygun": c.get("doygun", ""),
             "hareket_mg": c.get("hareket", ""), "usb_kayip": st["kayip"], "usb_bozuk": st["bozuk"], "tasma": st["tasma"],
             "sd_dosya": c.get("sd", ""), "alg": c.get("alg", "")}
    m = None
    if n >= FS * 2:
        wav_yaz(taban + ".wav", pcm); satir["ham_wav"] = os.path.basename(taban + ".wav")
        d = dinleme_kopyasi(pcm, taban)
        if d: satir["dinleme"] = os.path.basename(d)
        m = referans(pcm, tur)
        if m:
            satir.update(mac_durum=m["durum"], mac_nabiz=f"{m['nabiz']:.0f}", mac_r=f"{m['r']:.2f}",
                         mac_solunum=f"{m['solunum']:.0f}", mac_sdurum=m["sdurum"])
            mac_metin = (f"nabız {m['durum']} {m['nabiz']:.0f}/dk r={m['r']:.2f}"
                         + (f" ({m['yontem']} kuralı, düşük güven)" if m["durum"] == "OK" and m["yontem"] in ("pencere", "tutarli") else "")
                         + f", solunum {m['sdurum']}"
                         + (f" {m['solunum']:.0f}/dk" if m["sdurum"] == "OK" else ""))
            if eski:
                satir["uyum"] = "ESKI_YAZILIM"
                print(f"Bilgisayar (alg {m['alg']}, profil {tur}): {mac_metin}", flush=True)
            elif c and c.get("alg") != m["alg"]:
                satir["uyum"] = "ALG_FARKLI"
                print(f"Bilgisayar (alg {m['alg']}, profil {tur}): {mac_metin}", flush=True)
                print(f"UYARI: kart alg {c.get('alg')}, bilgisayar alg {m['alg']}: kart yazılımı ile depo aynı sürümde değil; "
                      f"özet bilgisayarın sonucundan verilir.", flush=True)
            elif c:
                sayi = lambda k: float(c.get(k, 0) or 0)
                ayni = (c.get("durum") == m["durum"] and abs(sayi("nabiz") - m["nabiz"]) <= 1
                        and c.get("sdurum", m["sdurum"]) == m["sdurum"] and abs(sayi("solunum") - m["solunum"]) <= 1)
                satir["uyum"] = "UYUM" if ayni else "FARK"
                print(f"Mac referansı: {mac_metin}  →  kartla {'UYUMLU' if ayni else 'FARKLI !!!'}"
                      + ("" if ayni or not (st["kayip"] or st["bozuk"]) else "  (USB akışında kayıp var; fark bundan olabilir)"), flush=True)
            else:
                print(f"Bilgisayar (alg {m['alg']}, profil {tur}): {mac_metin}", flush=True)
        else:
            print("(Mac referansı ve dinleme kopyası için numpy gerekli: python3 -m pip install --user --break-system-packages numpy)")
        print(f"ham kayıt : {taban}.wav", flush=True)
        if d: print(f"dinleme   : {d}", flush=True)
    else:
        print("!!! ses verisi yok ya da çok kısa; dosya yazılmadı", flush=True)
    if st["kayip"] or st["bozuk"]:
        print(f"UYARI: USB akışında {st['kayip']} kayıp / {st['bozuk']} bozuk çerçeve. Bilgisayardaki ham kayıt eksik; "
              f"tam kayıt SD kartta ({c.get('sd', 'yok')}).", flush=True)

    # özet: kart güncel ve bilgisayarla aynı alg sürümündeyse kartın sonucu; eski/farklı sürümse (ya da karttan sonuç
    # gelmediyse) aynı ham kayıt üzerinde bilgisayarın hesapladığı sonuç (tür profili + solunum burada var).
    oz = c; ek = ""
    if m and (eski or not c or c.get("alg") != m["alg"]):
        oz = {"durum": m["durum"], "nabiz": f"{m['nabiz']:.0f}", "r": f"{m['r']:.2f}", "yontem": m["yontem"], "nvurus": m["nvurus"],
              "pen_n": m["pen_n"], "pen_iyi": m["pen_iyi"], "pen_uyan": m["pen_uyan"], "r2t": f"{m['r2t']:.2f}",
              "nabiz_ac": f"{m['nabiz_ac']:.0f}", "nabiz_vurus": f"{m['nabiz_vurus']:.0f}", "sdurum": m["sdurum"],
              "solunum": f"{m['solunum']:.0f}", "rs": f"{m['rs']:.2f}"}
        ek = "; bilgisayarda hesaplandı"
    drm = oz.get("durum", "YOK"); sd = oz.get("sdurum", "")
    if drm == "OK" and oz.get("yontem") == "vurus":
        print(f"\n=== NABIZ: {oz.get('nabiz')} /dk  (ritim düzensiz; vuruş sayımıyla, {oz.get('nvurus')} vuruş, profil {tur}{ek}) ===", flush=True)
    elif drm == "OK" and oz.get("yontem") == "pencere":
        print(f"\n=== NABIZ: {oz.get('nabiz')} /dk  DÜŞÜK GÜVEN  (pencere kuralı: {oz.get('pen_iyi')}/{oz.get('pen_n')} pencere net; nabız kayıt içinde değişmiş "
              f"ya da bir bölüm gürültülü; bütün kayıt r={oz.get('r')}, profil {tur}{ek}) ===", flush=True)
    elif drm == "OK" and oz.get("yontem") == "tutarli":
        print(f"\n=== NABIZ: {oz.get('nabiz')} /dk  DÜŞÜK GÜVEN  (tutarlılık kuralı: {oz.get('pen_uyan')}/{oz.get('pen_n')} pencere aynı ritimde; "
              f"ses zayıf/hışırtılı, r={oz.get('r')}, profil {tur}{ek}) ===", flush=True)
    elif drm == "OK": print(f"\n=== NABIZ: {oz.get('nabiz')} /dk  (güven r={oz.get('r')}, profil {tur}{ek}) ===", flush=True)
    elif drm == "BELIRSIZ": print(f"\n=== NABIZ: BELİRSİZ (r={oz.get('r')}, aday {oz.get('nabiz_ac', oz.get('nabiz'))}/dk{ek}). Teması düzeltip tekrar ölçün. ===", flush=True)
    elif drm == "KAT_SUPHESI": print(f"\n=== NABIZ: BELİRSİZ (iki yöntem yarı/çift hızda ayrışıyor: {oz.get('nabiz_ac')} ve {oz.get('nabiz_vurus')}{ek}). Tekrar ölçün. ===", flush=True)
    elif drm == "MIRLAMA": print(f"\n=== NABIZ: ÖLÇÜLEMEDİ (kedi mırlıyor{ek}). Mırlama bitince tekrar ölçün. ===", flush=True)
    elif drm == "YOK": print("\n=== SONUÇ: karttan sonuç alınamadı (ham kayıt varsa saklandı) ===", flush=True)
    else: print(f"\n=== NABIZ: GEÇERSİZ ({drm}{ek}) ===", flush=True)
    if sd == "OK": print(f"=== SOLUNUM: {oz.get('solunum')} /dk  (güven r={oz.get('rs')}) ===", flush=True)
    elif sd == "SOLUMA": print("=== SOLUNUM: hayvan soluyor; dinlenme solunumu ölçülemez ===", flush=True)
    elif sd == "KISA": print("=== SOLUNUM: ölçülemedi (kayıt en az 21 sn olmalı) ===", flush=True)
    elif sd: print(f"=== SOLUNUM: ölçülemedi ({sd}) ===", flush=True)
    return satir


def main():
    ap = argparse.ArgumentParser(description="NAR etiketli ölçüm seansı (kedi/köpek/insan)")
    ap.add_argument("--tur"); ap.add_argument("--hayvan"); ap.add_argument("--kilo"); ap.add_argument("--yer"); ap.add_argument("--durum")
    ap.add_argument("--sure", type=int); ap.add_argument("--ref-nabiz", dest="ref_nabiz"); ap.add_argument("--ref-solunum", dest="ref_solunum")
    ap.add_argument("--not", dest="not_"); ap.add_argument("--nosd", action="store_true", help="karttaki SD'ye yazma")
    ap.add_argument("--sormadan", action="store_true", help="hiç soru sorma, tek ölçüm yap (eksik alanlar boş kalır)")
    g = ap.parse_args()
    dizin = os.environ.get("KAYIT_DIZINI") or os.path.expanduser("~/Desktop/NAR-kayitlar/olcum"); os.makedirs(dizin, exist_ok=True)
    csv_yol = os.path.join(dizin, "olcumler.csv")
    try: import serial  # noqa: F401
    except ImportError: sys.exit("pyserial gerekli: python3 -m pip install --user --break-system-packages pyserial")

    son = {"tur": TURLER.get((g.tur or "").lower(), ""), "hayvan": g.hayvan or "", "kilo": sayi_ya_da_bos(g.kilo),
           "yer": g.yer or "", "durum": g.durum or "", "sure": g.sure or 0, "nosd": g.nosd}
    if g.tur and not son["tur"]: sys.exit("--tur: kedi | kopek | insan")
    p = port_ac()
    try:
        while True:
            if not g.sormadan:
                print("\n── yeni ölçüm ──────────────────────────────────────────────")
                t = ""
                while t not in TURLER: t = sor("Tür (k=kedi, p=köpek, i=insan)", son["tur"] or "k").lower()
                son["tur"] = TURLER[t]
                son["hayvan"] = sor("Hayvanın adı / numarası", son["hayvan"] or "1")
                son["kilo"] = sayi_ya_da_bos(sor("Kilo (kg, bilinmiyorsa boş)", son["kilo"]))
                son["yer"] = sor("Mikrofonun yeri (" + ", ".join(f"{i + 1}={s}" for i, s in enumerate(YERLER)) + ")", son["yer"] or YERLER[0], YERLER)
                son["durum"] = sor("Hayvanın durumu (" + ", ".join(f"{i + 1}={s}" for i, s in enumerate(DURUMLAR)) + ")", son["durum"] or DURUMLAR[1], DURUMLAR)
                vs = son["sure"] or (15 if son["tur"] == "insan" else 30)
                try: son["sure"] = max(5, min(300, int(sor("Süre (sn)", vs))))
                except ValueError: son["sure"] = vs
                sor("Mikrofonu yerleştirin; hazır olunca Enter", "")
            else:
                if not son["tur"]: sys.exit("--sormadan için --tur gerekli")
                son["hayvan"] = son["hayvan"] or "1"; son["yer"] = son["yer"] or YERLER[0]; son["durum"] = son["durum"] or DURUMLAR[1]
                son["sure"] = son["sure"] or (15 if son["tur"] == "insan" else 30)
            satir = olcum(p, son, dizin)
            if g.sormadan:
                satir.update(ref_nabiz=sayi_ya_da_bos(g.ref_nabiz), ref_solunum=sayi_ya_da_bos(g.ref_solunum)); satir["not"] = g.not_ or ""
            else:
                print("\nReferans (elle sayılan değerler; ölçümün doğruluğu bunlarla hesaplanacak):")
                satir["ref_nabiz"] = sayi_ya_da_bos(sor("  Nabız /dk (yoksa boş)"))
                satir["ref_solunum"] = sayi_ya_da_bos(sor("  Solunum /dk (yoksa boş)"))
                satir["not"] = sor("  Not (üfürüm, mırlama, hareket vb.; yoksa boş)")
            if satir.get("cihaz_durum") == "YOK" and not satir.get("ham_wav"):
                print("ölçüm alınamadı; CSV'ye satır yazılmadı", flush=True)
            else:
                csv_ekle(csv_yol, satir)
                print(f"kaydedildi: {csv_yol}", flush=True)
            if g.sormadan: break
            if sor("\nYeni ölçüm? (e/h)", "e").lower().startswith("h"): break
    except KeyboardInterrupt:
        print("\nseans kesildi")
    finally:
        try: p.close()
        except Exception: pass


if __name__ == "__main__":
    main()
