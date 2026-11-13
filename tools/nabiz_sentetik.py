#!/usr/bin/env python3
"""
nabiz_sentetik.py: algoritma sınaması için yapay kalp / nefes / mırlama / soluma sinyalleri (48 kHz, 24 bit). numpy gerekir.

GERÇEK HAYVAN KAYDININ YERİNİ TUTMAZ. Amaç: algoritmanın mantığını (kat hatası, düzensiz ritim, mırlama, soluma, solunum
sayısı) bilinen cevaplı sinyallerle sınamak ve kart kodu ile referansın aynı sonucu verdiğini göstermek.
Değerler literatürdeki tipik aralıklardan seçildi (kedi 140-220/dk, köpek 60-160/dk, mırlama ~25 Hz, soluma ~300/dk).

Kullanım (tek dosya üretmek için):
    python3 tools/nabiz_sentetik.py kedi_200.wav kalp=200 nefes=30 sure=30
    python3 tools/nabiz_sentetik.py kedi_mir.wav kalp=180 mirlama=25 sure=30
    python3 tools/nabiz_sentetik.py kopek.wav kalp=90 rsa=0.25 nefes=20 sure=30
Anahtarlar: kalp (dk), f1/f2 (S1/S2 frekansı Hz), a2 (S2/S1 genlik), rsa (0-0.4), nefes (dk), mirlama (Hz), soluma (dk),
            gurultu (dBFS), sebeke (50 Hz genlik dBFS), sure (sn), tohum.
"""
import math, sys, wave

FS = 48000


def _patlama(n, f, fs):
    import numpy as np
    t = np.arange(n) / fs
    return (0.5 - 0.5 * np.cos(2 * np.pi * np.arange(n) / n)) * np.sin(2 * np.pi * f * t)


def kalp(hr, sure, f1=50.0, f2=70.0, d1=0.09, d2=0.06, a2=0.6, rsa=0.0, nefes=20.0, seg=0.0, alternans=1.0, kayma=0.0, tohum=1):
    """S1 + S2 patlamaları. rsa: solunuma bağlı periyot değişimi (0,25 = ±%25). alternans: her ikinci vuruşun genlik
    çarpanı (1 = eşit; 0,6 = ardışık vuruşlar farklı → yarı hız tuzağı). kayma: nabzın kayıt boyunca doğrusal değişimi
    (-0,15 = sonda %15 daha yavaş). Dönüş (sinyal, vuruş zamanları)."""
    import numpy as np
    rng = np.random.default_rng(tohum); x = np.zeros(int(sure * FS)); t = 0.25; vurus = []
    while t < sure - 0.6:
        T = 60.0 / (hr * (1.0 + kayma * t / sure)) * (1.0 + rsa * math.sin(2 * math.pi * t * nefes / 60.0)) * (1.0 + seg * rng.standard_normal())
        sistol = 0.08 + 0.2 * T                      # S1→S2 arası; hızlandıkça kısalır
        g = alternans if len(vurus) % 2 else 1.0
        for (t0, f, d, a) in ((t, f1, d1, 1.0), (t + sistol, f2, d2, a2)):
            i = int(t0 * FS); n = int(d * FS)
            if i + n < len(x): x[i:i + n] += g * a * (1.0 + 0.1 * rng.standard_normal()) * _patlama(n, f, FS)
        vurus.append(t); t += T
    return x, vurus


def gercek_nabiz(vurus):
    """Vuruş zamanlarından sayım tabanlı ortalama nabız (/dk)."""
    return 60.0 * (len(vurus) - 1) / (vurus[-1] - vurus[0]) if len(vurus) > 1 else 0.0


def _bant_gurultu(n, lo, hi, rng):
    import numpy as np
    X = np.fft.rfft(rng.standard_normal(n)); F = np.fft.rfftfreq(n, 1.0 / FS); X[(F < lo) | (F > hi)] = 0
    y = np.fft.irfft(X, n); return y / (np.abs(y).max() + 1e-12)


def nefes(rr, sure, tohum=2, ie=(0.32, 0.42)):
    """Trakeal nefes: bant gürültüsü (250-1200 Hz), her döngüde alış + veriş + duraklama."""
    import numpy as np
    rng = np.random.default_rng(tohum); n = int(sure * FS); g = _bant_gurultu(n, 250.0, 1200.0, rng)
    zarf = np.zeros(n); Tc = 60.0 / rr; t = 0.4
    while t < sure:
        for (bas, uz, a) in ((0.0, ie[0], 0.8), (ie[0] + 0.06, ie[1], 1.0)):
            i = int((t + bas * Tc) * FS); m = int(uz * Tc * FS)
            if i < n:
                m = min(m, n - i); zarf[i:i + m] += a * (0.5 - 0.5 * np.cos(2 * np.pi * np.arange(m) / max(m, 1)))
        t += Tc
    return g * zarf


def mirlama(f0, sure, nefes_hz=0.5, tohum=3):
    """Mırlama: f0 temel + harmonikler; alış/veriş arasında frekans ±1 Hz oynar, genlik hafif dalgalanır, kesilmez."""
    import numpy as np
    t = np.arange(int(sure * FS)) / FS
    f = f0 + 1.0 * np.sin(2 * np.pi * nefes_hz * t)
    faz = 2 * np.pi * np.cumsum(f) / FS
    y = sum((1.0 / h) * np.sin(h * faz) for h in range(1, 7))
    return y * (0.8 + 0.2 * np.sin(2 * np.pi * nefes_hz * t + 1.0)) / 2.0


