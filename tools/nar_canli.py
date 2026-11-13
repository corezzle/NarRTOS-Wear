#!/usr/bin/env python3
"""
nar_canli.py: karttan gelen sesi CANLI izleme sayfası. Tarayıcıda üç grafik akar:
    1) ham veri            mikrofondan gelen ses, hiç işlenmemiş (48 kHz)
    2) kalın sesler        kalp bandı süzgecinden geçmiş ses (ölçüm algoritmasındaki süzgeçle aynı)
    3) ses düzeyi          aynı sesin düzeyi (zarf); her tepe bir kalp atışı

Kart yazılımında değişiklik GEREKTİRMEZ: ölçüm aracının (nar_olcum.py) kullandığı `mic nabiz ... usb nosd` komutunu
art arda gönderir, gelen ses akışını bilgisayarda işler ve yalnız bu bilgisayardan açılan bir sayfada gösterir
(http://127.0.0.1:8770). Her bölümün sonunda kart kendi sonucunu yazar; aynı bölüm bilgisayarda da hesaplanır ve ikisi
sayfada gösterilir. Bölümler arasında kart hesap yaparken grafik birkaç saniye durur.

Kayıt: her bölümün SONUCU (ses değil) ~/Desktop/NAR-kayitlar/canli/canli_sonuclar.csv dosyasına bir satır olarak eklenir
(Excel/Numbers ile açılır). Her bölümün SESİ de ~/Desktop/NAR-kayitlar/canli/ses/<tarih>_<saat>_<tur>.wav olarak saklanır
(48 kHz 24 bit; dakikada ~8,6 MB) ve sayfanın altındaki "Kaydedilen bölümler" listesinden dinlenebilir (dinleme kopyası
sesi yükseltir; kalp sesi kalın olduğu için KULAKLIKLA dinleyin). Kayıt istenmezse --kaydetme (sonuç + ses) ya da
--ses-kaydetme (yalnız ses). Kaydedilmiş bir bölümü grafiklerde yeniden izlemek için: --dosya <o .wav>.

Kullanım:
    python3 tools/nar_canli.py                         kart USB'de takılı; sayfa kendiliğinden açılır
    python3 tools/nar_canli.py --tur kopek             köpek profili (insan | kopek | kedi)
    python3 tools/nar_canli.py --sure 60               bölüm uzunluğu (sn; varsayılan 30, en çok 300)
    python3 tools/nar_canli.py --dosya kayit.wav       kart olmadan: kayıtlı bir ölçümü canlıymış gibi oynatır (kayıt tutmaz)
    python3 tools/nar_canli.py --kaydetme              bölüm sonuçlarını ve sesini dosyaya yazma
    python3 tools/nar_canli.py --ses-kaydetme          yalnız sonuçları yaz, sesi saklama
Durdurmak için terminalde Ctrl+C.

Aynı anda nar_olcum.py ÇALIŞTIRILMAZ (ikisi aynı USB portunu kullanır). Gerekenler: pyserial, numpy.
"""
import argparse, csv, json, os, queue, re, struct, sys, threading, time, wave, webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

BURASI = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, BURASI)
import nabiz_ref as nr          # noqa: E402

FS = 48000
BLOK = 4800            # 100 ms: 25 ham/süzülmüş çubuk (4 ms) + 10 zarf değeri (10 ms)


# ───────────────────────── sayfaya yayın ─────────────────────────
class Yayin:
    """Tarayıcı bağlantılarına (SSE) ileti dağıtır. Son durum yeni bağlanana hemen gönderilir."""
    def __init__(self):
        self.kilit = threading.Lock(); self.kuyruklar = []
        self.durum_ = {"t": "durum", "baglanti": "başlatılıyor", "kaynak": "", "tur": "", "bolum_sn": 0, "kart": "", "mac": "", "akiyor": False, "dosya": "", "kayitlar": []}

    def kayit_ekle(self, ad, zaman, sonuc):
        """Kaydedilen bölüm listesinin başına ekler (en çok 40 tutulur) ve sayfaya bildirir."""
        with self.kilit: l = [{"ad": ad, "zaman": zaman, "sonuc": sonuc}] + self.durum_["kayitlar"]
        self.durum(kayitlar=l[:40])

    def baglan(self):
        q = queue.Queue(maxsize=600)
        with self.kilit:
            self.kuyruklar.append(q); q.put(dict(self.durum_))
        return q

    def ayril(self, q):
        with self.kilit:
            if q in self.kuyruklar: self.kuyruklar.remove(q)

    def gonder(self, ileti):
        with self.kilit:
            for q in self.kuyruklar:
                try: q.put_nowait(ileti)
                except queue.Full: pass            # yavaş/arka plandaki sekme: veri atlanır, sayfa kilitlenmez

    def durum(self, **kw):
        with self.kilit: self.durum_.update(kw); d = dict(self.durum_)
        self.gonder(d)


