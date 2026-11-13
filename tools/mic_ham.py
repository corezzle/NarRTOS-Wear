#!/usr/bin/env python3
"""
mic_ham.py — ES7210 mikrofonundan HAM ses akışı, satır satır dosyaya (stetoskop deneyi).

Kullanım:
    ~/.platformio/penv/bin/python tools/mic_ham.py ~/Desktop/mic1.csv [sure_sn=30] [hz=1000] [kanal=1]
    (dosya varsa adına saat eklenir, üstüne yazmaz; Ctrl+C ile erken bitirilebilir)

Dosya: t_ms, deger  (saniyede `hz` satır; deger = 16-bit ADC örneklerinin DC'si alınmış, hz'e
indirgenmiş ortalaması; birim ham ADC). Ekranda saniyede bir özet: örnek sayısı, RMS, tepe.
Cihaz tarafı: firmware 'mic capture <sn> <hz> <kanal> [varyant]' (kanal 1 = MIC1, 2 = MIC2; varyant 2 = REG40 0xC3, MICBIAS açık).
"""
import glob, os, re, sys, time, math

strip = lambda s: re.sub(r"\x1b\[[0-9;]*m", "", s)

def main():
    if len(sys.argv) < 2:
        print("kullanım: mic_ham.py <dosya.csv> [sure_sn] [hz] [kanal] [varyant]"); sys.exit(1)
    path = sys.argv[1]
    sec = int(sys.argv[2]) if len(sys.argv) > 2 else 30
    hz  = int(sys.argv[3]) if len(sys.argv) > 3 else 1000
    ch  = int(sys.argv[4]) if len(sys.argv) > 4 else 1
    var = int(sys.argv[5]) if len(sys.argv) > 5 else 0
    if os.path.exists(path):
        b, e = os.path.splitext(path); path = f"{b}_{time.strftime('%H%M%S')}{e or '.csv'}"
    import serial
    # 2026-11-02: port bekleme + yeniden deneme. NAR SiP bazen ilk açılışta takılıyor;
    # uygulama 18 sn içinde "CmdSvc" basmazsa port kapatılıp yeniden açılır (kart resetlenir).
    deneme_sayisi = int(os.environ.get("MIC_DENEME", "6")); alive = False; p = None
    for deneme in range(1, deneme_sayisi + 1):
        t_wait = time.time(); ports = glob.glob("/dev/cu.usbmodem*")
        while not ports and time.time() - t_wait < 60:
            time.sleep(0.2); ports = glob.glob("/dev/cu.usbmodem*")
        if not ports: print(f"deneme {deneme}: USB portu yok — kartı tak"); continue
        try:
            p = serial.Serial(); p.port = ports[0]; p.baudrate = 115200; p.timeout = 0.1
            p.dtr = False; p.rts = False; p.open()
        except Exception as e:
            print(f"deneme {deneme}: port açılamadı ({e})"); time.sleep(2); continue
        print(f"deneme {deneme}: kart açılıyor…", flush=True)
        buf = b""; t0 = time.time()
        try:
            while time.time() - t0 < 18:
                buf += p.read(8192)
                if b"CmdSvc" in buf: alive = True; break
        except Exception as e:
            print(f"deneme {deneme}: USB koptu ({e})", flush=True)
        if alive:
            print(f"UYGULAMA AYAKTA ({time.strftime('%H:%M:%S')})", flush=True); time.sleep(3.0); break
        print(f"deneme {deneme}: açılmadı", flush=True)
        try: p.close()
        except Exception: pass
        time.sleep(1.0)
    if not alive: print("kart açılmadı — fişten çekip soğutun, tekrar deneyin"); sys.exit(2)
    f = open(path, "w"); f.write("t_ms,deger\n"); iz = open(path + ".iz", "w")
    print(f"kayıt: {path}   ({sec} sn, {hz} Hz, kanal {ch})")
    print("saat      örnek    RMS     tepe")
    n_total = 0; sec_vals = []; sec_t = time.time(); done = False
    p.reset_input_buffer(); p.write(f"mic capture {min(sec, 60)} {hz} {ch} {var}\r\n".encode())
    remaining = sec - min(sec, 60); pending = b""; t_start = time.time()
    try:
        while not done:
            chunk = p.read(4096)
            if chunk: pending += chunk
            while b"\n" in pending:
                line, pending = pending.split(b"\n", 1)
                l = strip(line.decode("utf-8", "replace")).strip()
                if l.startswith("[mic] "):
                    toks = l[6:].split()
                    # Satır kaybı izi (2026-11-06): cihaz zaman damgası + o ana kadarki örnek sayısı.
                    if toks and toks[0].isdigit(): iz.write(f"{toks[0]},{n_total}\n")
                    for v in toks[1:]:
                        if v.lstrip("-").isdigit():
                            t_ms = int(n_total * 1000 / hz); n_total += 1
                            f.write(f"{t_ms},{v}\n"); sec_vals.append(int(v))
                    if time.time() - sec_t >= 1.0 and sec_vals:
                        rms = math.sqrt(sum(x * x for x in sec_vals) / len(sec_vals)); pk = max(abs(x) for x in sec_vals)
                        print(f"{time.strftime('%H:%M:%S')}  {n_total:7d}  {rms:7.1f}  {pk:6d}", flush=True)
                        sec_vals = []; sec_t = time.time(); f.flush()
                elif "mic capture bitti" in l:
                    if remaining > 0:
                        p.write(f"mic capture {min(remaining, 60)} {hz} {ch} {var}\r\n".encode()); remaining -= min(remaining, 60)
                    else: done = True
                elif "ERR: mic" in l or "MicSvc" in l and ("HATA" in l or "kurulamadi" in l or "cevap" in l):
                    print("cihaz:", l); done = True
            if time.time() - t_start > sec + 20: done = True
    except KeyboardInterrupt:
        pass
    finally:
        f.close(); iz.close(); p.close()
        print(f"\nbitti: {n_total} örnek ({n_total / hz:.1f} sn) → {path}")

if __name__ == "__main__":
    main()
