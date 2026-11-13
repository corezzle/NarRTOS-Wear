#!/usr/bin/env python3
"""
nabiz_ref.py: karttaki ölçüm algoritmasının (alg A3) bilgisayardaki bağımsız referansı. numpy gerekir.

Kullanım:
    python3 tools/nabiz_ref.py <kayit.wav> [insan|kopek|kedi]      tek dosya
    python3 tools/nabiz_ref.py <a.wav> <b.wav> ... [profil]        birden çok dosya

Kart kodu: src/services/nabiz_alg.c (saf C; tools/nabiz_test.py onu bilgisayarda derleyip bu dosyayla karşılaştırır).
İKİSİ BİRLİKTE değişmeli ve ALG sürümü artmalı. Kart float32, burası float64: sayılar son hanede oynayabilir.

Alg A3 (2026-11-11; A2 = 2026-11-10 + iki ek kabul yolu), sırayla:
  Akış     48 kHz → 48'lik ortalama → x1k (1 kHz, saklanır)
           48 kHz → 12'lik ortalama → 4 kHz → HP 200 Hz (4. derece) → LP 1500 Hz → |x| → 40'lık ortalama → solunum zarfı (100 Hz)
  Kalite   ilk 1 sn hariç: tepe, doygun örnek sayısı; kalp bandı RMS, solunum bandı RMS
  Nabız 1  x1k → HP/LP (profil) → |x| → 10'luk ortalama → kalp zarfı (100 Hz) → ilk 1 sn at → 5'lik düzleştirme →
           otokorelasyon. Tepe seçimi: aralıktaki en yüksek tepenin %80'ine ulaşan EN KISA gecikmeli yerel tepe
           (yarı hız / kat hatasına karşı) + parabol ara değeri (kedide 10 ms adımın ~7/dk hatasını giderir).
  Nabız 2  aynı zarfta vuruş tepeleri (eşik = ortanca + 0,25×(P98 − ortanca), en az 80 ms ara) → aralıklar.
           Aralıklar kısa-uzun diye almaşıyorsa çevrim başına iki tepe (S1+S2) sayılır. Düzensiz ritimde (köpekte
           solunuma bağlı) otokorelasyon zayıflar, bu yöntem çalışır.
  Karar    Yöntem 2 yalnız vuruşlar BELİRGİNSE güvenilir sayılır (ortanca tepe / ortanca zarf ≥ 4,5; yeterli vuruş,
           sınırlı yayılım). r ≥ 0,5: OK; ama güvenilir vuruş sayımı yarı ya da çift hız diyorsa KAT_SUPHESI (sayı yok).
           r < 0,5: güvenilir vuruş sayımı varsa OK (yöntem "vurus": düzensiz ritim), yoksa BELIRSIZ.
           Ölçüm notu: zayıf temaslı gerçek kayıtlarda doğru cevap otokorelasyon adayıydı, vuruş sayımı yukarı sapıyordu;
           bu yüzden vuruş sayımı zayıf sinyalde tek başına sonuç üretmez.
           A3 ekleri (yukarıdakiler sonuç vermezse, sırayla; ikisi de DÜŞÜK GÜVEN sayılır):
           "pencere": 8 sn pencerelerden r ≥ 0,5 olanlar en az 3 tane ve pencerelerin en az %40'ı ise, hepsi de
             ortancalarına ±%12 yakınsa sonuç = ortanca. Kayıt sırasında nabız değişirse ya da kaydın bir bölümü
             gürültülüyse bütün kayıt otokorelasyonu düşer ama temiz pencereler tek tek nettir (insan kaydı 11 Kas 14:02).
           "tutarli": r ≥ 0,3, en az 4 pencere var ve en az %80'i bütün kayıt adayına ±%6 yakın, ayrıca otokorelasyon
             iki periyot sonra da (±%6) ≥ 0,15 ve ≤ r ise sonuç = aday. Hışırtılı ama ritmi düzgün kayıt (11 Kas 13:14, 13:19).
           İki kural da yalnız GERÇEK yerel tepeden gelen adayı kabul eder (aralığın kenarına yaslanan "tepe" yavaş
           dalgalanan gürültüdür). Kalpsiz yapay kayıt taramasında (gürültü, sürtünme, tıkırtı, şebeke, yalnız nefes)
           bu iki koruma olmadan yanlış kabul çıkıyordu. Üçüncü koruma: aday, solunum bandında bulunan hıza ya da
           iki katına ±%10 yakınsa (kalp duyulmuyor, yalnız nefes sesi var) bu iki kural sonuç vermez.
           Bu iki kuralın eşikleri tek kişinin 13 kaydı ve yapay gürültüyle seçildi; referans cihazla doğrulanmadı.
  Mırlama  (kedi) x1k → 15-150 Hz; 1024'lük Hann çerçeveler; 18-40 Hz temel + 2. ve 3. harmonik tarağındaki enerji payı.
  Soluma   (köpek) solunum zarfında 200-460/dk periyodiklik.
  Solunum  solunum zarfı → kalp periyodunun tam katı uzunlukta ortalama (kalp sızıntısını sıfırlar) → 20 Hz →
           otokorelasyon 8-60/dk, yalnız gerçek (iç) tepe kabul edilir.
Bütün eşikler BAŞLANGIÇ değerleridir: insan kayıtları ve yapay sinyallerle ayarlandı, hayvan kaydıyla doğrulanmadı.
"""
import math, sys, wave