# ───────────────────────── ses işleme ─────────────────────────
class Isleyici:
    """48 kHz örnekleri alır; 100 ms'lik bloklar hâlinde üç grafiğin verisini ve anlık nabız tahminini üretir."""
    def __init__(self, tur, yayin):
        p = nr.PROFIL[tur]; self.lag_min, self.lag_max = p["lag_min"], p["lag_max"]
        self.hp = nr.biquad("hp", p["hp"], 1000.0); self.lp = nr.biquad("lp", p["lp"], 1000.0)   # algoritmadaki kalp bandı
        self.zh = [0.0, 0.0]; self.zl = [0.0, 0.0]
        self.yayin = yayin; self.bek = None; self.e = []; self.blok = 0

    @staticmethod
    def _bq(k, z, xs):
        b0, b1, b2, a1, a2 = k; z1, z2 = z; out = [0.0] * len(xs)
        for i, v in enumerate(xs):
            y = b0 * v + z1; z1 = b1 * v - a1 * y + z2; z2 = b2 * v - a2 * y; out[i] = y
        z[0], z[1] = z1, z2
        return out

    def yeni_bolum(self):
        """Bölümler arasında akış kesilir: zarf geçmişi ve artık örnekler atılır (tahmin boşluğun üstünden hesaplanmasın)."""
        self.bek = None; self.e = []

    def besle(self, ornek):
        import numpy as np
        self.bek = ornek if self.bek is None or len(self.bek) == 0 else np.concatenate((self.bek, ornek))
        while len(self.bek) >= BLOK:
            x = self.bek[:BLOK].astype(np.float64) / 8388608.0; self.bek = self.bek[BLOK:]
            h = x.reshape(25, 192)
            y = np.asarray(self._bq(self.lp, self.zl, self._bq(self.hp, self.zh, x.reshape(100, 48).mean(1).tolist())))
            s = y.reshape(25, 4)
            z0 = len(self.e); self.e.extend(np.abs(y).reshape(10, 10).mean(1).tolist())
            zarf = [sum(self.e[max(0, i - 4):i + 1]) / (i + 1 - max(0, i - 4)) for i in range(z0, z0 + 10)]   # 5'lik düzleştirme
            if len(self.e) > 3000: del self.e[:len(self.e) - 3000]
            ileti = {"t": "veri", "hmin": h.min(1).round(7).tolist(), "hmax": h.max(1).round(7).tolist(),
                     "smin": s.min(1).round(8).tolist(), "smax": s.max(1).round(8).tolist(), "zarf": [round(v, 8) for v in zarf]}
            self.blok += 1
            if self.blok % 10 == 0:                                   # saniyede bir: son 8 sn'ye göre anlık tahmin
                if len(self.e) >= 800:
                    b, r, _, ic = nr.nabiz_ac_ic(np.asarray(self.e[-800:]), self.lag_min, self.lag_max)
                    ileti["tahmin"] = {"nabiz": round(b, 1), "r": round(r, 2), "ic": int(ic)}
                else:
                    ileti["tahmin"] = {"bekle": round(8 - len(self.e) / 100.0, 1)}
            self.yayin.gonder(ileti)


SUTUNLAR = ["zaman", "tur", "sure_sn", "alg", "durum", "nabiz", "guven", "yontem", "netlik_r", "aday_nabiz", "solunum_durum", "solunum",
            "kalp_bandi_dbfs", "vurus_belirginligi", "pencereler", "kart_alg", "kart_durum", "kart_nabiz", "kart_r"]
_csv_kilit = threading.Lock()


def sonuc_kaydet(yol, zaman, tur, s, kart):
    """Bölüm sonucunu CSV'ye ekler (dosya yoksa başlıkla oluşturur). s: nabiz_ref sonucu, kart: kartın sonuc satırı sözlüğü."""
    ok = s["durum"] == "OK"
    satir = {"zaman": zaman, "tur": tur, "sure_sn": f"{s['sure']:.0f}", "alg": s["alg"], "durum": s["durum"],
             "nabiz": f"{s['nabiz']:.0f}" if ok else "", "guven": ("dusuk" if s["yontem"] in ("pencere", "tutarli") else "normal") if ok else "",
             "yontem": s["yontem"] if ok else "", "netlik_r": f"{s['r']:.2f}", "aday_nabiz": f"{s['nabiz_ac']:.0f}",
             "solunum_durum": s["sdurum"], "solunum": f"{s['solunum']:.0f}" if s["sdurum"] == "OK" else "",
             "kalp_bandi_dbfs": f"{s['seviye']:.1f}", "vurus_belirginligi": f"{s['belirgin']:.1f}",
             "pencereler": " ".join(f"{b}({r:.2f})" for b, r in s["pencere"]),
             "kart_alg": kart.get("alg", ""), "kart_durum": kart.get("durum", ""), "kart_nabiz": kart.get("nabiz", ""), "kart_r": kart.get("r", "")}
    with _csv_kilit:
        os.makedirs(os.path.dirname(yol), exist_ok=True); yeni = not os.path.exists(yol)
        with open(yol, "a", newline="", encoding="utf-8-sig" if yeni else "utf-8") as f:
            w = csv.DictWriter(f, fieldnames=SUTUNLAR)
            if yeni: w.writeheader()
            w.writerow(satir)


def ses_adi(zaman, tur): return f"{zaman.replace(' ', '_').replace(':', '-')}_{tur}.wav"


def ses_yaz(yol, ornekler):
    """int32 örnekleri 48 kHz 24 bit mono WAV olarak yazar (nar_olcum.py'nin ham kayıtlarıyla aynı biçim)."""
    import numpy as np
    os.makedirs(os.path.dirname(yol), exist_ok=True)
    b = np.ascontiguousarray(np.asarray(ornekler).astype("<i4")).view(np.uint8).reshape(-1, 4)[:, :3].tobytes()
    with wave.open(yol + ".yaz", "wb") as f:
        f.setnchannels(1); f.setsampwidth(3); f.setframerate(FS); f.writeframes(b)
    os.replace(yol + ".yaz", yol)


