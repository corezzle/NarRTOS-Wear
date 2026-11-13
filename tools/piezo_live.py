#!/usr/bin/env python3
"""
piezo_live.py — NAR tasma piezo sensöründen (A modu, analog) CANLI nabız / solunum.

Kullanım (PlatformIO'nun Python'u pyserial içerir):
    ~/.platformio/penv/bin/python tools/piezo_live.py              → tarayıcı: http://localhost:8765
    ~/.platformio/penv/bin/python tools/piezo_live.py --terminal   → terminalde sade görünüm (saniyede 1 satır); tarayıcı da açık kalır
    ... --terminal --ayrinti                                        → ayrıntılı satır (ham değerler, r, çizgi grafik)
    ... --kedi                                                     → kedi profili (nabız 80-280, nefes 12-70/dk)
    ... --sert                                                     → katı hareket kapısı (her kıpırtıda kes; varsayılan yumuşak)
    ... --bul                                                      → nabız noktası bulucu (yarım sn'de bir sinyal çubuğu)
    ... --nabiz                                                    → tarayıcıda yalnız nabız + EKG tarzı akan dalga (http://localhost:8765)
    ... --ham                                                      → tarayıcı dalgası ham ADC olsun (filtresiz); --nabiz veya normal sayfa ile
    ... --log kayit.csv                                            → ham + temiz örnekleri dosyaya yaz
    Durdurmak: Ctrl+C

Veri yolu (cihaz → Mac):
  Cihaza arka arkaya `piezo capture 60 200` gönderilir (firmware: vibration_service_capture).
  Her satır: "[cap] <t_ms> p1..p20 | ax ay az ×5"  — piezo 200 Hz (12-bit ADC), ivme 50 Hz (mg).
  Kayıt sırasında cihazın ekran çizimi durur (QSPI komşu hat paraziti) ve darbe sayımı bekler.

İşleme zinciri (her örnek için):
  1. 50 Hz şebeke notch'u (RBJ biquad, Q=20).
  2. İki bant: NABIZ 0.7-3 Hz, SOLUNUM 0.12-0.7 Hz — 4. derece Butterworth (2 biquad HP + 2 LP).
  3. Hareket iptali: ivmeölçerin 3 ekseni aynı banda süzülür ve NLMS uyarlamalı filtreyle
     piezodan çıkarılır (ivme = hareketin referansı; nabız/nefes ivmede görünmez, hareket görünür).
  4. Hareket kapısı: ivme sapması (1 sn RMS, mg) ve piezo genliği; kapı kapalıyken ölçüm yok.
  5. Ritim: NABIZ otokorelasyon (8 sn, 40-120/dk; >120 = titreme), SOLUNUM/hareket otokorelasyon
     (20 sn, 8-45/dk), SOLUNUM/nabızdan = atım aralığı + atım yüksekliği modülasyonu (40 sn,
     sinüs aritmisi; bilek/boyundan çalışır).
  6. Her ritim için 1-B Kalman izleyici: ölçüm güveni → ölçüm gürültüsü; belirsizlik (P)
     küçükse "kilitli" gösterilir, ölçüm gelmeyince P büyür ve kilit düşer. Sayı zıplamaz.
"""
import glob, json, math, re, sys, threading, time
from collections import deque
from http.server import HTTPServer, BaseHTTPRequestHandler

HZ = 200
ACC_HZ = 50
# Tür profili: insan (varsayılan) / kedi (--kedi). Nabız ve solunum bantları, aralık sınırları.
PROFILES = {
    # hr_band alt sınırı 0.5 Hz: süzülmüş gürültünün ACF tepesi bant kenarında çıkar; 0.7 Hz'de bu
    # 42-50/dk sahte kilit üretiyordu. 0.5 Hz = 30/dk → arama aralığının (≥37/dk) dışında kalır.
    "insan": dict(hr_band=(0.5, 3.0),  hr_lag=(0.3, 1.5),  hr_max=120,  rr_band=(0.15, 0.6), rr_lag=(1.5, 7.0), rr_ok=(0.4, 1.6), rr_win=30, hr_win=8, hr_acq=5),
    # rr_band alt sınırı 0.15 Hz (9/dk): 0.08-0.13 Hz'deki yavaş kaymalar "8/dk" sahte kilit üretiyordu
    # (gerçek sayım 18/dk). Dinlenen yetişkin 12-20/dk; 9 altı beklenmez.
    # kedi: 170/dk → 6 sn'de 17 atım yeter; kilit için 3 ardışık ölçüm. İlk sayı ~8 sn, kayıp ~5 sn.
    "kedi":  dict(hr_band=(1.0, 5.0),  hr_lag=(0.2, 0.8),  hr_max=280,  rr_band=(0.15, 1.2), rr_lag=(0.8, 5.0), rr_ok=(0.2, 0.75), rr_win=20, hr_win=6, hr_acq=3),
}
PROF = PROFILES["kedi" if "--kedi" in sys.argv else "insan"]
# Hareket kapısı (VARSAYILAN: yumuşak). Küçük hareketlerde (nefes, hafif baş çevirme, kıpırdanma;
# ivme < 120 mg) ölçüm sürer — hareket iptali (NLMS) siler, güven şartı sayıyı tutar. Yalnız büyük
# hareket (kalkma, yürüme, kaşınma; ivme ≥ 120 mg) ölçümü keser, bitince ~5 sn'de geri gelir.
# --sert: eski katı kapı (ivme > 25 mg veya piezo genlik sıçraması → kes); masa testleri için.
SOFT_GATE = "--sert" not in sys.argv
RAW_VIEW = "--ham" in sys.argv     # tarayıcı dalgası: filtresiz ham ADC (ortalama çıkarılmış), nabız bandı değil
PORT = 8765
SEG_S = 60                      # tek capture süresi (firmware üst sınırı 60)
strip = lambda s: re.sub(r"\x1b\[[0-9;]*m", "", s)