ALG = "A3"
FS = 48000
# hp, lp: kalp bandı (Hz). lag_min/lag_max: 10 ms adım, nabız = 6000/gecikme. sn: varsayılan süre.
PROFIL = {
    "insan": dict(hp=20.0, lp=150.0, lag_min=30, lag_max=171, sn=15, mirlama=False, soluma=False),   # 35-200/dk
    "kopek": dict(hp=20.0, lp=150.0, lag_min=24, lag_max=150, sn=30, mirlama=False, soluma=True),    # 40-250/dk
    "kedi":  dict(hp=30.0, lp=300.0, lag_min=20, lag_max=60,  sn=30, mirlama=True,  soluma=False),   # 100-300/dk
}
R_ORTA, R_YUKSEK = 0.5, 0.7
DOYGUN_ORAN = 0.001            # ilk 1 sn sonrası örneklerin %0,1'i
SINYAL_YOK_DB = -90.0
TEPE_ORAN = 0.80               # en kısa gecikmeli tepe, en yüksek tepenin bu oranına ulaşmalı
V_ESIK = 0.25                  # vuruş eşiği: ortanca + V_ESIK × (P98 − ortanca)
V_MIN_ARA = 8                  # 80 ms: bundan yakın tepeler birleştirilir
V_ALMASIK = 0.30               # ardışık aralık farkı / ortanca aralık bunu aşarsa çevrimde iki tepe var
V_YAYILIM = 0.50               # (P75 − P25) / ortanca üst sınırı (yöntem 2 güvenilirliği; köpekte solunuma bağlı değişim ~0,35)
V_MIN_VURUS = 6
V_BELIRGIN = 4.5               # ortanca tepe / ortanca zarf. Ölçülen: iyi göğüs kaydı 5-6, zayıf temas 2,3-3,4, gürültü 1,3
KAT_ORAN = 0.08                # iki yöntemin oranı 2 ya da 0,5'e bu kadar yakınsa kat şüphesi (veto)
DUZENSIZ = 0.12                # yayılım bunu aşarsa ritim düzensiz sayılır (bilgi)
M_N, M_ADIM = 1024, 512             # mırlama çerçevesi (1 kHz)
M_F0_MIN, M_F0_MAX = 18.0, 40.0
M_PAY, M_CERCEVE_ORAN, M_MIN_CERCEVE = 0.45, 0.5, 4
S_LAG_MIN, S_LAG_MAX = 20, 150      # solunum: 20 Hz zarfta 1,0-7,5 sn (60-8/dk)
S_MIN_N20 = 400                     # en az 20 sn
S_MIN_CEVRIM = 2.5
S_DUZ = 45                          # solunum zarfı düzleştirme (100 Hz örnek); nabız biliniyorsa kalp periyodunun tam katı
P_LAG_MIN, P_LAG_MAX = 13, 30       # soluma: 100 Hz zarfta 200-460/dk
P_TREND = 51                        # soluma: 0,51 sn'lik ortalama çıkarılır (yavaş nefes zarfı periyodiklik sanılmasın)
P_R = 0.5
SIZINTI = 0.10                      # solunum/soluma hızı nabza (soluma için iki katına da) bu kadar yakınsa kalp sızıntısı
PEN_MIN, PEN_YAKIN = 3, 0.12        # pencere kuralı: en az 3 net pencere (ve pencerelerin en az 2/5'i), ortancaya ±%12
T_R, T_MIN_PEN, T_YAKIN = 0.3, 4, 0.06   # tutarlılık kuralı: r alt sınırı, en az pencere, adaya yakınlık (pencerelerin en az 4/5'i)
T_R2T, T_BANT = 0.15, 0.06          # tutarlılık kuralı: iki periyot sonraki otokorelasyon alt sınırı ve arama bandı (±%6)