def bolum_sonucu(yayin, ornekler, tur, kayit=None, kart=None, ses_dizini=None):
    """Biten bölümü bilgisayarda ölçüm algoritmasıyla (nabiz_ref) hesaplar; sonuç sayfaya yazılır, kayit verilmişse CSV'ye
    eklenir, ses_dizini verilmişse bölümün sesi WAV olarak saklanır."""
    zaman = time.strftime("%Y-%m-%d %H:%M:%S")
    ses_ad = None
    if ses_dizini:                                                    # ses önce yazılır: hesap hata verse de kayıt kalsın
        try: ses_ad = ses_adi(zaman, tur); ses_yaz(os.path.join(ses_dizini, ses_ad), ornekler)
        except Exception as e: ses_ad = None; yayin.durum(dosya=f"SES YAZILAMADI: {e}")
    try:
        s = nr.analiz(ornekler, 24, tur)
        if s["durum"] == "OK":
            guven = "düşük güven, " + s["yontem"] + " kuralı" if s["yontem"] in ("pencere", "tutarli") else ("düzensiz ritim, vuruş sayımı" if s["yontem"] == "vurus" else f"netlik {s['r']:.2f}".replace(".", ","))
            m = f"{s['nabiz']:.0f} atış/dk ({guven})"
        elif s["durum"] in ("BELIRSIZ", "KAT_SUPHESI"): m = f"belirsiz (aday {s['nabiz_ac']:.0f} atış/dk, netlik {s['r']:.2f})".replace(".", ",")
        else: m = {"KISA": "kayıt çok kısa", "SINYAL_YOK": "sinyal yok", "DOYGUN": "kayıt doygun", "MIRLAMA": "mırlama algılandı"}.get(s["durum"], s["durum"])
        m += f"; solunum {s['solunum']:.0f}/dk" if s["sdurum"] == "OK" else "; solunum belirsiz"
        yayin.durum(mac=f"{time.strftime('%H:%M:%S')}  {m}  [alg {s['alg']}, {s['sure']:.0f} sn]")
    except Exception as e:                                            # hesap hatası akışı durdurmasın
        yayin.durum(mac=f"hesaplanamadı: {e}")
        if ses_ad: yayin.kayit_ekle(ses_ad, zaman[11:], "hesaplanamadı")
        return
    if ses_ad: yayin.kayit_ekle(ses_ad, zaman[11:], m)
    if kayit:
        try: sonuc_kaydet(kayit, zaman, tur, s, kart or {})
        except OSError as e: yayin.durum(dosya=f"YAZILAMADI: {e}")


# ───────────────────────── kaynak: kart ─────────────────────────
def kart_bolumu(p, sure, tur, isleyici, yayin, kayit=None, ses_dizini=None):
    """Bir `mic nabiz` bölümü: komutu gönderir, akışı çözer, bitince kartın ve bilgisayarın sonucunu yayınlar."""
    import numpy as np
    p.reset_input_buffer(); p.write(f"mic nabiz {sure} 3 24 0 {tur} usb nosd\r\n".encode()); p.flush()
    isleyici.yeni_bolum()
    buf = bytearray(); metin = bytearray(); artik = b""; beklenen = None; parcalar = []; t0 = time.time(); ilk_veri = False
    yayin.durum(baglanti="kart bağlı", akiyor=False, bolum_sn=sure)
    while time.time() - t0 < sure + 45:
        d = p.read(65536)
        if not d: continue
        buf += d
        while True:
            i = buf.find(b"NARA")
            if i < 0:
                if len(buf) > 3: metin += buf[:-3]; del buf[:-3]      # son 3 bayt yarım "NARA" olabilir
                break
            if i > 0: metin += buf[:i]; del buf[:i]
            if len(buf) < 16: break
            seq, ln, _ov, top = struct.unpack_from("<IHHI", buf, 4)
            if ln > 960: del buf[:4]; continue
            if len(buf) < 16 + ln: break
            v = bytes(buf[16:16 + ln])
            if sum(v) != top: del buf[:4]; continue                   # bozuk çerçeve: atla
            del buf[:16 + ln]
            if beklenen is not None and beklenen < seq <= beklenen + 50: v = bytes(960 * (seq - beklenen)) + v   # kayıp = sessizlik
            beklenen = seq + 1
            v = artik + v; n = len(v) // 3 * 3; artik = v[n:]
            if n:
                o = nr.pcm_coz(v[:n], 24); parcalar.append(o); isleyici.besle(o)
                if not ilk_veri: ilk_veri = True; yayin.durum(akiyor=True)
        if len(metin) > 6000: del metin[:len(metin) - 6000]
        if b"[cmd] OK: mic nabiz" in metin or b"[cmd] ERR" in metin: break
    yayin.durum(akiyor=False)
    satirlar = [m.decode("latin1") for m in re.findall(rb"\[(?:mic|cmd)\][ -~]*", bytes(metin))]
    hata = next((s for s in satirlar if s.startswith(("[cmd] ERR", "[mic] I2S", "[mic] bellek yok"))), None)
    nb = next((s[len("[mic] nabiz: "):] for s in satirlar if s.startswith("[mic] nabiz:")), None)
    sl = next((s[len("[mic] solunum: "):] for s in satirlar if s.startswith("[mic] solunum:")), None)
    if hata: yayin.durum(kart=f"{time.strftime('%H:%M:%S')}  HATA: {hata}")
    elif nb: yayin.durum(kart=f"{time.strftime('%H:%M:%S')}  {nb.split('  sure ')[0]}" + (f"; solunum {sl}" if sl else ""))
    else: yayin.durum(kart=f"{time.strftime('%H:%M:%S')}  karttan sonuç satırı gelmedi")
    sn = next((s[len("[mic] sonuc: "):] for s in satirlar if s.startswith("[mic] sonuc:")), "")
    kart = dict(t.split("=", 1) for t in sn.split(" etiket=")[0].split() if "=" in t)
    if parcalar: threading.Thread(target=bolum_sonucu, args=(yayin, np.concatenate(parcalar), tur, kayit, kart, ses_dizini), daemon=True).start()
    return hata is None and bool(parcalar)


