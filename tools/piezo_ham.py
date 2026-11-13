#!/usr/bin/env python3
"""
piezo_ham.py — Piezo sensöründen HAM veri akışı, satır satır dosyaya. Hiçbir filtre/işleme yok.

Kullanım:
    ~/.platformio/penv/bin/python tools/piezo_ham.py ~/Desktop/ham1.csv [sure_sn]
    (sure verilmezse Ctrl+C'ye kadar sürer; dosya varsa adına saat eklenir, üstüne yazmaz)

Dosya biçimi (CSV, her satır bir örnek, saniyede 200 satır):
    t_ms, ham, ax_mg, ay_mg, az_mg
    t_ms : kayıt başından beri geçen milisaniye
    ham  : ADC değeri 0-4095 (piezo modülü A çıkışı, 12-bit; ~0.8 mV/birim)
    ax..az : ivmeölçer (mg, 50 Hz'de güncellenir; aradaki örneklerde son değer tekrar eder)

Ekranda saniyede bir özet: örnek sayısı, ham min/ort/max, ivme sapması.
Cihaz tarafı: firmware 'piezo capture 60 200' komutu (60 sn'lik parçalar, arka arkaya).
"""
import glob, os, re, sys, time, math

HZ = 200
strip = lambda s: re.sub(r"\x1b\[[0-9;]*m", "", s)

def main():
    if len(sys.argv) < 2:
        print("kullanım: piezo_ham.py <dosya.csv> [sure_sn]"); sys.exit(1)
    path = sys.argv[1]; total_s = float(sys.argv[2]) if len(sys.argv) > 2 else None
    if os.path.exists(path):
        b, e = os.path.splitext(path); path = f"{b}_{time.strftime('%H%M%S')}{e or '.csv'}"
    import serial
    ports = glob.glob("/dev/cu.usbmodem*")
    if not ports: print("USB portu yok — kartı tak"); sys.exit(1)
    p = serial.Serial(); p.port = ports[0]; p.baudrate = 115200; p.timeout = 0.1
    p.dtr = False; p.rts = False; p.open()
    print("kart açılıyor (port açılınca kart yeniden başlar, ~10 sn)…")
    buf = b""; t0 = time.time()
    while time.time() - t0 < 60:
        buf += p.read(8192)
        if b"giris olarak kuruldu" in buf: time.sleep(1.0); break
    f = open(path, "w"); f.write("t_ms,ham,ax_mg,ay_mg,az_mg\n")
    print(f"kayıt: {path}   (Ctrl+C ile bitir)")
    print("saat      örnek   ham min/ort/max     ivme sapması(mg)")
    n_total = 0; t_start = time.time(); last_acc = (0, 0, 0)
    sec_vals = []; sec_acc = []; sec_t = time.time()
    try:
        while total_s is None or time.time() - t_start < total_s:
            p.reset_input_buffer(); p.write(b"piezo capture 60 200\r\n")
            pending = b""; done = False; t1 = time.time()
            while not done and time.time() - t1 < 65:
                chunk = p.read(4096)
                if not chunk: continue
                pending += chunk
                while b"\n" in pending:
                    line, pending = pending.split(b"\n", 1)
                    l = strip(line.decode("utf-8", "replace")).strip()
                    if l.startswith("[cap] "):
                        pz, _, ac = l[6:].partition("|")
                        vals = [int(x) for x in pz.split()[1:] if x.lstrip("-").isdigit()]
                        a = [int(x) for x in ac.split() if x.lstrip("-").isdigit()]
                        trip = [tuple(a[i:i + 3]) for i in range(0, len(a) - 2, 3)]
                        for k, v in enumerate(vals):
                            if trip: last_acc = trip[min(len(trip) - 1, k // 4)]
                            t_ms = int(n_total * 1000 / HZ); n_total += 1
                            f.write(f"{t_ms},{v},{last_acc[0]},{last_acc[1]},{last_acc[2]}\n")
                            sec_vals.append(v); sec_acc.append(math.sqrt(sum(c * c for c in last_acc)))
                        if time.time() - sec_t >= 1.0 and sec_vals:
                            m = sum(sec_vals) / len(sec_vals); am = sum(sec_acc) / len(sec_acc)
                            dev = math.sqrt(sum((x - am) ** 2 for x in sec_acc) / len(sec_acc))
                            print(f"{time.strftime('%H:%M:%S')}  {n_total:6d}   {min(sec_vals):4d}/{m:6.1f}/{max(sec_vals):4d}     {dev:6.1f}", flush=True)
                            sec_vals = []; sec_acc = []; sec_t = time.time(); f.flush()
                    elif "capture bitti" in l: done = True
                    elif "ERR: piezo" in l: print("cihaz:", l); time.sleep(2); done = True
                if total_s is not None and time.time() - t_start >= total_s: break
    except KeyboardInterrupt:
        pass
    finally:
        f.close(); p.close()
        print(f"\nbitti: {n_total} örnek ({n_total / HZ:.1f} sn) → {path}")

if __name__ == "__main__":
    main()