# ───────────────────────── temel yardımcılar (C'de birebir karşılığı var) ─────────────────────────
def biquad(tur, fc, fs, q=0.7071):
    w = 2.0 * math.pi * fc / fs; c = math.cos(w); sn = math.sin(w); al = sn / (2.0 * q); a0 = 1 + al
    if tur == "lp": b0 = (1 - c) / 2 / a0; b1 = (1 - c) / a0
    else:           b0 = (1 + c) / 2 / a0; b1 = -(1 + c) / a0
    return b0, b1, b0, -2 * c / a0, (1 - al) / a0


def suz(k, xs):
    b0, b1, b2, a1, a2 = k; z1 = z2 = 0.0; out = [0.0] * len(xs)
    for i, v in enumerate(xs):
        y = b0 * v + z1; z1 = b1 * v - a1 * y + z2; z2 = b2 * v - a2 * y; out[i] = y
    return out


def duzlestir(e, w):
    """w (tek sayı) genişlikli ortalama; kenarlarda eldeki komşuların ortalaması."""
    import numpy as np
    e = np.asarray(e, dtype=np.float64); n = len(e)
    return np.convolve(e, np.ones(w), "same") / np.convolve(np.ones(n), np.ones(w), "same")


def ortanca(a):
    b = sorted(a); n = len(b)
    return 0.5 * (b[(n - 1) // 2] + b[n // 2])


def yuzdelik(a, q):
    b = sorted(a)
    return b[int(q * (len(b) - 1))]


def otokor(s, lo, hi):
    """Ortalaması çıkarılmış s için ac[lag] = Σ s[i]·s[i-lag] / Σ s², lag = lo..hi. Sözlük döner; ac0 ≤ 0 ise boş."""
    import numpy as np
    s = np.asarray(s, dtype=np.float64); n = len(s); ac0 = float((s * s).sum())
    if ac0 <= 0: return {}
    return {lag: float((s[lag:] * s[:-lag]).sum()) / ac0 for lag in range(max(1, lo), min(hi, n - 1) + 1)}


def tepe_sec(ac, lag_min, lag_max, yalniz_ic=False):
    """Aralıktaki en yüksek değerin TEPE_ORAN katına ulaşan en kısa gecikmeli yerel tepe + parabol ara değeri.
    Yerel tepe: ac[l] > ac[l-1] ve ac[l] >= ac[l+1] (ac'de lag_min-1 .. lag_max+1 bulunmalı).
    Yerel tepe yoksa: yalniz_ic ise bulunamadı (0, 0.0, 0.0); değilse aralığın en yüksek değeri (kenar olabilir).
    Dönüş (lag_tam, lag_kesirli, r)."""
    aday = [l for l in range(lag_min, lag_max + 1) if l in ac]
    if not aday: return 0, 0.0, 0.0
    gl = aday[0]
    for l in aday:
        if ac[l] > ac[gl]: gl = l
    rmax = ac[gl]; sec = 0
    if rmax > 0:
        for l in aday:
            if (l - 1) in ac and (l + 1) in ac and ac[l] > ac[l - 1] and ac[l] >= ac[l + 1] and ac[l] >= TEPE_ORAN * rmax:
                sec = l; break
    if sec == 0:
        if yalniz_ic: return 0, 0.0, 0.0
        sec = gl
    kes = float(sec)
    if (sec - 1) in ac and (sec + 1) in ac:
        ym, y0, yp = ac[sec - 1], ac[sec], ac[sec + 1]; payda = ym - 2 * y0 + yp
        if payda < 0:
            d = 0.5 * (ym - yp) / payda
            if -1.0 < d < 1.0: kes = sec + d
    return sec, kes, ac[sec]


# ───────────────────────── nabız ─────────────────────────
def nabiz_ac_ic(e, lag_min, lag_max):
    """Yöntem 1. e: kalp zarfı (100 Hz, ilk 1 sn atılmış). Dönüş (nabız, r, r2, ic); en az 4 sn yoksa (0,0,0,0).
    ic = 1: seçilen gecikme gerçek bir yerel tepe. ic = 0: yerel tepe yoktu, aralığın en yüksek değeri (çoğunlukla
    aralığın kenarı; yavaş dalgalanan gürültüde olur) alındı. A3 kuralları yalnız ic = 1 adayları kabul eder."""
    n = len(e)
    if n < 400: return 0.0, 0.0, 0.0, 0
    s = duzlestir(e, 5); s = s - s.mean()
    ac = otokor(s, 1, lag_max + 1)
    if not ac: return 0.0, 0.0, 0.0, 0
    sec, kes, r = tepe_sec(ac, lag_min, lag_max)
    if sec <= 0 or kes <= 0: return 0.0, 0.0, 0.0, 0
    h = sec // 2
    ic = 1 if ((sec - 1) in ac and (sec + 1) in ac and ac[sec] > ac[sec - 1] and ac[sec] >= ac[sec + 1]) else 0
    return 6000.0 / kes, r, (ac[h] if h >= 1 else 0.0), ic


def nabiz_ac(e, lag_min, lag_max):
    """nabiz_ac_ic'in ilk üç değeri (nabız, r, r2)."""
    return nabiz_ac_ic(e, lag_min, lag_max)[:3]


def r_iki_kat(e, kes):
    """Tutarlılık kuralı: zarf otokorelasyonunun iki periyot (2×kes, ±T_BANT) çevresindeki en yüksek değeri. Yoksa 0."""
    n = len(e)
    if n < 400 or kes <= 0: return 0.0
    lo = int(math.floor(2.0 * kes * (1.0 - T_BANT))); hi = int(math.ceil(2.0 * kes * (1.0 + T_BANT)))
    if lo < 1 or hi > n // 2: return 0.0
    s = duzlestir(e, 5); s = s - s.mean()
    ac = otokor(s, lo, hi)
    return max(ac.values()) if ac else 0.0


def pencere_karari(pen):
    """Pencere kuralı. pen: [(nabız, r, ic)] (8 sn, 4 sn adım). Dönüş (nabız ya da 0, net pencere sayısı)."""
    iyi = sorted(b for b, r, ic in pen if b > 0 and ic and r >= R_ORTA)
    gerek = max(PEN_MIN, (2 * len(pen) + 4) // 5)
    if len(iyi) < gerek: return 0.0, len(iyi)
    med = ortanca(iyi)
    return (med if all(abs(b - med) <= PEN_YAKIN * med for b in iyi) else 0.0), len(iyi)


def tutarli_karari(bpm1, r, ic, pen, r2t):
    """Tutarlılık kuralı. Dönüş (kabul mü, adaya yakın pencere sayısı).
    r2t ≤ r koşulu: iki periyot sonraki benzerlik adayınkinden yüksekse gerçek periyot iki kat uzundur (yalnız nefes
    sesi olan kayıtta alış + veriş çifti böyle görünür); o zaman aday kabul edilmez."""
    uyan = sum(1 for b, _, _ in pen if b > 0 and bpm1 > 0 and abs(b - bpm1) <= T_YAKIN * bpm1)
    kabul = (ic and r >= T_R and len(pen) >= T_MIN_PEN and uyan >= (4 * len(pen) + 4) // 5 and T_R2T <= r2t <= r)
    return bool(kabul), uyan


def nabiz_vurus(e, lag_min, lag_max):
    """Yöntem 2. Dönüş sözlük: nabiz, n (çevrim sayısı), yayilim, iki_tepe (0/1), belirgin, guvenilir (0/1)."""
    bos = dict(nabiz=0.0, n=0, yayilim=0.0, iki_tepe=0, belirgin=0.0, guvenilir=0)
    n = len(e)
    if n < 400: return bos
    s = duzlestir(e, 5).tolist()
    med = ortanca(s); p98 = yuzdelik(s, 0.98)
    if p98 <= med or med <= 0: return bos
    esik = med + V_ESIK * (p98 - med)
    tepe = []
    for i in range(1, n - 1):
        if s[i] > esik and s[i] >= s[i - 1] and s[i] > s[i + 1]:
            if tepe and i - tepe[-1] < V_MIN_ARA:
                if s[i] > s[tepe[-1]]: tepe[-1] = i
            else: tepe.append(i)
    m = len(tepe)
    if m < 5: return bos
    belirgin = ortanca([s[i] for i in tepe]) / med
    i1 = [tepe[k + 1] - tepe[k] for k in range(m - 1)]
    med1 = ortanca(i1)
    alm = ortanca([abs(i1[k + 1] - i1[k]) for k in range(len(i1) - 1)]) / med1 if med1 > 0 else 0.0
    iki = 1 if (alm > V_ALMASIK and m >= 6) else 0
    cev = [tepe[k + 2] - tepe[k] for k in range(m - 2)] if iki else i1
    medc = ortanca(cev)
    if medc <= 0: return bos
    yay = (yuzdelik(cev, 0.75) - yuzdelik(cev, 0.25)) / medc
    nv = m // 2 if iki else m
    guv = 1 if (nv >= V_MIN_VURUS and belirgin >= V_BELIRGIN and yay <= V_YAYILIM
                and 0.75 <= nv / (n / medc) <= 1.25 and lag_min <= medc <= lag_max) else 0
    return dict(nabiz=6000.0 / medc, n=nv, yayilim=yay, iki_tepe=iki, belirgin=belirgin, guvenilir=guv)


# ───────────────────────── mırlama / soluma / solunum ─────────────────────────
def mirlama_bul(xp):
    """xp: 15-150 Hz süzülmüş 1 kHz sinyal (ilk 1 sn atılmış). Dönüş (var mı 0/1, Hz, tonal çerçeve oranı)."""
    import numpy as np
    xp = np.asarray(xp, dtype=np.float64); n = len(xp)
    if n < M_N: return 0, 0.0, 0.0
    pen = 0.5 - 0.5 * np.cos(2.0 * np.pi * np.arange(M_N) / M_N)
    df = 1000.0 / M_N
    k_lo = int(math.ceil(15.0 / df)); k_hi = int(math.floor(150.0 / df))
    f_lo = int(math.ceil(M_F0_MIN / df)); f_hi = int(math.floor(M_F0_MAX / df))
    gen = [int(round(1.5 * h / df)) for h in (1, 2, 3)]
    top = 0; tonal = 0; f0lar = []
    for b in range(0, n - M_N + 1, M_ADIM):
        P = np.abs(np.fft.rfft(xp[b:b + M_N] * pen)) ** 2
        bant = float(P[k_lo:k_hi + 1].sum()); top += 1
        if bant <= 0: continue
        en = 0.0; en_k = 0
        for k0 in range(f_lo, f_hi + 1):
            parca = []
            for h in (1, 2, 3):
                a = max(k_lo, h * k0 - gen[h - 1]); z = min(k_hi, h * k0 + gen[h - 1])
                parca.append(float(P[a:z + 1].sum()) if z >= a else 0.0)
            tarak = sum(parca)
            if tarak <= 0: continue
            if sum(1 for q in parca if q >= 0.15 * tarak) < 2: continue      # tek çizgi (50 Hz şebeke vb.) mırlama değildir
            if tarak / bant > en: en = tarak / bant; en_k = k0
        if en >= M_PAY: tonal += 1; f0lar.append(en_k * df)
    if top == 0: return 0, 0.0, 0.0
    oran = tonal / top
    var = 1 if (tonal >= M_MIN_CERCEVE and oran >= M_CERCEVE_ORAN) else 0
    return var, (ortanca(f0lar) if f0lar else 0.0), oran


def soluma_bul(eb):
    """eb: solunum zarfı (100 Hz, ilk 1 sn atılmış). Dönüş (hız /dk, r); bulunamazsa (0, 0)."""
    import numpy as np
    eb = np.asarray(eb, dtype=np.float64); n = len(eb)
    if n < 400: return 0.0, 0.0
    s = duzlestir(eb - duzlestir(eb, P_TREND), 3); s = s - s.mean()
    ac = otokor(s, P_LAG_MIN - 1, P_LAG_MAX + 1)
    if not ac: return 0.0, 0.0
    sec, kes, r = tepe_sec(ac, P_LAG_MIN, P_LAG_MAX, yalniz_ic=True)
    return (6000.0 / kes, r) if kes > 0 else (0.0, 0.0)


def solunum_bul(eb, kalp_periyot=0.0):
    """eb: solunum zarfı (100 Hz). kalp_periyot: kalp periyodu (10 ms birim; 0 = bilinmiyor): zarf, kalp periyodunun
    tam katı uzunlukta ortalamayla düzleştirilir; böylece solunum bandına sızan kalp sesi periyodikliği sıfırlanır.
    Dönüş (solunum /dk, r, durum): durum KISA | BELIRSIZ | OK (soluma/mırlama/sızıntı kararı çağıranda)."""
    import numpy as np
    eb = np.asarray(eb, dtype=np.float64)
    if len(eb) // 5 < S_MIN_N20: return 0.0, 0.0, "KISA"
    w = S_DUZ
    if kalp_periyot > 0: w = int(math.floor(math.ceil(S_DUZ / kalp_periyot) * kalp_periyot + 0.5))
    if w % 2 == 0: w += 1
    d = duzlestir(eb, w); n20 = len(d) // 5
    b = d[:n20 * 5].reshape(-1, 5).mean(1)
    s = b - b.mean()
    ac = otokor(s, S_LAG_MIN - 1, S_LAG_MAX + 1)
    if not ac: return 0.0, 0.0, "BELIRSIZ"
    sec, kes, r = tepe_sec(ac, S_LAG_MIN, S_LAG_MAX, yalniz_ic=True)
    if kes <= 0: return 0.0, 0.0, "BELIRSIZ"
    if n20 < S_MIN_CEVRIM * sec: return 1200.0 / kes, r, "KISA"
    return 1200.0 / kes, r, ("OK" if r >= R_ORTA else "BELIRSIZ")


def yakin(a, b, oran):
    return b > 0 and abs(a - b) <= oran * b


# ───────────────────────── tam analiz ─────────────────────────
def analiz(ornekler, bit, profil="insan"):
    """ornekler: işaretli tam sayı örnekler (np dizi), 48 kHz mono. Dönüş: sonuç sözlüğü."""
    import numpy as np
    p = PROFIL[profil]; lag_min, lag_max = p["lag_min"], p["lag_max"]
    tam = float(1 << (bit - 1)); v = np.asarray(ornekler, dtype=np.int64)
    db = lambda a: 20.0 * math.log10(a) if a > 0 else -120.0

    # kalite (ilk 1 sn hariç)
    q = np.abs(v[FS:]) if len(v) > FS else np.zeros(0, dtype=np.int64)
    tepe = int(q.max()) if len(q) else 0
    doygun = int((q >= int(0.999 * (tam - 1))).sum()) if len(q) else 0

    # akış: 1 kHz ve solunum zarfı
    n1 = len(v) // 48
    x1k = (v[:n1 * 48].reshape(-1, 48).sum(1) / 48.0 / tam).tolist()
    n4 = len(v) // 12
    x4k = (v[:n4 * 12].reshape(-1, 12).sum(1) / 12.0 / tam).tolist()
    yb = suz(biquad("lp", 1500.0, 4000.0), suz(biquad("hp", 200.0, 4000.0, 1.30656296), suz(biquad("hp", 200.0, 4000.0, 0.54119610), x4k)))
    yba = np.abs(np.asarray(yb))
    sb = yba[4000:]; s_rms = float(np.sqrt((sb * sb).mean())) if len(sb) else 0.0
    nb = len(yba) // 40
    envb = yba[:nb * 40].reshape(-1, 40).mean(1)
    eb = envb[100:] if len(envb) > 100 else envb[:0]

    # kalp zarfı
    yh = suz(biquad("lp", p["lp"], 1000.0), suz(biquad("hp", p["hp"], 1000.0), x1k))
    yha = np.abs(np.asarray(yh))
    hb = yha[1000:]; h_rms = float(np.sqrt((hb * hb).mean())) if len(hb) else 0.0
    nh = len(yha) // 10
    envh = yha[:nh * 10].reshape(-1, 10).mean(1)
    e = envh[100:] if len(envh) > 100 else envh[:0]

    bpm1, r, r2, ic1 = nabiz_ac_ic(e, lag_min, lag_max)
    v2 = nabiz_vurus(e, lag_min, lag_max)
    pencere = []; pen = []
    for k in range(0, len(e) - 800 + 1, 400):
        b, wr, _, wic = nabiz_ac_ic(e[k:k + 800], lag_min, lag_max); pen.append((b, wr, wic)); pencere.append((round(b), round(wr, 2)))
    r2t = r_iki_kat(e, 6000.0 / bpm1) if bpm1 > 0 else 0.0
    pen_nabiz, pen_iyi = pencere_karari(pen)
    tutarli, pen_uyan = tutarli_karari(bpm1, r, ic1, pen, r2t)

    # mırlama (kedi) / soluma (köpek)
    mir, mir_hz, mir_oran = 0, 0.0, 0.0
    if p["mirlama"]:
        xp = suz(biquad("lp", 150.0, 1000.0), suz(biquad("hp", 15.0, 1000.0), x1k))
        mir, mir_hz, mir_oran = mirlama_bul(xp[1000:])
    sol_hiz, sol_r = (soluma_bul(eb) if p["soluma"] else (0.0, 0.0))

    # A3 koruması: aday, eşlenmemiş (kalp periyoduna göre düzleştirilmemiş) solunum hızına ya da iki katına yakınsa
    # bulunan ritim büyük olasılıkla nefes sesidir; düşük güvenli kurallar bu adayı kabul etmez.
    nefes0 = []
    def nefes_gibi(aday):
        if not nefes0: nefes0.append(solunum_bul(eb, 0.0))
        rr0, _, sd0 = nefes0[0]
        return sd0 == "OK" and (yakin(aday, rr0, SIZINTI) or yakin(aday, 2.0 * rr0, SIZINTI))

    # nabız kararı
    tepe_db = db(tepe / tam); seviye_db = db(h_rms)
    nabiz = bpm1; yontem = "ac"
    if len(e) < 400: durum = "KISA"
    elif tepe == 0 or seviye_db < SINYAL_YOK_DB: durum = "SINYAL_YOK"
    elif len(q) and doygun > DOYGUN_ORAN * len(q): durum = "DOYGUN"
    elif bpm1 <= 0: durum = "BELIRSIZ"
    elif mir: durum = "MIRLAMA"
    elif r >= R_ORTA:
        oran = v2["nabiz"] / bpm1 if v2["guvenilir"] else 0.0
        kat = abs(oran - 2.0) <= 2.0 * KAT_ORAN or abs(oran - 0.5) <= 0.5 * KAT_ORAN
        durum = "KAT_SUPHESI" if kat else "OK"            # güvenilir vuruş sayımı yarı/çift hız diyorsa sayı verilmez
    elif v2["guvenilir"]:
        durum = "OK"; nabiz = v2["nabiz"]; yontem = "vurus"   # düzensiz ritim: otokorelasyon zayıf, vuruşlar belirgin
    elif pen_nabiz > 0 and not nefes_gibi(pen_nabiz):
        durum = "OK"; nabiz = pen_nabiz; yontem = "pencere"   # A3: nabız kayıt içinde değişmiş / bir bölüm gürültülü
    elif tutarli and not nefes_gibi(bpm1):
        durum = "OK"; yontem = "tutarli"                      # A3: benzerlik düşük ama bütün pencereler aynı ritmi söylüyor
    else: durum = "BELIRSIZ"

    # soluma (köpek): nabızla ya da iki katıyla çakışıyorsa kalp sesi sızıntısıdır
    soluma = 0
    if p["soluma"] and sol_r >= P_R:
        soluma = 0 if (durum == "OK" and (yakin(sol_hiz, nabiz, SIZINTI) or yakin(sol_hiz, 2 * nabiz, SIZINTI))) else 1

    # solunum: kalp periyodu biliniyorsa (ya da zayıf da olsa bir aday varsa) ona eşlenmiş düzleştirme
    kp = 6000.0 / nabiz if (durum == "OK" and nabiz > 0) else (6000.0 / bpm1 if (bpm1 > 0 and r >= 0.3) else 0.0)
    rr, rs, sdurum = solunum_bul(eb, kp)
    if sdurum == "OK":
        if mir: sdurum = "MIRLAMA"
        elif soluma: sdurum = "SOLUMA"
        elif durum == "OK" and yakin(rr, nabiz, SIZINTI): sdurum = "KALP_SIZINTISI"
        elif durum != "OK" and rr >= 6000.0 / lag_max: sdurum = "BELIRSIZ"     # nabız bilinmiyor; bu hız kalp de olabilir
    elif sdurum == "BELIRSIZ" and soluma: sdurum = "SOLUMA"

    return {"alg": ALG, "profil": profil, "durum": durum, "nabiz": nabiz, "r": r, "r2": r2, "yontem": yontem,
            "nabiz_ac": bpm1, "nabiz_vurus": v2["nabiz"], "nvurus": v2["n"], "yayilim": v2["yayilim"], "iki_tepe": v2["iki_tepe"],
            "belirgin": v2["belirgin"], "vguven": v2["guvenilir"], "solunum": rr, "rs": rs, "sdurum": sdurum, "mirlama": mir, "mirlama_hz": mir_hz,
            "mirlama_oran": mir_oran, "soluma": soluma, "soluma_hiz": sol_hiz, "soluma_r": sol_r,
            "doygun": doygun, "tepe": tepe_db, "seviye": seviye_db, "sseviye": db(s_rms), "sure": len(e) / 100.0, "pencere": pencere,
            "pen_n": len(pen), "pen_iyi": pen_iyi, "pen_uyan": pen_uyan, "r2t": r2t}


# ───────────────────────── dosya / çıktı ─────────────────────────
def pcm_coz(d, bit):
    """Küçük uçlu ham PCM baytları (16 ya da 24 bit, mono) → np.int32 örnekler."""
    import numpy as np
    if bit == 24:
        n = len(d) // 3
        a = np.frombuffer(bytes(d[:n * 3]), dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        r = a[:, 0] | a[:, 1] << 8 | a[:, 2] << 16
        return np.where(r >= 1 << 23, r - (1 << 24), r)
    if bit == 16: return np.frombuffer(bytes(d[:len(d) // 2 * 2]), dtype="<i2").astype(np.int32)
    raise ValueError("16 ya da 24 bit bekleniyor")


def wav_oku(yol):
    """Mono 16/24 bit WAV → (np.int32 örnekler, bit). 48 kHz değilse hata."""
    with wave.open(yol) as f:
        fs, w, n, ch = f.getframerate(), f.getsampwidth(), f.getnframes(), f.getnchannels(); d = f.readframes(n)
    if fs != FS or ch != 1: raise ValueError(f"{yol}: 48 kHz mono bekleniyor ({fs} Hz, {ch} kanal)")
    if w not in (2, 3): raise ValueError(f"{yol}: 16 ya da 24 bit bekleniyor")
    return pcm_coz(d, w * 8), w * 8


def satir(s):
    """Kartın '[mic] sonuc:' satırıyla aynı anahtar=değer biçimi (hareket/sd/etiket kartta eklenir)."""
    return (f"alg={s['alg']} profil={s['profil']} durum={s['durum']} nabiz={s['nabiz']:.0f} r={s['r']:.2f} r2={s['r2']:.2f} "
            f"yontem={s['yontem']} nabiz_ac={s['nabiz_ac']:.0f} nabiz_vurus={s['nabiz_vurus']:.0f} nvurus={s['nvurus']} "
            f"yayilim={s['yayilim']:.2f} belirgin={s['belirgin']:.1f} vguven={s['vguven']} solunum={s['solunum']:.0f} rs={s['rs']:.2f} sdurum={s['sdurum']} "
            f"mirlama={s['mirlama']} soluma={s['soluma']} doygun={s['doygun']} tepe={s['tepe']:.1f} seviye={s['seviye']:.1f} "
            f"sseviye={s['sseviye']:.1f} sure={s['sure']:.1f} pen_n={s['pen_n']} pen_iyi={s['pen_iyi']} pen_uyan={s['pen_uyan']} r2t={s['r2t']:.2f}")


def main():
    arg = sys.argv[1:]
    profil = "insan"
    if arg and arg[-1] in PROFIL: profil = arg.pop()
    if not arg: print(__doc__); sys.exit(1)
    for yol in arg:
        v, bit = wav_oku(yol); s = analiz(v, bit, profil)
        print(f"{yol}: {satir(s)}")
        print("   pencereler (8 sn): " + " ".join(f"{b}({r:.2f})" for b, r in s["pencere"]))


if __name__ == "__main__":
    main()