def kart_dongusu(g, isleyici, yayin):
    import serial
    import nar_olcum as no
    while True:
        yayin.durum(baglanti="kart aranıyor; açılışı 30-45 sn sürebilir", kaynak="kart (USB)", akiyor=False)
        try: p = no.port_ac()
        except SystemExit as e:
            yayin.durum(baglanti=f"kart yok: {e}"); time.sleep(3); continue
        try:
            ust_uste_hata = 0
            while True:
                ok = kart_bolumu(p, g.sure, g.tur, isleyici, yayin, g.kayit, g.ses_dizini)
                ust_uste_hata = 0 if ok else ust_uste_hata + 1
                if ust_uste_hata >= 3:
                    yayin.durum(baglanti="kart üç kez art arda ses göndermedi; durduruldu. Terminalde Ctrl+C ile kapatıp yeniden başlatın."); return
                time.sleep(0.5)
        except (serial.SerialException, OSError) as e:
            yayin.durum(baglanti=f"kart bağlantısı koptu ({e}); yeniden aranıyor", akiyor=False)
            try: p.close()
            except Exception: pass
            time.sleep(2)


# ───────────────────────── kaynak: kayıtlı dosya ─────────────────────────
def dosya_dongusu(g, isleyici, yayin):
    v, bit = nr.wav_oku(g.dosya)
    if bit != 24: sys.exit("24 bit WAV bekleniyor (nar_olcum.py'nin yazdığı ham kayıt)")
    ad = os.path.basename(g.dosya)
    while True:
        isleyici.yeni_bolum(); yayin.durum(baglanti="kayıttan oynatılıyor", kaynak=ad, akiyor=True, bolum_sn=len(v) // FS)
        t0 = time.time()
        for k in range(0, len(v) - BLOK + 1, BLOK):
            isleyici.besle(v[k:k + BLOK])
            bekle = t0 + (k + BLOK) / FS - time.time()
            if bekle > 0: time.sleep(bekle)
        yayin.durum(akiyor=False, kart="(dosyadan oynatma: kart sonucu yok)")
        threading.Thread(target=bolum_sonucu, args=(yayin, v, g.tur), daemon=True).start()
        time.sleep(2.0)


# ───────────────────────── web ─────────────────────────
SAYFA = r"""<!doctype html>
<html lang="tr"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>NAR canlı ses izleme</title>
<style>
  :root { --bg:#f8fafc; --kart:#ffffff; --ink:#0f172a; --ink2:#475569; --cizgi:#e2e8f0; --teal:#0f766e; --teal2:#ccfbf1; --gri:#64748b; --amber:#b45309; --kirmizi:#dc2626; }
  @media (prefers-color-scheme: dark) { :root { --bg:#0b1220; --kart:#111a2e; --ink:#e2e8f0; --ink2:#94a3b8; --cizgi:#243049; --teal:#2dd4bf; --teal2:#134e4a; --gri:#94a3b8; --amber:#fbbf24; --kirmizi:#f87171; } }
  * { box-sizing: border-box; }
  body { margin:0; background:var(--bg); color:var(--ink); font-family:-apple-system,"Helvetica Neue",Helvetica,Arial,sans-serif; font-size:14px; }
  main { max-width:1180px; margin:0 auto; padding:16px; }
  header { display:flex; flex-wrap:wrap; gap:12px; align-items:stretch; margin-bottom:12px; }
  .kutu { background:var(--kart); border:1px solid var(--cizgi); border-radius:8px; padding:10px 14px; }
  .anlik { min-width:230px; }
  .anlik .sayi { font-size:34px; font-weight:600; line-height:1.1; font-variant-numeric:tabular-nums; }
  .anlik .sayi small { font-size:14px; font-weight:400; color:var(--ink2); }
  .etiket { font-size:12px; color:var(--ink2); }
  .durum { flex:1; min-width:300px; display:grid; grid-template-columns:auto 1fr; gap:3px 10px; align-content:center; }
  .durum b { font-weight:600; font-size:12px; color:var(--ink2); white-space:nowrap; }
  .durum span { font-variant-numeric:tabular-nums; overflow-wrap:anywhere; }
  .nokta { display:inline-block; width:9px; height:9px; border-radius:50%; background:var(--gri); margin-right:6px; }
  .nokta.acik { background:var(--teal); }
  .ayar { display:flex; flex-direction:column; gap:8px; justify-content:center; }
  button, select { font:inherit; color:var(--ink); background:var(--kart); border:1px solid var(--cizgi); border-radius:6px; padding:5px 10px; cursor:pointer; }
  .grafik { margin-bottom:12px; padding:10px 12px 6px 12px; }
  .grafik h2 { font-size:14px; font-weight:600; margin:0 0 1px 0; }
  .grafik p { margin:0 0 6px 0; font-size:12px; color:var(--ink2); }
  canvas { display:block; width:100%; height:170px; }
  footer { font-size:12px; color:var(--ink2); line-height:1.5; }
</style></head>
<body><main>
<header>
  <div class="kutu anlik">
    <div class="etiket">Anlık tahmin (son 8 saniye)</div>
    <div class="sayi" id="sayi">–</div>
    <div class="etiket" id="netlik">veri bekleniyor</div>
  </div>
  <div class="kutu durum">
    <b>Bağlantı</b><span><i class="nokta" id="nokta"></i><span id="baglanti">bağlanıyor</span></span>
    <b>Kaynak</b><span id="kaynak">–</span>
    <b>Kartın son bölüm sonucu</b><span id="kart">–</span>
    <b>Bilgisayarın son bölüm sonucu</b><span id="mac">–</span>
    <b>Sonuçların kaydedildiği dosya</b><span id="dosya">–</span>
  </div>
  <div class="kutu ayar">
    <label class="etiket">Görünen süre <select id="pencere"><option value="8">8 sn</option><option value="15">15 sn</option><option value="30">30 sn</option></select></label>
    <button id="dondur">Dondur</button>
  </div>
</header>
<section class="kutu grafik"><h2>1. Ham veri</h2><p>Mikrofondan gelen ses; hiçbir ayırma ya da süzme yapılmadan.</p><canvas id="c1"></canvas></section>
<section class="kutu grafik"><h2>2. Kalın sesler ayrıldıktan sonra mikrofonun duyduğu</h2><p>Yalnız kalp sesinin bulunduğu kalın sesler (insan ve köpekte 20–150 Hz, kedide 30–300 Hz).</p><canvas id="c2"></canvas></section>
<section class="kutu grafik"><h2>3. Aynı sesin düzeyi: her tepe bir kalp atışı</h2><p>Büyük tepe "lub", hemen ardındaki küçük tepe "dub".</p><canvas id="c3"></canvas></section>
<section class="kutu grafik" id="kayitkutu" style="display:none"><h2>Kaydedilen bölümler</h2><p>Her 30 saniyelik bölümün sesi. Kalp sesi kalın olduğu için kulaklıkla dinleyin; dinleme kopyasında ses yükseltilmiştir. Dosyalar: <span id="sesdizini"></span></p>
<table id="kayitlar" style="width:100%; border-collapse:collapse; font-size:13px"><tbody></tbody></table></section>
<footer>Anlık tahmin yalnız son 8 saniyeye bakar ve ölçüm sonucu değildir; netlik 0,5'in altındaysa güvenilmez. Her bölümün sonunda kart hesap yaparken akış birkaç saniye durur. Grafiklerin dikey ölçeği kendiliğinden ayarlanır; tek seferlik darbeler kırpılır.</footer>
</main>
<script>
const HZ = 250, ZHZ = 100, KAP = 30;                      // çubuk/sn, zarf/sn, saklanan saniye
const NB = HZ * KAP, NZ = ZHZ * KAP;
const hmin = new Float32Array(NB), hmax = new Float32Array(NB), smin = new Float32Array(NB), smax = new Float32Array(NB), zarf = new Float32Array(NZ);
let nb = 0, nz = 0, dondu = false, kirli = true, pencere = 8;
const olcek = { c1: null, c2: null, c3: null };
const $ = id => document.getElementById(id);
const tr = (x, n) => x.toFixed(n).replace('.', ',');

function ekle(m) {
  for (let i = 0; i < m.hmin.length; i++) { const k = (nb + i) % NB; hmin[k] = m.hmin[i]; hmax[k] = m.hmax[i]; smin[k] = m.smin[i]; smax[k] = m.smax[i]; }
  nb += m.hmin.length;
  for (let i = 0; i < m.zarf.length; i++) zarf[(nz + i) % NZ] = m.zarf[i];
  nz += m.zarf.length; kirli = true;
  if (m.tahmin) {
    const t = m.tahmin;
    if (t.bekle !== undefined) { $('sayi').textContent = '–'; $('netlik').textContent = 'veri toplanıyor: ' + tr(t.bekle, 0) + ' sn'; }
    else {
      $('sayi').innerHTML = Math.round(t.nabiz) + ' <small>atış/dk</small>';
      const net = t.r >= 0.5 && t.ic;
      $('sayi').style.color = net ? 'var(--teal)' : 'var(--gri)';
      $('netlik').textContent = 'netlik ' + tr(t.r, 2) + (net ? ' (net)' : ' (net değil, güvenilmez)');
    }
  }
}

function hazirla(cv) {
  const o = window.devicePixelRatio || 1, w = Math.round(cv.clientWidth * o), h = Math.round(cv.clientHeight * o);
  if (cv.width !== w || cv.height !== h) { cv.width = w; cv.height = h; }
  const g = cv.getContext('2d'); g.setTransform(1, 0, 0, 1, 0, 0); g.clearRect(0, 0, w, h); return { g, w, h, o };
}
function renk(ad) { return getComputedStyle(document.documentElement).getPropertyValue(ad).trim(); }

// Görünen aralıktaki tipik tepe yüksekliği: 0,5 sn'lik parçaların en yükseklerinin 75. yüzdeliği (tek darbe ölçeği bozmasın)
function tipik(dizi) { if (!dizi.length) return 0; const s = Array.from(dizi).sort((a, b) => a - b); return s[Math.min(s.length - 1, Math.floor(0.75 * s.length))]; }
function yumusat(ad, yeni) { const e = olcek[ad]; olcek[ad] = (e === null || !isFinite(e)) ? yeni : (yeni > e ? e + 0.5 * (yeni - e) : e + 0.08 * (yeni - e)); return olcek[ad]; }

function eksen(g, w, h, o, altBosluk) {
  g.strokeStyle = renk('--cizgi'); g.fillStyle = renk('--gri'); g.lineWidth = 1; g.font = (11 * o) + 'px -apple-system, Helvetica, Arial'; g.textAlign = 'center';
  const adim = pencere <= 8 ? 1 : (pencere <= 15 ? 2 : 5);
  for (let s = 0; s <= pencere; s += adim) {
    const x = Math.round(w - (s / pencere) * (w - 1)) + 0.5;
    g.beginPath(); g.moveTo(x, 0); g.lineTo(x, h - altBosluk); g.stroke();
    g.textAlign = s === 0 ? 'right' : (s === pencere ? 'left' : 'center');
    g.fillText(s === 0 ? 'şimdi' : '−' + s + ' sn', s === 0 ? w - 2 : (s === pencere ? 2 : x), h - 4 * o);
  }
}

function cizMinMax(id, mn, mx, simetrik) {
  const cv = $(id), { g, w, h, o } = hazirla(cv), alt = 18 * o, H = h - alt;
  eksen(g, w, h, o, alt);
  const say = Math.min(nb, pencere * HZ); if (say < 2) return;
  const bas = nb - say, cmin = new Float32Array(w).fill(Infinity), cmax = new Float32Array(w).fill(-Infinity);
  const x0 = w - Math.round(say / (pencere * HZ) * w);
  const parca = [], PB = HZ / 2; let pm = 0, lo = Infinity, hi = -Infinity, orta = 0;
  for (let i = 0; i < say; i++) orta += (mn[(bas + i) % NB] + mx[(bas + i) % NB]) / 2;
  orta = simetrik ? 0 : orta / say;
  for (let i = 0; i < say; i++) {
    const k = (bas + i) % NB, a = mn[k], b = mx[k], x = Math.min(w - 1, x0 + Math.floor(i / say * (w - x0)));
    if (a < cmin[x]) cmin[x] = a; if (b > cmax[x]) cmax[x] = b;
    pm = Math.max(pm, Math.abs(a - orta), Math.abs(b - orta));
    if ((i + 1) % PB === 0) { parca.push(pm); pm = 0; }
  }
  if (!parca.length) parca.push(pm);
  const yari = yumusat(id, Math.max(tipik(parca) * 1.5, 1e-7));
  const y = v => { const t = (v - orta) / yari; return H / 2 - Math.max(-1, Math.min(1, t)) * (H / 2 - 2 * o); };
  g.strokeStyle = renk('--cizgi'); g.beginPath(); g.moveTo(0, H / 2 + 0.5); g.lineTo(w, H / 2 + 0.5); g.stroke();
  g.fillStyle = simetrik ? renk('--gri') : renk('--ink2'); g.beginPath(); let acik = false;
  for (let x = x0; x < w; x++) { if (cmax[x] === -Infinity) continue; const yy = y(cmax[x]); if (!acik) { g.moveTo(x, yy); acik = true; } else g.lineTo(x, yy); }
  for (let x = w - 1; x >= x0; x--) { if (cmin[x] === Infinity) continue; g.lineTo(x, Math.max(y(cmin[x]), y(cmax[x]) + o)); }
  g.closePath(); g.fill();
}

function cizZarf() {
  const cv = $('c3'), { g, w, h, o } = hazirla(cv), alt = 18 * o, H = h - alt;
  eksen(g, w, h, o, alt);
  const say = Math.min(nz, pencere * ZHZ); if (say < 2) return;
  const bas = nz - say, x0 = w - Math.round(say / (pencere * ZHZ) * w), parca = []; let pm = 0;
  for (let i = 0; i < say; i++) { pm = Math.max(pm, zarf[(bas + i) % NZ]); if ((i + 1) % (ZHZ / 2) === 0) { parca.push(pm); pm = 0; } }
  if (!parca.length) parca.push(pm);
  const tavan = yumusat('c3', Math.max(tipik(parca) * 1.25, 1e-8));
  const y = v => H - 1 - Math.min(1, v / tavan) * (H - 6 * o), xx = i => x0 + i / (say - 1) * (w - 1 - x0);
  g.beginPath(); g.moveTo(x0, H);
  for (let i = 0; i < say; i++) g.lineTo(xx(i), y(zarf[(bas + i) % NZ]));
  g.lineTo(w - 1, H); g.closePath(); g.fillStyle = renk('--teal2'); g.fill();
  g.beginPath();
  for (let i = 0; i < say; i++) { const X = xx(i), Y = y(zarf[(bas + i) % NZ]); if (i === 0) g.moveTo(X, Y); else g.lineTo(X, Y); }
  g.strokeStyle = renk('--teal'); g.lineWidth = 1.6 * o; g.lineJoin = 'round'; g.stroke();
}

function ciz() { if (kirli && !dondu) { kirli = false; cizMinMax('c1', hmin, hmax, false); cizMinMax('c2', smin, smax, true); cizZarf(); } requestAnimationFrame(ciz); }

function durum(d) {
  $('baglanti').textContent = d.baglanti + (d.akiyor ? ' · ses akıyor' : ''); $('nokta').className = 'nokta' + (d.akiyor ? ' acik' : '');
  $('kaynak').textContent = (d.kaynak || '–') + (d.tur ? ' · profil ' + d.tur : '') + (d.bolum_sn ? ' · bölüm ' + d.bolum_sn + ' sn' : '');
  $('kart').textContent = d.kart || '–'; $('mac').textContent = d.mac || '–'; $('dosya').textContent = d.dosya || 'kaydedilmiyor';
  kayitlariGoster(d.kayitlar || [], d.ses_dizini || '');
  if (!d.akiyor) { $('netlik').textContent = 'akış durdu (kart hesap yapıyor ya da bekleniyor)'; $('sayi').style.color = 'var(--gri)'; }
}

// Kaydedilen bölümler: yalnız YENİ satırlar eklenir (çalan sesi kesmemek için liste baştan kurulmaz)
const gosterilen = new Set();
function kayitlariGoster(liste, dizin) {
  if (!liste.length) return;
  $('kayitkutu').style.display = ''; $('sesdizini').textContent = dizin;
  const tb = $('kayitlar').tBodies[0];
  for (let i = liste.length - 1; i >= 0; i--) {
    const k = liste[i]; if (gosterilen.has(k.ad)) continue; gosterilen.add(k.ad);
    const tr_ = document.createElement('tr'); tr_.style.borderTop = '1px solid var(--cizgi)';
    const h = (t, st) => { const td = document.createElement('td'); td.style.cssText = 'padding:6px 8px 6px 0;vertical-align:middle;' + (st || ''); if (t !== null) td.textContent = t; tr_.appendChild(td); return td; };
    h(k.zaman, 'white-space:nowrap;font-variant-numeric:tabular-nums'); h(k.sonuc, 'width:45%');
    const a = document.createElement('audio'); a.controls = true; a.preload = 'none'; a.src = '/ses/' + encodeURIComponent(k.ad); a.style.cssText = 'height:30px;width:260px';
    h(null).appendChild(a);
    const ham = document.createElement('a'); ham.href = '/ses/' + encodeURIComponent(k.ad) + '?ham=1'; ham.textContent = k.ad; ham.style.cssText = 'color:var(--ink2);font-size:12px'; ham.title = 'Ham kaydı indir';
    h(null).appendChild(ham);
    tb.insertBefore(tr_, tb.firstChild);
  }
  while (tb.rows.length > 40) tb.deleteRow(tb.rows.length - 1);
}

$('pencere').onchange = e => { pencere = +e.target.value; olcek.c1 = olcek.c2 = olcek.c3 = null; kirli = true; };
$('dondur').onclick = e => { dondu = !dondu; e.target.textContent = dondu ? 'Devam' : 'Dondur'; kirli = true; };
window.addEventListener('resize', () => { kirli = true; });
const es = new EventSource('/akis');
es.onmessage = ev => { const m = JSON.parse(ev.data); if (m.t === 'veri') ekle(m); else if (m.t === 'durum') durum(m); };
es.onerror = () => { $('baglanti').textContent = 'araç kapalı ya da bağlantı koptu (terminalde nar_canli.py çalışıyor mu?)'; $('nokta').className = 'nokta'; };
requestAnimationFrame(ciz);
</script></body></html>
"""


_dinleme = {}        # ad -> dinleme kopyası baytları (son birkaç dosya)


def dinleme_kopyasi(yol):
    """Ham 24 bit kaydın dinlemeye uygun 16 bit kopyası: ortalama çıkarılır, ses yükseltilir (tek darbe sesi kısmasın diye
    %99,9'luk düzey 0,5'e çekilir), taşan yerler kırpılır. Dönüş: WAV dosyası baytları."""
    import io
    import numpy as np
    v, _bit = nr.wav_oku(yol); x = np.asarray(v, dtype=np.float64); x -= x.mean()
    d = np.percentile(np.abs(x), 99.9) if len(x) else 0.0
    y = np.clip(x * (0.5 * 32767.0 / d if d > 0 else 0.0), -32767, 32767).astype("<i2")
    b = io.BytesIO()
    with wave.open(b, "wb") as f:
        f.setnchannels(1); f.setsampwidth(2); f.setframerate(FS); f.writeframes(y.tobytes())
    return b.getvalue()


def sunucu(yayin, port, ses_dizini=None):
    class Istek(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *a): pass

        def do_GET(self):
            if self.path == "/":
                b = SAYFA.encode("utf-8")
                self.send_response(200); self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(b))); self.send_header("Cache-Control", "no-store"); self.end_headers(); self.wfile.write(b)
            elif self.path == "/akis":
                self.send_response(200); self.send_header("Content-Type", "text/event-stream"); self.send_header("Cache-Control", "no-store")
                self.send_header("Connection", "close"); self.end_headers(); self.close_connection = True
                q = yayin.baglan()
                try:
                    while True:
                        try: m = q.get(timeout=10)
                        except queue.Empty: self.wfile.write(b": bekle\n\n"); self.wfile.flush(); continue
                        self.wfile.write(b"data: " + json.dumps(m, separators=(",", ":")).encode("utf-8") + b"\n\n"); self.wfile.flush()
                except (BrokenPipeError, ConnectionResetError, OSError): pass
                finally: yayin.ayril(q)
            elif self.path.startswith("/ses/") and ses_dizini:
                from urllib.parse import unquote, urlsplit
                u = urlsplit(self.path); ad = unquote(u.path[len("/ses/"):])
                yol = os.path.join(ses_dizini, ad)
                if not re.fullmatch(r"[0-9A-Za-z_.-]+\.wav", ad) or not os.path.isfile(yol):      # yalnız kayıt klasöründeki .wav
                    self.send_response(404); self.send_header("Content-Length", "0"); self.end_headers(); return
                try:
                    if "ham=1" in u.query:
                        b = open(yol, "rb").read(); ek = [("Content-Disposition", f'attachment; filename="{ad}"')]
                    else:
                        if ad not in _dinleme:
                            if len(_dinleme) >= 6: _dinleme.pop(next(iter(_dinleme)))
                            _dinleme[ad] = dinleme_kopyasi(yol)
                        b = _dinleme[ad]; ek = []
                except Exception:
                    self.send_response(500); self.send_header("Content-Length", "0"); self.end_headers(); return
                bas, son, kod = 0, len(b) - 1, 200
                m = re.fullmatch(r"bytes=(\d*)-(\d*)", self.headers.get("Range", "") or "")
                if m and (m.group(1) or m.group(2)):                  # ses oynatıcının ileri/geri sarması için
                    if m.group(1): bas = int(m.group(1)); son = min(int(m.group(2)), son) if m.group(2) else son
                    else: bas = max(0, len(b) - int(m.group(2)))
                    if bas > son: self.send_response(416); self.send_header("Content-Range", f"bytes */{len(b)}"); self.send_header("Content-Length", "0"); self.end_headers(); return
                    kod = 206
                self.send_response(kod); self.send_header("Content-Type", "audio/wav"); self.send_header("Accept-Ranges", "bytes")
                if kod == 206: self.send_header("Content-Range", f"bytes {bas}-{son}/{len(b)}")
                for k_, v_ in ek: self.send_header(k_, v_)
                self.send_header("Content-Length", str(son - bas + 1)); self.send_header("Cache-Control", "no-store"); self.end_headers()
                try: self.wfile.write(b[bas:son + 1])
                except (BrokenPipeError, ConnectionResetError, OSError): pass
            else:
                self.send_response(404); self.send_header("Content-Length", "0"); self.end_headers()

    s = ThreadingHTTPServer(("127.0.0.1", port), Istek); s.daemon_threads = True
    return s


