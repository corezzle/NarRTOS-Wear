#!/usr/bin/env python3
"""
mic_wav.py — mic_ham.py'nin yazdığı CSV'yi (t_ms,deger) dinlenebilir WAV'a çevirir.

Kullanım:
    python3 tools/mic_wav.py ~/Desktop/mic1.csv            → ~/Desktop/mic1.wav
    python3 tools/mic_wav.py ~/Desktop/mic1.csv 4000       → örnekleme hızını elle ver
    MIC_KAZANC=8 python3 tools/mic_wav.py dosya.csv        → sabit ×8 kazanç, normalize YOK

Örnekleme hızı verilmezse t_ms sütunundan hesaplanır. Varsayılan: genlik dosyadaki en
yüksek değere göre normalize edilir (sessiz kayıt da duyulur). Dikkat: sessiz bir kayıtta
bu ×100-300 kazanç demektir ve gürültü tabanı cızırtı gibi duyulur (2026-11-02). Gerçek
seviyeyi duymak için MIC_KAZANC ile sabit kazanç verin. Dinle: `afplay dosya.wav`.
"""
import os, sys, struct, wave

def main():
    if len(sys.argv) < 2:
        print("kullanım: mic_wav.py <dosya.csv> [hz]"); sys.exit(1)
    src = sys.argv[1]; vals = []; t_last = 0
    with open(src) as f:
        next(f)
        for line in f:
            t, v = line.strip().split(",")[:2]; vals.append(int(v)); t_last = int(t)
    if not vals: print("boş dosya"); sys.exit(1)
    hz = int(sys.argv[2]) if len(sys.argv) > 2 else (round(len(vals) * 1000 / t_last) if t_last else 1000)
    # İsteğe bağlı baştan kırpma (MIC_BASTAN_KES=1.0 sn): kayıt başındaki MICBIAS/ADC açılış
    # patlaması (tepe ~28000) akıllı kazancı düşürüyordu (2026-11-02).
    kes = os.environ.get("MIC_BASTAN_KES")
    if kes: vals = vals[int(float(kes) * hz):] or vals
    # İsteğe bağlı alçak geçiren (kalp sesi için, ör. MIC_ALCAK=300): 2 kat 1. derece IIR.
    alcak = os.environ.get("MIC_ALCAK")
    if alcak:
        import math
        a = 1 - math.exp(-2 * math.pi * float(alcak) / hz); y1 = y2 = float(vals[0])
        out = []
        for v in vals:
            y1 += a * (v - y1); y2 += a * (y1 - y2); out.append(y2)
        vals = out
    m = sum(vals) / len(vals); vals = [v - m for v in vals]   # DC çıkar
    peak = max(1, max(abs(v) for v in vals))
    sabit = os.environ.get("MIC_KAZANC")
    tavan = os.environ.get("MIC_KAZANC_TAVAN")
    if sabit:
        k = float(sabit)
    elif tavan:
        # Akıllı kazanç: tek darbeler kazancı düşürmesin diye %99,9'luk değere göre, en fazla TAVAN kat.
        # MIC_KIRPMASIZ=1: tepeye göre, hiç kırpma yok (2026-11-06; %99,9 ile ×55 kazanç 363 örneği sert kırpmıştı).
        srt = sorted(abs(v) for v in vals); p999 = max(1, srt[int(len(srt) * 0.999)])
        k = min(32000 / (peak if os.environ.get("MIC_KIRPMASIZ") else p999), float(tavan))
    else:
        k = 32000 / peak
    dst = os.path.splitext(src)[0] + ".wav"
    with wave.open(dst, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(hz)
        w.writeframes(b"".join(struct.pack("<h", max(-32768, min(32767, int(v * k)))) for v in vals))
    print(f"{dst}  ({len(vals)} örnek, {hz} Hz, {len(vals)/hz:.1f} sn, tepe {peak}, kazanç ×{k:.1f}{' sabit' if sabit else (' tavanlı' if tavan else ' normalize')}{(', alçak geçiren ' + alcak + ' Hz') if alcak else ''})  →  afplay {dst}")

if __name__ == "__main__":
    main()
