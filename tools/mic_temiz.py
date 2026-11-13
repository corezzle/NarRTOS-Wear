#!/usr/bin/env python3
"""
mic_temiz.py: kalp kaydında hışırtıyı azaltır, dinleme için bası ve seviyeyi ayarlar (numpy gerekir).

Kullanım: python3 tools/mic_temiz.py <giris.wav> <cikis.wav>

Gürültü izi vuruş aralarından (en sessiz %20 çerçeve) ölçülür ve çıkarılır; 15 Hz altı ve 400 Hz üstü
yumuşak kesilir; 150 Hz altına +4 dB verilir; tepeler yumuşak sınırlanır. Üfürüm gibi yüksek frekanslı
sesleri de azaltır: tıbbi değerlendirme için ham dosya saklanmalıdır. Sonda ritim tahmini yazar.
"""
import sys, wave
import numpy as np

KES_UST = 400.0
KES_ALT = 15.0
BAS_DB = 4.0

def oku(yol):
    with wave.open(yol) as f:
        fs, w, n = f.getframerate(), f.getsampwidth(), f.getnframes(); d = f.readframes(n)
    if w == 3:
        a = np.frombuffer(d, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        r = a[:, 0] | a[:, 1] << 8 | a[:, 2] << 16
        return np.where(r >= 1 << 23, r - (1 << 24), r) / 2 ** 23, fs
    return np.frombuffer(d, dtype=np.int16) / 2 ** 15, fs

def yaz(yol, x, fs):
    b = np.round(x * (2 ** 23 - 1)).astype(np.int32)
    o = np.zeros((len(b), 3), dtype=np.uint8); o[:, 0] = b & 255; o[:, 1] = (b >> 8) & 255; o[:, 2] = (b >> 16) & 255
    with wave.open(yol, "wb") as f:
        f.setnchannels(1); f.setsampwidth(3); f.setframerate(fs); f.writeframes(o.tobytes())

def ritim(x, fs):
    S = np.fft.rfft(x); F = np.fft.rfftfreq(len(x), 1 / fs); S[(F < 20) | (F > 300)] = 0
    e = np.abs(np.fft.irfft(S, len(x))); L = fs // 100; e = e[:len(e) // L * L].reshape(-1, L).mean(1)
    e = np.convolve(e, np.ones(5) / 5, "same"); e = e - e.mean()
    ac = np.correlate(e, e, "full")[len(e) - 1:]; ac = ac / ac[0]
    i = 20 + int(np.argmax(ac[20:171]))
    return 6000 / i, float(ac[i]), float(ac[i // 2])

def main():
    v, fs = oku(sys.argv[1])
    N, H = 4096, 1024; w = np.sqrt(np.hanning(N) + 1e-12)
    bas = range(0, len(v) - N, H)
    S = np.array([np.fft.rfft(v[k:k + N] * w) for k in bas]); P = np.abs(S) ** 2; F = np.fft.rfftfreq(N, 1 / fs)
    gur = np.sort(P, axis=0)[:max(1, len(P) // 5)].mean(0)
    G = np.maximum(1 - 2.5 * gur / (P + 1e-20), 0.06)
    for i in range(1, len(G)): G[i] = np.maximum(G[i], 0.6 * G[i - 1])
    sekil = (1 / np.sqrt(1 + (F / KES_UST) ** 8)) * (1 / np.sqrt(1 + (KES_ALT / np.maximum(F, 1)) ** 4))
    sekil = sekil * (1 + (10 ** (BAS_DB / 20) - 1) / np.sqrt(1 + (F / 150.0) ** 4))
    Y = S * G * sekil
    o = np.zeros(len(v)); t = np.zeros(len(v))
    for i, k in enumerate(bas):
        o[k:k + N] += np.fft.irfft(Y[i], N) * w; t[k:k + N] += w * w
    o = o / np.maximum(t, 1e-6); o[:N] *= np.linspace(0, 1, N)
    o = np.tanh(o * 1.2 / max(np.percentile(np.abs(o), 99.9), 1e-9))   # tek darbeler seviyeyi düşürmesin
    o = o * 0.891 / np.abs(o).max()
    yaz(sys.argv[2], o, fs)
    a, b = ritim(v, fs), ritim(o, fs)
    print(f"ritim ham {a[0]:.0f}/dk r={a[1]:.2f} (yarı periyot r={a[2]:.2f}); temiz {b[0]:.0f}/dk r={b[1]:.2f}")

if __name__ == "__main__":
    main()
