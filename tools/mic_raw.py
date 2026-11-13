#!/usr/bin/env python3
"""
mic_raw.py: `mic raw` ikili akışını okur, WAV yazar (48 kHz, 16/24 bit, mono).

Kullanım: python3 tools/mic_raw.py <dosya.wav> [sn=30] [kanal=3] [bit=24] [varyant=0]

Çerçeve: "NARA" + seq(u32) + len(u16) + tasma(u16) + toplam(u32) + veri. Sıra atlarsa eksik
çerçeve sessizlikle doldurulur, sayısı yazılır. Baştaki 1 sn açılış patlaması diye atılır (MIC_BASTAN_KES).
DC süzülür, tepe -1 dBFS'e çekilir: kırpma yapılmaz.
"""
import glob, os, struct, sys, time, wave

FS = 48000

def main():
    path = sys.argv[1]
    sec = int(sys.argv[2]) if len(sys.argv) > 2 else 30
    ch = int(sys.argv[3]) if len(sys.argv) > 3 else 3
    bits = int(sys.argv[4]) if len(sys.argv) > 4 else 24
    var = int(sys.argv[5]) if len(sys.argv) > 5 else 0
    import serial
    t0 = time.time(); ports = glob.glob("/dev/cu.usbmodem*")
    while not ports and time.time() - t0 < 60:
        time.sleep(0.2); ports = glob.glob("/dev/cu.usbmodem*")
    if not ports: print("USB portu yok"); sys.exit(2)
    p = serial.Serial(); p.port = ports[0]; p.baudrate = 115200; p.timeout = 0.05; p.dtr = False; p.rts = False; p.open()
    buf = b""; t0 = time.time(); alive = False
    while time.time() - t0 < 25:
        buf += p.read(8192)
        if b"CmdSvc" in buf: alive = True; break
    if not alive: print("kart açılmadı"); sys.exit(2)
    print(f"UYGULAMA AYAKTA ({time.strftime('%H:%M:%S')})", flush=True)
    # Açılış testi aynı USB hattına metin basar, çerçeveleri bozar: bitmesini bekle.
    bekle = float(os.environ.get("MIC_BEKLE", "45")); t0 = time.time(); acilis = b""; son = None
    while time.time() - t0 < bekle:
        acilis += p.read(65536)
        if son is None and b"CHECKLIST" in acilis: son = time.time()
        if son and time.time() - son > 4: break
    print(f"açılış testi {'bitti' if son else 'görülmedi'} ({time.time() - t0:.0f} sn)", flush=True)
    # MIC_KOMUT: "mic raw" (varsayılan) ya da "mic nabiz" (kartta analiz + akış); MIC_EK: komut sonuna eklenen bayraklar (ör. "usb")
    komut = os.environ.get("MIC_KOMUT", "mic raw"); ek = os.environ.get("MIC_EK", "")
    p.reset_input_buffer(); p.write(f"{komut} {sec} {ch} {bits} {var} {ek}".strip().encode() + b"\r\n")
    print(f"kayıt: {sec} sn, {FS} Hz, {bits} bit, kanal {ch}, varyant {var}", flush=True)

    raw = bytearray(); t0 = time.time(); son = t0; bitti = False
    while time.time() - t0 < sec + 25:
        d = p.read(65536)
        if d: raw += d
        if time.time() - son >= 5: print(f"  {time.strftime('%H:%M:%S')} {len(raw)//1024} kB", flush=True); son = time.time()
        if b"raw bitti" in raw[-300:]:
            bitti = True
            # analiz satırları (mic nabiz) akıştan sonra gelir: [cmd] OK/ERR görülene kadar en çok 3 sn daha oku
            t2 = time.time()
            while time.time() - t2 < 3:
                raw += p.read(4096)
                if b"[cmd] OK" in raw[-600:] or b"[cmd] ERR" in raw[-600:]: break
            break
        if b"ERR: mic raw" in raw: print("cihaz:", raw[raw.find(b"ERR: mic raw"):][:50]); break
    p.close()

    w = 3 if bits == 24 else 2
    pcm = bytearray(); i = 0; beklenen = 0; kayip = bozuk = cerceve = tasma = 0; ilk = True; yer = []; metinler = []
    while True:
        i = raw.find(b"NARA", i)
        if i < 0 or i + 16 > len(raw): break
        seq, ln, ov, top = struct.unpack_from("<IHHI", raw, i + 4)
        if ln > 960 or i + 16 + ln > len(raw): i += 4; continue
        v = raw[i + 16:i + 16 + ln]
        if sum(v) != top:
            bozuk += 1
            import re
            m = re.findall(rb"[ -~]{12,}", bytes(raw[i:i + 16 + ln + 200]))
            if m and len(metinler) < 6: metinler.append(b" | ".join(m)[:110].decode("latin1"))
            i += 4; continue
        if ilk: beklenen = seq; ilk = False
        if seq > beklenen:
            kayip += seq - beklenen; pcm += bytes(960 * (seq - beklenen))
            yer.append(f"{beklenen * 320 / FS:.2f}s/{seq - beklenen}")
        pcm += v; beklenen = seq + 1; tasma = ov; cerceve += 1; i += 16 + ln
    n = len(pcm) // w
    metin = [l for l in raw[-400:].decode("latin1").splitlines() if "raw bitti" in l or "ERR" in l]
    # Kart analiz satırları (mic nabiz): akışın içine karışmış metinlerden ayıkla
    for l in raw.decode("latin1").splitlines():
        for k in ("[mic] nabiz", "[mic] pencereler", "[mic] SD kayit", "[mic] bellek"):
            if k in l: print("cihaz:", l[l.find("[mic]"):].strip()[:200], flush=True); break
    print(f"çerçeve {cerceve}, kayıp {kayip}, bozuk {bozuk}, tampon taşması {tasma}, örnek {n} ({n / FS:.1f} sn), bitti={bitti}", flush=True)
    for m in metinler: print("bozan metin:", m, flush=True)
    if yer: print("boşluklar (24 bit konum):", " ".join(yer[:20]), flush=True)
    if metin: print(metin[-1][metin[-1].find("[mic]"):].strip(), flush=True)
    if n < sec * FS / 2: print("kayıt yarım kaldı"); sys.exit(3)

    kes = int(float(os.environ.get("MIC_BASTAN_KES", "1.0")) * FS)
    if w == 2: vals = list(struct.unpack(f"<{n}h", bytes(pcm[:n * 2])))
    else: vals = [int.from_bytes(pcm[k * 3:k * 3 + 3], "little", signed=True) for k in range(n)]
    vals = vals[kes:]
    tam = 2 ** (bits - 1) - 1
    ham = max(abs(v) for v in vals)
    doygun = sum(1 for v in vals if abs(v) >= tam * 0.999)
    # 20 Hz DC süzgeci
    a = 0.99738; y = 0.0; x1 = float(vals[0]); out = []
    for v in vals:
        y = a * (y + v - x1); x1 = v; out.append(y)
    tepe = max(1.0, max(abs(v) for v in out))
    k = tam * 0.891 / tepe
    with wave.open(path, "wb") as f:
        f.setnchannels(1); f.setsampwidth(w); f.setframerate(FS)
        if w == 2: f.writeframes(struct.pack(f"<{len(out)}h", *[int(v * k) for v in out]))
        else: f.writeframes(b"".join(int(v * k).to_bytes(3, "little", signed=True) for v in out))
    print(f"{path}: {len(out) / FS:.1f} sn, ham tepe {ham}/{tam}, ADC doygun örnek {doygun}, kazanç ×{k:.2f}", flush=True)

if __name__ == "__main__":
    main()