def main():
    ap = argparse.ArgumentParser(description="NAR canlı ses izleme (üç grafik, tarayıcıda)")
    ap.add_argument("--tur", default="insan", choices=sorted(nr.PROFIL), help="profil (varsayılan insan)")
    ap.add_argument("--sure", type=int, default=30, help="bölüm uzunluğu, sn (varsayılan 30; en çok 300)")
    ap.add_argument("--dosya", help="kart yerine kayıtlı 24 bit WAV oynat")
    ap.add_argument("--http", type=int, default=8770, help="sayfanın portu (varsayılan 8770)")
    ap.add_argument("--acma", action="store_true", help="tarayıcıyı kendiliğinden açma")
    ap.add_argument("--kaydetme", action="store_true", help="bölüm sonuçlarını ve sesini dosyaya yazma")
    ap.add_argument("--ses-kaydetme", dest="ses_kaydetme", action="store_true", help="bölümlerin sesini saklama (sonuçlar yine yazılır)")
    g = ap.parse_args()
    g.sure = max(10, min(300, g.sure))
    try: import numpy  # noqa: F401
    except ImportError: sys.exit("numpy gerekli: python3 -m pip install --user --break-system-packages numpy")
    if not g.dosya:
        try: import serial  # noqa: F401
        except ImportError: sys.exit("pyserial gerekli: python3 -m pip install --user --break-system-packages pyserial")
    g.kayit = None
    if not g.dosya and not g.kaydetme:
        g.kayit = os.path.join(os.environ.get("CANLI_KAYIT_DIZINI") or os.path.expanduser("~/Desktop/NAR-kayitlar/canli"), "canli_sonuclar.csv")
    g.ses_dizini = os.path.join(os.path.dirname(g.kayit), "ses") if (g.kayit and not g.ses_kaydetme) else None
    yayin = Yayin(); yayin.durum(tur=g.tur, dosya=g.kayit or "", ses_dizini=g.ses_dizini or ""); isleyici = Isleyici(g.tur, yayin)
    if g.ses_dizini and os.path.isdir(g.ses_dizini):                  # önceki oturumların kayıtları da listede görünsün
        sonuclar = {}
        try:
            with open(g.kayit, encoding="utf-8-sig") as f:
                for r in csv.DictReader(f): sonuclar[r.get("zaman", "")] = (f"{r.get('nabiz')} atış/dk ({r.get('guven')} güven)" if r.get("durum") == "OK" else (r.get("durum") or "").lower())
        except OSError: pass
        eski = []
        for ad in sorted((a for a in os.listdir(g.ses_dizini) if a.endswith(".wav")), reverse=True)[:40]:
            z = ad[:10] + " " + ad[11:19].replace("-", ":")
            eski.append({"ad": ad, "zaman": z[5:], "sonuc": sonuclar.get(z, "")})
        if eski: yayin.durum(kayitlar=eski)
    try: s = sunucu(yayin, g.http, g.ses_dizini)
    except OSError as e: sys.exit(f"http://127.0.0.1:{g.http} açılamadı ({e}). Araç zaten çalışıyor olabilir; başka port için --http 8771")
    threading.Thread(target=s.serve_forever, daemon=True).start()
    adres = f"http://127.0.0.1:{g.http}/"
    print(f"canlı izleme sayfası: {adres}   (durdurmak için Ctrl+C)", flush=True)
    print(f"bölüm sonuçları: {g.kayit}" if g.kayit else "bölüm sonuçları kaydedilmiyor", flush=True)
    print(f"bölüm ses kayıtları: {g.ses_dizini}  (dakikada ~8,6 MB)" if g.ses_dizini else "ses kaydedilmiyor", flush=True)
    if not g.acma: webbrowser.open(adres)
    try:
        if g.dosya: dosya_dongusu(g, isleyici, yayin)
        else: kart_dongusu(g, isleyici, yayin)
        while True: time.sleep(1)                                     # kaynak durduysa sayfa durum iletisiyle açık kalsın
    except KeyboardInterrupt:
        print("\nkapatıldı", flush=True)


if __name__ == "__main__":
    main()