def soluma(hiz, sure, tohum=4):
    """Köpek soluması: dakikada `hiz` kısa nefes; bant gürültüsü patlamaları."""
    import numpy as np
    rng = np.random.default_rng(tohum); n = int(sure * FS); g = _bant_gurultu(n, 300.0, 1400.0, rng)
    t = np.arange(n) / FS
    return g * (0.5 - 0.5 * np.cos(2 * np.pi * hiz / 60.0 * t)) ** 2


def surtunme(sure, adet, tohum=5, db=(-40.0, -20.0)):
    """Rastgele zamanlı, 0,02-0,4 sn süreli geniş bant gürültü patlamaları (giysi/tüy sürtünmesi benzeri). Kalp yok."""
    import numpy as np
    rng = np.random.default_rng(tohum); n = int(sure * FS); x = np.zeros(n)
    for t in rng.uniform(0.5, sure - 0.5, adet):
        a = int(t * FS); m = len(x[a:a + int(rng.uniform(0.02, 0.4) * FS)])
        x[a:a + m] += rng.standard_normal(m) * 10.0 ** (rng.uniform(*db) / 20.0)
    return x


def tikirti(sure, hiz, tohum=6, db=(-36.0, -26.0)):
    """Rastgele zamanlı (ortalama `hiz` /sn), kalp sesine benzer frekansta (40-120 Hz) kısa darbeler. Ritim YOK."""
    import numpy as np
    rng = np.random.default_rng(tohum); n = int(sure * FS); x = np.zeros(n); t = 0.3
    while t < sure - 0.5:
        a = int(t * FS); m = int(rng.uniform(0.04, 0.1) * FS)
        x[a:a + m] += 10.0 ** (rng.uniform(*db) / 20.0) * _patlama(m, rng.uniform(40.0, 120.0), FS)[:len(x[a:a + m])]
        t += rng.exponential(1.0 / hiz) + 0.08
    return x


def karistir(sure=30.0, kalp_dk=0, nefes_dk=0, mirlama_hz=0, soluma_dk=0, gurultu_db=-60.0, sebeke_db=None,
             kalp_db=-30.0, nefes_db=-45.0, mirlama_db=-25.0, soluma_db=-35.0, tohum=1, patlama=None,
             surtunme_adet=0, tikirti_hiz=0.0, **kalp_arg):
    """Bileşenleri verilen tepe seviyelerinde (dBFS) toplar. Dönüş: np.int32 örnekler (24 bit).
    patlama: (başlangıç sn, süre sn, dBFS) tek bir geniş bant gürültü bölümü. surtunme_adet / tikirti_hiz: yukarıdaki
    kalpsiz bozucular."""
    import numpy as np
    n = int(sure * FS); x = np.zeros(n); g = lambda db: 10.0 ** (db / 20.0)
    rng = np.random.default_rng(tohum + 100)
    if kalp_dk:
        k, _ = kalp(kalp_dk, sure, tohum=tohum, **kalp_arg); x += g(kalp_db) * k / (np.abs(k).max() + 1e-12)
    if nefes_dk: x += g(nefes_db) * nefes(nefes_dk, sure, tohum=tohum + 1)
    if mirlama_hz: x += g(mirlama_db) * mirlama(mirlama_hz, sure)
    if soluma_dk: x += g(soluma_db) * soluma(soluma_dk, sure)
    if sebeke_db is not None: x += g(sebeke_db) * np.sin(2 * np.pi * 50.0 * np.arange(n) / FS)
    x += g(gurultu_db) * rng.standard_normal(n) / 3.0
    if patlama:
        a = int(patlama[0] * FS); m = len(x[a:a + int(patlama[1] * FS)])
        x[a:a + m] += g(patlama[2]) * np.random.default_rng(tohum + 200).standard_normal(m) / 3.0
    if surtunme_adet: x += surtunme(sure, surtunme_adet, tohum=tohum + 300)
    if tikirti_hiz: x += tikirti(sure, tikirti_hiz, tohum=tohum + 400)
    return np.clip(np.round(x * 8388607.0), -8388608, 8388607).astype(np.int32)


def wav_yaz(yol, v):
    import numpy as np
    b = np.asarray(v, dtype=np.int32); o = np.zeros((len(b), 3), dtype=np.uint8)
    o[:, 0] = b & 255; o[:, 1] = (b >> 8) & 255; o[:, 2] = (b >> 16) & 255
    with wave.open(yol, "wb") as f:
        f.setnchannels(1); f.setsampwidth(3); f.setframerate(FS); f.writeframes(o.tobytes())


def main():
    if len(sys.argv) < 3: print(__doc__); sys.exit(1)
    ad = {"kalp": "kalp_dk", "nefes": "nefes_dk", "mirlama": "mirlama_hz", "soluma": "soluma_dk", "gurultu": "gurultu_db", "sebeke": "sebeke_db"}
    arg = {}
    for a in sys.argv[2:]:
        k, v = a.split("=", 1); arg[ad.get(k, k)] = int(v) if k == "tohum" else float(v)
    wav_yaz(sys.argv[1], karistir(**arg)); print(sys.argv[1])


if __name__ == "__main__":
    main()