# ─── Filtreler ───────────────────────────────────────────────────────────────
class Biquad:
    """RBJ cookbook biquad, direct form I."""
    def __init__(self, b0, b1, b2, a0, a1, a2):
        self.b0, self.b1, self.b2 = b0 / a0, b1 / a0, b2 / a0
        self.a1, self.a2 = a1 / a0, a2 / a0
        self.x1 = self.x2 = self.y1 = self.y2 = 0.0
    def push(self, x):
        y = self.b0 * x + self.b1 * self.x1 + self.b2 * self.x2 - self.a1 * self.y1 - self.a2 * self.y2
        self.x2, self.x1, self.y2, self.y1 = self.x1, x, self.y1, y
        return y
    @staticmethod
    def lowpass(f, q, fs=HZ):
        w = 2 * math.pi * f / fs; a = math.sin(w) / (2 * q); c = math.cos(w)
        return Biquad((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + a, -2 * c, 1 - a)
    @staticmethod
    def highpass(f, q, fs=HZ):
        w = 2 * math.pi * f / fs; a = math.sin(w) / (2 * q); c = math.cos(w)
        return Biquad((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + a, -2 * c, 1 - a)
    @staticmethod
    def notch(f, q, fs=HZ):
        w = 2 * math.pi * f / fs; a = math.sin(w) / (2 * q); c = math.cos(w)
        return Biquad(1, -2 * c, 1, 1 + a, -2 * c, 1 - a)


class Chain:
    def __init__(self, sections): self.s = sections
    def push(self, v):
        for b in self.s: v = b.push(v)
        return v


def bandpass4(lo, hi):
    """4. derece Butterworth bant geçiren: 2 HP + 2 LP biquad (Q = 0.5412, 1.3066)."""
    return Chain([Biquad.highpass(lo, 0.5412), Biquad.highpass(lo, 1.3066),
                  Biquad.lowpass(hi, 0.5412),  Biquad.lowpass(hi, 1.3066)])


class NLMS:
    """Uyarlamalı hareket iptali: d (piezo) − Σ w·ref (ivme eksenleri, aynı banda süzülmüş).
    Normalize LMS, her eksen için `taps` örnek gecikme hattı (eksenler arası faz farkı için)."""
    def __init__(self, nref=3, taps=8, mu=0.02):
        self.taps, self.mu = taps, mu
        self.h = [deque([0.0] * taps, maxlen=taps) for _ in range(nref)]
        self.w = [[0.0] * taps for _ in range(nref)]
    def push(self, d, refs):
        for q, r in zip(self.h, refs): q.appendleft(r)
        y = 0.0; norm = 1e-6
        for wi, q in zip(self.w, self.h):
            for k, (w, r) in enumerate(zip(wi, q)): y += w * r; norm += r * r
        e = d - y
        g = self.mu * e / norm
        for wi, q in zip(self.w, self.h):
            for k, r in enumerate(q): wi[k] += g * r
        return e


class Kalman1D:
    """Sabit-değer modeli: x̂ (bpm), P (varyans). predict: P += q·dt. update(m, conf):
    R = r_min / max(conf, 0.05)²; K = P/(P+R). Kilit: P < p_lock ve son ölçümler yakın."""
    def release(self, cooldown=0.0):
        """Dış tetik (temas kaybı): kilidi anında bırak, alma zincirini sıfırla; cooldown sn boyunca
        yeniden kilit alma (pencere eski atımlarla dolu, sahte yeniden-kilit olurdu)."""
        self._acquired = False; self.confs.clear(); self.meas.clear(); self.P = max(self.P, self.p_lock * 4)
        self.t_cool = time.time() + cooldown
    def __init__(self, q, r_min, p_lock, name):
        self.q, self.r_min, self.p_lock, self.name = q, r_min, p_lock, name
        self.x = None; self.P = 1e6; self.t_last_meas = 0.0; self.innov = 0.0
        self.confs = deque(maxlen=PROF["hr_acq"]); self.meas = deque(maxlen=PROF["hr_acq"]); self.t_lock = 0.0; self._was_locked = False
        self._acquired = False; self.weak = 0          # ardışık zayıf (0.25-0.3) ölçüm sayacı
        self.t_cool = 0.0
    def predict(self, dt):
        self.P += self.q * dt
    def update(self, m, conf):
        if m is None or conf < 0.25:                 # gürültü ölçümü izleyiciyi beslemez; alma zincirini kırar
            self.confs.clear(); self.meas.clear(); return
        self.confs.append(conf); self.meas.append(m)
        self.weak = self.weak + 1 if conf < 0.35 else 0  # tutma için güven ≥ 0.35 gerekir
        R = self.r_min / max(conf, 0.05) ** 2
        if self.x is None:
            if conf >= 0.4: self.x = m; self.P = R; self.t_last_meas = time.time()
            return
        self.innov = m - self.x
        # Kaba sapan ölçüm (±%35 dışı) düşük güvenle içeri alınır: izleyici kaymaz, ama yeni
        # gerçek değere zamanla yakınsar.
        if abs(self.innov) > 0.35 * self.x: R *= 6
        K = self.P / (self.P + R)
        self.x += K * self.innov; self.P *= (1 - K); self.t_last_meas = time.time()
    @property
    def locked(self):
        # Histerezis. ALMA: son 5 ölçümün her biri güven ≥ 0.4 ve ritimler ±%10 içinde (gürültü,
        # örtüşen 8 sn pencerelerde 3 sn boyunca şans eseri 0.4'ü geçebiliyordu → masada 54/dk sahte
        # kilit). TUTMA: alındıktan sonra belirsizlik küçük kaldıkça ve 8 sn içinde ölçüm geldikçe
        # sürer; tek bir sapan ölçüm düşük ağırlıkla girer, kilidi düşürmez.
        acquire = (len(self.confs) == PROF["hr_acq"] and min(self.confs) >= 0.4 and
                   max(self.meas) <= 1.1 * min(self.meas))
        hold = (self.x is not None and self.P < self.p_lock and time.time() - self.t_last_meas < 3
                and self.weak < 3)                      # 3 ardışık zayıf ölçüm → kilit düşer
        if not self._acquired and acquire and hold and time.time() >= self.t_cool: self._acquired = True
        if self._acquired and not hold: self._acquired = False
        now_locked = self._acquired
        if now_locked and not self._was_locked: self.t_lock = time.time()
        self._was_locked = now_locked
        return now_locked
    @property
    def value(self): return round(self.x) if self.locked else None
    @property
    def sd(self): return math.sqrt(self.P) if self.x is not None else None


# ─── Paylaşılan durum ────────────────────────────────────────────────────────
lock    = threading.Lock()
pulse_q = deque(maxlen=HZ * 60)   # temiz nabız bandı (hareket iptali sonrası)
resp_q  = deque(maxlen=HZ * 60)   # temiz solunum bandı
acc_q   = deque(maxlen=HZ * 60)   # ivme sapması (mg, yüksek geçirilmiş büyüklük) — kapı için
rawadc_q = deque(maxlen=HZ * 5)   # ham ADC (hiç işlenmemiş) — terminalde min/ort/max için
rawn_q  = deque(maxlen=HZ * 10)   # bulucu sinyali: notch + hafif 0.5-8 Hz bant (2. derece, ~0.3 s tepki)
pamp_q  = deque(maxlen=60)        # saniyelik piezo genlikleri (taban)
lock_amps = deque(maxlen=10)      # kilit sırasında saniyelik nabız bandı genlikleri (temas kaybı tespiti)
status  = {"state": "port bekleniyor", "n": 0, "acc_ok": False,
           "motion": False, "acc_rms": 0.0, "amp": 0.0,
           "hr": None, "hr_raw": None, "hr_r": 0.0, "hr_sd": None, "hr_why": "sinyal bekleniyor",
           "rr": None, "rr_raw": None, "rr_r": 0.0, "rr_sd": None, "rr_why": "sinyal bekleniyor",
           "rsa": None, "rsa_raw": None, "rsa_q": 0.0, "rsa_beats": 0, "rsa_sd": None, "rsa_why": "atım bekleniyor",
           "hrv_ms": None, "hrv_n": 0}
clients = []
k_hr  = Kalman1D(q=0.6, r_min=4.0,  p_lock=9.0,  name="hr")    # kilit: sd < 3 /dk
k_rr  = Kalman1D(q=0.15, r_min=1.5, p_lock=4.0,  name="rr")    # kilit: sd < 2 /dk
k_rsa = Kalman1D(q=0.15, r_min=2.0, p_lock=4.0,  name="rsa")
logf = None


class Processor:
    """Satır → örnek işleme (filtre + NLMS). Tek iş parçacığından çağrılır."""
    def __init__(self):
        self.notch = Biquad.notch(50.0, 20.0)
        self.p_bp = bandpass4(*PROF["hr_band"]);   self.r_bp = bandpass4(*PROF["rr_band"])
        self.p_ref = [bandpass4(*PROF["hr_band"]) for _ in range(3)]
        self.r_ref = [bandpass4(*PROF["rr_band"]) for _ in range(3)]
        self.p_nlms = NLMS(3, 8, 0.02);    self.r_nlms = NLMS(3, 8, 0.02)
        self.acc_hp = [Biquad.highpass(0.3, 0.707) for _ in range(3)]
        self.f_bp = Chain([Biquad.highpass(0.5, 0.707), Biquad.lowpass(8.0, 0.707)])   # bulucu: hafif, hızlı
        self.acc_every = HZ // ACC_HZ
        self.last_acc = (0.0, 0.0, 0.0)
    def line(self, l):
        body = l[6:]
        pz, _, ac = body.partition("|")
        toks = pz.split()[1:]
        p = [int(x) for x in toks if x.lstrip("-").isdigit()]
        a = [int(x) for x in ac.split() if x.lstrip("-").isdigit()]
        trip = [tuple(a[i:i + 3]) for i in range(0, len(a) - 2, 3)]
        out_p, out_r, out_a, out_x = [], [], [], []
        for k, v in enumerate(p):
            if trip:
                self.last_acc = trip[min(len(trip) - 1, k // self.acc_every)]
                status["acc_ok"] = True
            ax, ay, az = self.last_acc
            hp = [f.push(float(c)) for f, c in zip(self.acc_hp, (ax, ay, az))]   # yerçekimi atıldı (mg)
            mag = math.sqrt(sum(h * h for h in hp))
            x = self.notch.push(float(v)); out_x.append(self.f_bp.push(x))
            pb = self.p_bp.push(x); rb = self.r_bp.push(x)
            if status["acc_ok"]:
                pr = [f.push(h) for f, h in zip(self.p_ref, hp)]
                rr = [f.push(h) for f, h in zip(self.r_ref, hp)]
                pb = self.p_nlms.push(pb, pr); rb = self.r_nlms.push(rb, rr)
            out_p.append(pb); out_r.append(rb); out_a.append(mag)
            if logf: logf.write(f"{v},{pb:.1f},{rb:.1f},{ax},{ay},{az}\n")
        with lock:
            pulse_q.extend(out_p); resp_q.extend(out_r); acc_q.extend(out_a); rawn_q.extend(out_x); rawadc_q.extend(p); status["n"] += len(out_p)
        if RAW_VIEW:
            m0 = sum(p) / len(p) if p else 0.0
            for q in list(clients): q.append([round(v - m0, 1) for v in p])   # ham, yalnız satır ortalaması çıkarılmış
        else:
            for q in list(clients): q.append([round(v, 1) for v in out_p])


# ─── Ritim tahminleri ────────────────────────────────────────────────────────
def _acf_peak(x, lag_lo, lag_hi, step):
    n = len(x); m = sum(x) / n; x = [v - m for v in x]
    var = sum(v * v for v in x) / n or 1.0
    lags = list(range(lag_lo, lag_hi, step))
    sub = max(1, step // 2)
    ac = [sum(x[i] * x[i + lag] for i in range(0, n - lag, sub)) / ((n - lag) / sub) / var for lag in lags]
    # Bant kenarına yakın (ilk/son %8) tepeler sayılmaz: kenara yapışan yavaş sürüklenme ve
    # bant dışı bileşenler sahte "kararlı" ritim üretir (ör. solunumda 9/dk, nabızda 133/dk).
    kmin, kmax = int(len(ac) * 0.08), int(len(ac) * 0.92)
    peaks = [(ac[k], lags[k]) for k in range(max(1, kmin), min(len(ac) - 1, kmax)) if ac[k] > ac[k - 1] and ac[k] >= ac[k + 1]]
    if not peaks: return (0.0, None)
    top = max(peaks)
    # Periyodik sinyalde 2T, 3T gecikmeleri de tepe verir (alt harmonik). En iyinin %85'i
    # içindeki EN KISA gecikme gerçek periyottur.
    for r, lag in sorted(peaks, key=lambda t: t[1]):
        if r >= 0.85 * top[0]: return (r, lag)
    return top


def estimate_hr():
    """(bpm, r, tremor): 8 sn, 0.3-1.6 s gecikme; >120/dk = titreme."""
    with lock: x = list(pulse_q)[-PROF["hr_win"] * HZ:]
    if len(x) < 4 * HZ: return None, 0.0, False
    r, lag = _acf_peak(x, int(PROF["hr_lag"][0] * HZ), int(PROF["hr_lag"][1] * HZ), 2)
    if lag is None: return None, 0.0, False
    bpm = 60.0 * HZ / lag
    return bpm, r, bpm > PROF["hr_max"]


def estimate_rr():
    """(nefes/dk, güven 0-1): rr_win sn solunum bandı, 0.01 Hz adımlı DFT; en güçlü bileşen /
    bant ortalaması oranından güven (oran 1.5 → 0, 5.5 → 1). Bant kenarının %8'i hariç.
    (ACF, zayıf ve düzensiz nefes sinyalinde kenara yapışıyordu; spektral tepe daha dayanıklı.)"""
    win = PROF["rr_win"]
    with lock: x = list(resp_q)[-win * HZ:]
    if len(x) < (win // 2) * HZ: return None, 0.0
    seg = x[::4]; fs = HZ / 4; m = sum(seg) / len(seg); seg = [v - m for v in seg]; n = len(seg)
    f0, f1 = PROF["rr_band"]; span = f1 - f0
    cos = math.cos; sin = math.sin; two_pi = 2 * math.pi
    spec = []; f = f0
    while f <= f1 + 1e-9:
        w = two_pi * f / fs
        re_ = sum(v * cos(w * i) for i, v in enumerate(seg)); im_ = sum(v * sin(w * i) for i, v in enumerate(seg))
        spec.append((math.hypot(re_, im_) / n, f)); f += 0.01
    mean = sum(a for a, _ in spec) / len(spec) or 1e-9
    inner = [(a, f) for a, f in spec if f0 + 0.08 * span <= f <= f1 - 0.08 * span]
    a, fpk = max(inner)
    ratio = a / mean
    return fpk * 60.0, min(1.0, max(0.0, (ratio - 1.5) / 4.0))


def detect_beats(x, hz=HZ, period_s=None):
    """Atım tepeleri. Bant geçiren filtre her atımdan sonra yarım periyotta küçük bir
    'çınlama' tepesi bırakır; bu yüzden en küçük atım aralığı, otokorelasyon nabzından gelen
    beklenen periyodun 0.6'sı alınır (bilinmiyorsa 0.3 s). Eşik: aday yüksekliklerinin
    %90'ının 0.4'ü (yükseklik nefesle %30 dalgalanır, RMS eşiği yarısını kaçırıyordu)."""
    n = len(x); mind = int((0.6 * period_s if period_s else PROF["hr_lag"][0]) * hz); cand = []
    for i in range(1, n - 1):
        if x[i] > x[i - 1] and x[i] >= x[i + 1] and x[i] > 0: cand.append((i, x[i]))
    if len(cand) < 5: return []
    hs = sorted(h for _, h in cand)
    thr = 0.4 * hs[int(len(hs) * 0.9)]
    beats = []; last = -mind
    for i, h in cand:
        if h < thr: continue
        if i - last < mind:
            if beats and h > beats[-1][1]: beats[-1] = (i, h); last = i
            continue
        beats.append((i, h)); last = i
    return beats


def estimate_rsa(win_s=40):
    """(nefes/dk, güven oranı, geçerli atım): atım aralığı + yüksekliği → 0.12-0.7 Hz baskın frekans."""
    with lock: x = list(pulse_q)[-win_s * HZ:]
    if len(x) < 20 * HZ: return None, 0.0, 0
    hb, _, _ = estimate_hr()
    beats = detect_beats(x, period_s=(60.0 / hb) if hb else None)
    if len(beats) < 12: return None, 0.0, len(beats)
    t = [i / HZ for i, _ in beats]; a = [h for _, h in beats]
    rr = [(t[k + 1] - t[k]) for k in range(len(t) - 1)]
    med = sorted(rr)[len(rr) // 2]
    lo, hi = PROF["rr_ok"]
    ok = [k for k, v in enumerate(rr) if lo <= v <= hi and abs(v - med) <= 0.30 * med]
    if len(ok) < 10: return None, 0.0, len(ok)
    tt = [t[k + 1] for k in ok]; rrv = [rr[k] for k in ok]; aa = [a[k + 1] for k in ok]
    # HRV (stres göstergesi): ardışık atım aralıklarının farkı, RMSSD (ms). Sakin → yüksek
    # (insan 30-80 ms), stres/efor → düşük. Yalnız ≥20 ardışık geçerli aralıkta hesaplanır.
    cons = [(rrv[j + 1] - rrv[j]) for j in range(len(ok) - 1) if ok[j + 1] == ok[j] + 1]
    rmssd = 1000 * math.sqrt(sum(d * d for d in cons) / len(cons)) if len(cons) >= 20 else None
    # Kapı: ≥20 ardışık aralık, nabız kilidi en az 45 sn'dir sürüyor, değer fizyolojik (<150 ms);
    # aksi hâlde gürültü atımlarından türeyen sahte HRV gösterilir (örn. 221 ms).
    if rmssd is not None and rmssd < 150 and k_hr.locked and time.time() - k_hr.t_lock >= 45:
        status["hrv_ms"] = round(rmssd); status["hrv_n"] = len(cons)
    else:
        status["hrv_ms"] = None
    fs = 4.0; t0, t1 = tt[0], tt[-1]
    if t1 - t0 < 15: return None, 0.0, len(ok)
    if hb and len(ok) < 0.6 * (t1 - t0) * hb / 60.0: return None, 0.0, len(ok)
    def resample(ts, vs):
        out = []; j = 0; tcur = t0
        while tcur <= t1:
            while j < len(ts) - 2 and ts[j + 1] < tcur: j += 1
            w = (tcur - ts[j]) / max(1e-6, ts[j + 1] - ts[j]); w = min(1.0, max(0.0, w))
            out.append(vs[j] * (1 - w) + vs[j + 1] * w); tcur += 1 / fs
        m = sum(out) / len(out); return [v - m for v in out]
    best = (0.0, None)
    for series in (resample(tt, rrv), resample(tt, aa)):
        spec = []; f = PROF["rr_band"][0]
        while f <= PROF["rr_band"][1]:
            re_ = sum(v * math.cos(2 * math.pi * f * k / fs) for k, v in enumerate(series))
            im_ = sum(v * math.sin(2 * math.pi * f * k / fs) for k, v in enumerate(series))
            spec.append((re_ * re_ + im_ * im_, f)); f += 0.02
        mean = sum(p for p, _ in spec) / len(spec) or 1e-9
        f0, f1 = PROF["rr_band"]; span = f1 - f0
        inner = [(p, f) for p, f in spec if f0 + 0.08 * span <= f <= f1 - 0.08 * span]   # kenar koruması
        pk = max(inner)
        if pk[0] / mean > best[0]: best = (pk[0] / mean, pk[1])
    if best[1] is None: return None, 0.0, len(ok)
    return best[1] * 60.0, best[0], len(ok)


# ─── İş parçacıkları ─────────────────────────────────────────────────────────
def serial_worker():
    import serial
    ports = glob.glob("/dev/cu.usbmodem*")
    if not ports:
        status["state"] = "USB portu yok — kartı tak"; return
    p = serial.Serial(); p.port = ports[0]; p.baudrate = 115200; p.timeout = 0.2
    p.dtr = False; p.rts = False; p.open()
    status["state"] = "kart açılıyor…"
    buf = b""; t0 = time.time()
    while time.time() - t0 < 60:
        buf += p.read(8192)
        if b"giris olarak kuruldu" in buf: time.sleep(1.0); break
    proc = Processor()
    while True:
        p.reset_input_buffer()
        p.write(f"piezo capture {SEG_S} {HZ}\r\n".encode())
        status["state"] = "kayıt akıyor"
        t1 = time.time(); pending = b""; done = False
        while not done and time.time() - t1 < SEG_S + 5:
            chunk = p.read(4096)
            if not chunk: continue
            pending += chunk
            while b"\n" in pending:
                line, pending = pending.split(b"\n", 1)
                l = strip(line.decode("utf-8", "replace")).strip()
                if l.startswith("[cap] "): proc.line(l)
                elif "capture bitti" in l: done = True; break
                elif "ERR: piezo" in l:
                    status["state"] = "cihaz: " + l; time.sleep(2); done = True; break


def analysis_worker():
    """Her saniye: hareket kapısı → ölçümler → Kalman güncellemeleri → durum."""
    last = time.time(); tick = 0
    while True:
        time.sleep(1.0); now = time.time(); dt = now - last; last = now; tick += 1
        for k in (k_hr, k_rr, k_rsa): k.predict(dt)
        with lock:
            pl = list(pulse_q)[-HZ:]; ac = list(acc_q)[-HZ:]
        if len(pl) < HZ: continue
        amp = math.sqrt(sum(v * v for v in pl) / len(pl))
        acc_rms = math.sqrt(sum(v * v for v in ac) / len(ac)) if ac else 0.0
        base = sorted(pamp_q)[len(pamp_q) // 2] if pamp_q else amp
        big = amp > max(400.0, 4.0 * base)
        if SOFT_GATE:
            # Yalnız kartın kendisi belirgin hareket ederse (ivme > 120 mg) kes; piezo genliği
            # (kablo kıpırtısı, masa titreşimi) kapıyı tetiklemez — güven şartı zaten sayıyı tutar.
            motion = bool(status["acc_ok"] and acc_rms > 120.0)
        else:
            motion = (status["acc_ok"] and acc_rms > 25.0) or big or amp > 1500.0
        if not big: pamp_q.append(amp)
        status.update(motion=motion, acc_rms=round(acc_rms, 1), amp=round(amp))
        # Temas kaybı / ani değişim: kilitliyken son 1 sn genliği, kilit sırasındaki tipik genliğin
        # 1/3'ünün altına düşerse (film kalktı) ya da 4 katını aşarsa (sert hareket) kilidi ANINDA bırak;
        # 6-8 sn'lik pencerenin eski atımları ritmi birkaç sn daha "var" gösterirdi.
        if k_hr.locked:
            lock_amps.append(amp)
            ref = sorted(lock_amps)[len(lock_amps) // 2]
            if len(lock_amps) >= 2 and (amp < 0.33 * ref or amp > 4.0 * ref):
                k_hr.release(cooldown=PROF["hr_win"]); k_rsa.release(cooldown=PROF["hr_win"]); lock_amps.clear(); status["hr_why"] = "temas kaybı"
        else:
            lock_amps.clear()
        # NABIZ
        b, r, tremor = estimate_hr()
        status["hr_raw"] = None if b is None else round(b); status["hr_r"] = round(r, 2)
        if motion:
            status["hr_why"] = "hareket / dokunma"
        elif b is None:
            status["hr_why"] = "sinyal arıyor"
        elif tremor:
            status["hr_why"] = f"titreme {round(b)}/dk — nabız DEĞİL (gevşet)" if r >= 0.4 else "sinyal arıyor"
        else:
            k_hr.update(b, r); status["hr_why"] = "kilitli" if k_hr.locked else f"izleniyor (±{k_hr.sd:.0f})" if k_hr.sd else "sinyal arıyor"
        status["hr"] = None if motion else k_hr.value; status["hr_sd"] = None if k_hr.sd is None else round(k_hr.sd, 1)
        # SOLUNUM (hareket)
        rb, rr = estimate_rr()
        status["rr_raw"] = None if rb is None else round(rb); status["rr_r"] = round(rr, 2)   # rr = güven (0-1)
        if not motion and rb is not None: k_rr.update(rb, rr)
        status["rr"] = None if motion else k_rr.value; status["rr_sd"] = None if k_rr.sd is None else round(k_rr.sd, 1)
        status["rr_why"] = "hareket" if motion else ("kilitli" if k_rr.locked else "sinyal arıyor")
        # SOLUNUM (nabızdan) — 2 sn'de bir (pahalı)
        if tick % 2 == 0:
            sb, sq, nb = estimate_rsa()
            status["rsa_raw"] = None if sb is None else round(sb); status["rsa_q"] = round(sq, 1); status["rsa_beats"] = nb
            conf = 0.0 if sb is None else min(1.0, max(0.0, (sq - 2.0) / 8.0))
            # Nabız kanalı kilitli değilse atımlar güvenilmez → nabızdan solunum beslenmez
            if not motion and sb is not None and k_hr.locked: k_rsa.update(sb, conf)
            status["rsa_why"] = ("hareket" if motion else "nabız kilidi yok" if not k_hr.locked else
                                 "atım yetersiz (%d)" % nb if sb is None else
                                 "kilitli" if k_rsa.locked else "nefes ritmi arıyor")
        if motion or not k_hr.locked: status["hrv_ms"] = None
        status["rsa"] = None if (motion or not k_hr.locked) else k_rsa.value; status["rsa_sd"] = None if k_rsa.sd is None else round(k_rsa.sd, 1)


def simple_loop():
    """Sade görünüm: 2 sn'de bir satır. Değer varsa sayı, yoksa '--'; yanında sinyal kalitesi
    (●○○○○ … ●●●●●): nabız için ham güven r, solunum için spektral güven. Hareket varsa tek kelime."""
    def dots(c):
        # 0-0.25 = gürültü tabanı → boş; 0.25-0.85 → 1-5 dolu (0.4 = sayı eşiği ≈ 2 dolu)
        k = max(0, min(5, round((c - 0.25) / 0.6 * 5))); return "●" * k + "○" * (5 - k)
    hamcol = RAW_VIEW
    print(("saat     | HAM min/ort/max  " if hamcol else "saat    ") + " | NABIZ          | SOLUNUM        | SOL(nabızdan)  | HRV/stres | durum")
    while True:
        time.sleep(1.0)
        with lock: n = len(pulse_q); rl = list(rawadc_q)[-HZ:]
        ham = (f"{min(rl):4d}/{sum(rl)/len(rl):6.1f}/{max(rl):4d} " if (hamcol and rl) else ("  --  /  --  /  --  " if hamcol else ""))
        if n < HZ:
            print(f'{time.strftime("%H:%M:%S")} | {status["state"]}', flush=True); continue
        if status["motion"]:
            print(f'{time.strftime("%H:%M:%S")} | {ham}' + ('| ' if hamcol else '') + '     --        |      --        |      --        |    --     | HAREKET', flush=True); continue
        hr = f'{status["hr"]:3d}/dk' if status["hr"] else '   -- '
        rr = f'{status["rr"]:3d}/dk' if status["rr"] else '   -- '
        rs = f'{status["rsa"]:3d}/dk' if status["rsa"] else '   -- '
        rsa_c = 0.0 if (status["rsa_q"] is None or not k_hr.locked) else min(1.0, max(0.0, (status["rsa_q"] - 2.0) / 8.0))
        hv = status["hrv_ms"]; hvs = f'{hv:3d} ms' if hv else '  --  '
        print(f'{time.strftime("%H:%M:%S")} | {ham}' + ('| ' if hamcol else '') + f'{hr} {dots(status["hr_r"])}  | {rr} {dots(status["rr_r"])}  | {rs} {dots(rsa_c)}  | {hvs}    | '
              f'{"sabit" if status["acc_rms"] < 25 else "kıpırtı " + str(status["acc_rms"]) + " mg"}', flush=True)



class Instant:
    """Anlık TEMAS göstergesi (bulucu + ölçüm ekranı ortak). Her 0.5 sn: hafif bantlı genlik,
    son 3 değerin ortancası, açılıştaki 10 sn'lik masa tabanına GÖRELİ (x1.8 temas, x8 basınç, x25 hareket).
    Ne söyler: film ciltte mi, masada mı, bastırılıyor/kıpırdıyor mu. Ne SÖYLEMEZ: damar üstünde mi —
    onu yalnız gecikmeli ritim (NABIZ sütunu / 'BU NOKTA' kararı) söyler."""
    def __init__(self):
        self.amps = deque(maxlen=3); self.cal = []; self.base = None
    def step(self):
        with lock: last = list(rawn_q)[-HZ // 2:]
        if len(last) < HZ // 2: return None, None, "sinyal bekleniyor"
        m = sum(last) / len(last); amp = math.sqrt(sum((v - m) ** 2 for v in last) / len(last))
        # Taban: açılıştan sonraki ilk 10 sn'nin ORTANCASI (film masada olmalı). Ortam gürültüsü
        # (USB, ofis, kablo) güne göre 14-60 arasında değişebildiği için mutlak eşik kullanılmaz.
        if self.base is None:
            self.cal.append(amp)
            if len(self.cal) < 20: return amp, None, "taban öğreniliyor (film masada, 10 sn)"
            self.base = max(5.0, sorted(self.cal)[len(self.cal) // 2])
        self.amps.append(amp); a = sorted(self.amps)[len(self.amps) // 2]; ratio = a / self.base
        zone = ("sinyal yok" if ratio < 1.8 else "SİNYAL VAR" if ratio <= 8.0 else
                "basınç/kıpırtı" if ratio <= 25.0 else "HAREKET")
        return amp, ratio, zone


def finder_loop():
    """Nabız noktası bulucu — ANLIK. Çubuk = son 0.5 sn'nin nabız bandı genliği (film damara
    değince anında büyür, masaya koyunca anında düşer). Sağda son 4 sn'nin ritmi (gecikmeli,
    bilgi amaçlı). Hedef: çubuk 'sinyal' bölgesinde (15-150) ve ritim 6 satır üst üste tutarlı."""
    print("NABIZ BULUCU — çubuk ANLIK genlik. Filmi yavaşça kaydır; 'SİNYAL' bölgesinde dur.  (Ctrl+C çıkış)")
    print("  çubuk = anlık temas genliği (masada ~x1, ciltte x2-10, basınç/hareket >x10). Damar üstünde olup olmadığını")
    print("  yalnız 6 sn'lik 'BU NOKTA' kararı söyler — çubuğa bakarak yer seçme, karar satırını bekle.")
    print("  (taban, film boştayken ilk 5-10 sn'de kendiliğinden öğrenilir — başta filmi masada bırak)")
    hist = deque(maxlen=6); instant = Instant()
    win = []                                                # son 6 sn: (oran, bpm, r) → nokta kararı
    print("  YÖNTEM: filmi koy, bantla tuttur, elini çek, 6 sn bekle → 'BU NOKTA' kararı. Sonra 1 cm kaydır, tekrar.")
    while True:
        time.sleep(0.5)
        with lock: x4 = list(pulse_q)[-4 * HZ:]
        amp, ratio, zone = instant.step()
        if ratio is None:
            print(f'{time.strftime("%H:%M:%S")}  {zone}', flush=True); continue
        # log ölçek: 5 → 0, 500 → 50 karakter
        n = int(max(0.0, min(1.0, (math.log10(max(amp, 5.0)) - math.log10(5.0)) / 2.0)) * 50)
        zone = {"sinyal yok": "sinyal yok (damar altında değil / sabit)", "SİNYAL VAR": "SİNYAL VAR (damar? → ritme bak)",
                "basınç/kıpırtı": "basınç / kıpırtı (gevşet)", "HAREKET": "HAREKET"}[zone]
        rtxt = ""
        if len(x4) >= 4 * HZ:
            r, lag = _acf_peak(x4, int(PROF["hr_lag"][0] * HZ), int(PROF["hr_lag"][1] * HZ), 2)
            bpm = (60.0 * HZ / lag) if lag else None
            hist.append(bpm if (bpm and r >= 0.35) else None)
            steady = all(hist) and len(hist) == 6 and max(hist) <= 1.1 * min(hist)
            rtxt = f'  ritim(son 4 sn): {round(bpm) if bpm else "--":>3}/dk r={r:.2f}' + ("  ← TUTARLI, BURADA DUR" if steady and r >= 0.5 else "")
        hr = status["hr"]
        n = int(max(0.0, min(1.0, math.log10(max(ratio, 1.0)) / math.log10(30.0))) * 50)   # x1 → 0, x30 → tam
        print(f'{time.strftime("%H:%M:%S")} |{"█" * n}{"·" * (50 - n)}| x{ratio:4.1f} {zone:30}{rtxt}{"  ✓ KİLİT " + str(hr) if hr else ""}', flush=True)
        # ── 6 sn'lik nokta kararı (koy-bırak-bekle yöntemi) ──
        if len(x4) >= 4 * HZ: win.append((ratio, bpm if bpm else None, r))
        if len(win) >= 12:
            ratios = sorted(w[0] for w in win); med = ratios[len(ratios) // 2]; spread = ratios[-1] / max(ratios[0], 0.1)
            rs = [w[2] for w in win]; bpms = [w[1] for w in win if w[1]]
            good_r = sum(1 for v in rs if v >= 0.4)
            consistent = len(bpms) >= 8 and max(bpms) <= 1.15 * min(bpms)
            if spread > 6:        verdict = "EL KIPIRDIYOR — bantla tuttur, elini çek, tekrar bekle"
            elif med < 1.8:   verdict = "sinyal yok → damar altında değil, 1 cm kaydır"
            elif med > 8.0:   verdict = "fazla basınç / kıpırtı → bandı gevşet, elini çek"
            elif good_r >= 6 and consistent: verdict = f"✓ NABIZ VAR ({round(sum(bpms)/len(bpms))}/dk) — BURAYA SABİTLE"
            elif good_r >= 3:     verdict = f"ritim belirmeye başladı ({round(sum(bpms)/len(bpms)) if bpms else '--'}/dk) — 6 sn daha bekle"
            else:                 verdict = "sinyal var ama ritim yok → yarım cm kaydır ya da basıncı azalt"
            print(f'{"=" * 9} BU NOKTA: {verdict}   (sinyal x{med:.1f}, güvenilir ritim {good_r}/12) {"=" * 9}', flush=True)
            win = []


def terminal_loop():
    bars = "▁▂▃▄▅▆▇█"
    print("saat      NABIZ   SOLUNUM  SOL(nabiz)  durum                 ayrinti: ivme mg / piezo / nabiz ham r / sol ham r / rsa ham,guven,atim")
    while True:
        time.sleep(1.0)
        with lock: x = list(pulse_q)
        if len(x) < HZ:
            print(time.strftime("%H:%M:%S"), " ", status["state"], flush=True); continue
        seg = x[-2 * HZ:]; step = max(1, len(seg) // 40)
        pts = [sum(seg[i:i + step]) / step for i in range(0, len(seg) - step + 1, step)]
        mx = max(abs(v) for v in pts) or 1.0
        spark = "".join(bars[min(7, int((v / mx + 1) / 2 * 7.999))] for v in pts)
        f = lambda v: f"{v:3d}/dk" if v else "  --  "
        if status["motion"]:
            print(f'{time.strftime("%H:%M:%S")}    --      --       --      HAREKET — sabit dur    (ivme {status["acc_rms"]} mg, piezo ±{status["amp"]})', flush=True); continue
        why = status["hr_why"] if status["hr"] is None else status["rsa_why"] if status["rsa"] is None else "kilitli"
        print(f'{time.strftime("%H:%M:%S")}  {f(status["hr"])}  {f(status["rr"])}   {f(status["rsa"])}    {why[:22]:22} '
              f'({status["acc_rms"]} / ±{status["amp"]} / {status["hr_raw"] or "-"} r={status["hr_r"]:.2f} / '
              f'{status["rr_raw"] or "-"} r={status["rr_r"]:.2f} / {status["rsa_raw"] or "-"},{status["rsa_q"]},{status["rsa_beats"]}) |{spark}|', flush=True)


PAGE = """<!doctype html><html lang="tr"><meta charset="utf-8"><title>Piezo canlı</title>
<style>body{margin:0;background:#111;color:#eee;font:14px system-ui}#top{padding:10px 16px;display:flex;gap:28px;align-items:baseline;flex-wrap:wrap}
.big{font-size:44px;font-weight:700}.lbl{color:#aaa}.sub{color:#777;font-size:12px}#st{color:#aaa}
canvas{display:block;width:100vw;height:60vh;background:#000}.bad{color:#f66}.ok{color:#6f6}.mid{color:#fc6}</style>
<div id=top>
<div><div class=lbl>NABIZ</div><div id=hr class=big>—</div><div id=hrwhy class=sub></div></div>
<div><div class=lbl>SOLUNUM (hareket)</div><div id=rr class=big>—</div><div id=rrwhy class=sub></div></div>
<div><div class=lbl>SOLUNUM (nabızdan)</div><div id=rsa class=big>—</div><div id=rsawhy class=sub></div></div>
<div><div class=lbl>HRV / stres göstergesi</div><div id=hrv class=big>—</div><div class=sub>RMSSD; sakin → yüksek, stres → düşük</div></div>
<div><div id=motion style="font-size:20px"></div><div id=st class=sub></div></div></div>
<canvas id=c></canvas>
<script>
const c=document.getElementById('c'),ctx=c.getContext('2d');const N=200*8;let buf=new Array(N).fill(0),i=0,motion=false;
function fit(){c.width=innerWidth*devicePixelRatio;c.height=innerHeight*0.6*devicePixelRatio}fit();onresize=fit;
function setv(id,v,sd,why){const e=document.getElementById(id);e.textContent=v==null?'—':v+' /dk';e.className='big '+(v==null?'bad':'ok');
 document.getElementById(id+'why').textContent=why+(sd!=null?' · ±'+sd:'')}
const es=new EventSource('/stream');
es.onmessage=e=>{const m=JSON.parse(e.data);if(m.s){for(const v of m.s){buf[i]=v;i=(i+1)%N}}
 if(m.st){const s=m.st;motion=s.motion;setv('hr',s.hr,s.hr_sd,s.hr_why+' (ham '+(s.hr_raw??'—')+', r '+s.hr_r+')');
  setv('rr',s.rr,s.rr_sd,s.rr_why+' (ham '+(s.rr_raw??'—')+', r '+s.rr_r+')');
  setv('rsa',s.rsa,s.rsa_sd,s.rsa_why+' (ham '+(s.rsa_raw??'—')+', güven '+s.rsa_q+', atım '+s.rsa_beats+')');
  const hv=document.getElementById('hrv');hv.textContent=s.hrv_ms==null?'—':s.hrv_ms+' ms';hv.className='big '+(s.hrv_ms==null?'bad':'ok');
  document.getElementById('motion').textContent=motion?'HAREKET — sabit dur':'sabit';document.getElementById('motion').className=motion?'bad':'ok';
  document.getElementById('st').textContent=s.state+' · ivme '+s.acc_rms+' mg · piezo ±'+s.amp+' · örnek '+s.n+(s.acc_ok?'':' · IMU YOK')}};
function draw(){const w=c.width,h=c.height;ctx.fillStyle='#000';ctx.fillRect(0,0,w,h);
 if(motion){ctx.fillStyle='#f66';ctx.font=(28*devicePixelRatio)+'px system-ui';ctx.fillText('hareket algılandı — dalga gizlendi',20*devicePixelRatio,h/2);requestAnimationFrame(draw);return}
 ctx.strokeStyle='#223';ctx.lineWidth=1;for(let s=0;s<=8;s++){const x=s*w/8;ctx.beginPath();ctx.moveTo(x,0);ctx.lineTo(x,h);ctx.stroke()}
 let mx=1;for(const v of buf)mx=Math.max(mx,Math.abs(v));ctx.strokeStyle='#4af';ctx.lineWidth=2*devicePixelRatio;ctx.beginPath();
 for(let k=0;k<N;k++){const v=buf[(i+k)%N];const x=k*w/N,y=h/2-v/mx*h/2*0.9;k?ctx.lineTo(x,y):ctx.moveTo(x,y)}ctx.stroke();
 ctx.fillStyle='#888';ctx.font=(12*devicePixelRatio)+'px system-ui';ctx.fillText('nabız bandı, hareket iptali sonrası · son 8 sn · genlik ±'+Math.round(mx),10*devicePixelRatio,16*devicePixelRatio);
 requestAnimationFrame(draw)}draw();
</script></html>"""


PAGE_NABIZ = """<!doctype html><html lang="tr"><meta charset="utf-8"><title>Nabız monitörü</title>
<style>html,body{margin:0;height:100%;background:#000;color:#0f0;font-family:ui-monospace,Menlo,monospace;overflow:hidden}
#top{position:absolute;left:24px;top:16px;display:flex;gap:40px;align-items:baseline}
#hr{font-size:120px;font-weight:700;line-height:1}#unit{font-size:28px;color:#0a0}#q{font-size:22px;color:#0a0}
#st{position:absolute;right:24px;top:24px;color:#777;font-size:14px}
#beat{display:inline-block;width:26px;height:26px;border-radius:50%;background:#0f0;margin-left:16px;vertical-align:middle;opacity:.15}
canvas{position:absolute;left:0;top:0;width:100vw;height:100vh}</style>
<canvas id=c></canvas>
<div id=top><div><span id=hr>—</span><span id=unit> /dk</span><span id=beat></span></div><div id=q></div></div>
<div id=st></div>
<script>
const c=document.getElementById('c'),ctx=c.getContext('2d');
const HZ=200,SEC=6,N=HZ*SEC;let buf=new Float32Array(N),pos=0,amp=50,lastBeatPos=-1,motion=false;
function fit(){c.width=innerWidth*devicePixelRatio;c.height=innerHeight*devicePixelRatio}fit();onresize=fit;
const es=new EventSource('/stream');
es.onmessage=e=>{const m=JSON.parse(e.data);
 if(m.s){for(const v of m.s){buf[pos]=v;pos=(pos+1)%N;const a=Math.abs(v);amp=Math.max(amp*0.999,a*0.9+amp*0.1)}}
 if(m.st){const s=m.st;motion=s.motion;const h=document.getElementById('hr');
  h.textContent=s.hr==null?'—':s.hr;h.style.color=s.hr==null?'#0a0':'#0f0';
  const dots='●'.repeat(Math.max(0,Math.min(5,Math.round((s.hr_r-0.25)/0.6*5))))+'○'.repeat(5-Math.max(0,Math.min(5,Math.round((s.hr_r-0.25)/0.6*5))));
  document.getElementById('q').textContent=dots+(motion?'  HAREKET':(s.hr==null?'  ritim aranıyor':''));
  document.getElementById('st').textContent=(s.raw_view?'HAM ADC (filtresiz) · ':'nabız bandı · ')+s.state+' · ivme '+s.acc_rms+' mg'}};
function draw(){const w=c.width,h=c.height,mid=h*0.6,scale=(h*0.22)/Math.max(amp,20);
 ctx.fillStyle='#000';ctx.fillRect(0,0,w,h);
 ctx.strokeStyle='#062';ctx.lineWidth=1;for(let x=0;x<w;x+=w/(SEC*5)){ctx.beginPath();ctx.moveTo(x,0);ctx.lineTo(x,h);ctx.stroke()}
 for(let y=0;y<h;y+=h/10){ctx.beginPath();ctx.moveTo(0,y);ctx.lineTo(w,y);ctx.stroke()}
 if(motion){ctx.fillStyle='#f44';ctx.font=(36*devicePixelRatio)+'px ui-monospace';ctx.fillText('HAREKET — bekleniyor',w*0.3,mid);requestAnimationFrame(draw);return}
 // EKG süpürme: yazma konumu sağa doğru ilerler, önünde karanlık boşluk
 ctx.strokeStyle='#0f0';ctx.lineWidth=2.5*devicePixelRatio;ctx.shadowColor='#0f0';ctx.shadowBlur=8;ctx.beginPath();
 for(let k=0;k<N;k++){const x=k*w/N,y=mid-buf[k]*scale;k?ctx.lineTo(x,y):ctx.moveTo(x,y)}ctx.stroke();ctx.shadowBlur=0;
 const gx=pos*w/N;ctx.fillStyle='#000';ctx.fillRect(gx,0,w*0.04,h);ctx.fillStyle='#0f0';ctx.fillRect(gx,0,2*devicePixelRatio,h);
 // atım göstergesi: yazma konumunun hemen gerisinde yerel tepe varsa yanıp sön
 const i=(pos-40+N)%N,a=buf[i],b1=buf[(i-10+N)%N],b2=buf[(i+10)%N];
 document.getElementById('beat').style.opacity=(a>b1&&a>b2&&a>0.5*amp)?1:0.15;
 requestAnimationFrame(draw)}draw();
</script></html>"""


class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        if self.path == "/stream":
            self.send_response(200); self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache"); self.end_headers()
            q = deque(); clients.append(q); last_st = 0
            try:
                while True:
                    s = []
                    while q: s.extend(q.popleft())
                    msg = {"s": s}
                    if time.time() - last_st > 0.5: msg["st"] = dict(status, raw_view=RAW_VIEW); last_st = time.time()
                    self.wfile.write(f"data: {json.dumps(msg)}\n\n".encode()); self.wfile.flush()
                    time.sleep(0.1)
            except Exception:
                pass
            finally:
                if q in clients: clients.remove(q)
        else:
            body = (PAGE_NABIZ if "--nabiz" in sys.argv else PAGE).encode(); self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8"); self.send_header("Content-Length", str(len(body)))
            self.end_headers(); self.wfile.write(body)


if __name__ == "__main__":
    if "--log" in sys.argv:
        import os
        path = sys.argv[sys.argv.index("--log") + 1]
        if os.path.exists(path):   # ASLA üstüne yazma: 22 Ekim'de 20 dk'lık kedi kaydı böyle kaybedildi
            base, ext = os.path.splitext(path); path = f"{base}_{time.strftime('%H%M%S')}{ext or '.csv'}"
        print(f"kayıt dosyası: {path}")
        logf = open(path, "w"); logf.write("ham,nabiz_bandi,solunum_bandi,ax,ay,az\n")
    threading.Thread(target=serial_worker, daemon=True).start()
    threading.Thread(target=analysis_worker, daemon=True).start()
    if "--bul" in sys.argv:
        try: finder_loop()
        except KeyboardInterrupt: print("\nkapatıldı")
        sys.exit(0)
    from socketserver import ThreadingMixIn
    class S(ThreadingMixIn, HTTPServer): daemon_threads = True
    srv = S(("127.0.0.1", PORT), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()   # tarayıcı her modda açık
    print(f"Canlı görünüm: http://localhost:{PORT}   (Ctrl+C ile çık)")
    try:
        if "--terminal" in sys.argv:
            terminal_loop() if "--ayrinti" in sys.argv else simple_loop()
        else:
            while True: time.sleep(3600)
    except KeyboardInterrupt: print("\nkapatıldı")
