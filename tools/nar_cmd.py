#!/usr/bin/env python3
"""
nar_cmd.py — NAR kartına seri komut gönderir, cevabı ekrana yazar (mikrofon/pad/PMIC teşhisi).

Kullanım:
    ~/.platformio/penv/bin/python tools/nar_cmd.py "<komut>" [bekleme_sn=6]

Örnekler:
    nar_cmd.py "pad test 15 14 16 41 40"   ped sürme / ayrım / volt testi (kısa devre, kelepçe, Hi-Z)
    nar_cmd.py "get temp"                  PMIC çip sıcaklığı
    nar_cmd.py "get rails"                 PMIC rayları
    nar_cmd.py "mic clocks" 40             I2S saatleri + SDOUT canlılık + stereo I2S + ES7210 master testi
    nar_cmd.py "info"                      sürüm / uptime
    nar_cmd.py "imu apex tilt on; imu events 30" 40   birden çok komut: ";" ile ayır, aynı açılışta sırayla gider

Not: port açılınca kart yeniden başlar (~10 sn); betik açılışı bekleyip komutu sonra gönderir.
Seri port meşgulse (monitör açık) kapatın. Sadece [cmd]/[mic]/[pad]/[imu]/MicSvc satırları gösterilir.
Ayar komutları (imu apex …) RAM'de durur; port her açılışta kartı resetlediği için aynı çağrıda ";" ile verin.
"""
import glob, re, sys, time

try:
    import serial
except ImportError:
    sys.exit("pyserial yok: ~/.platformio/penv/bin/python ile çalıştırın")

strip = lambda s: re.sub(r"\x1b\[[0-9;]*m", "", s)

def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    cmd = sys.argv[1]
    wait = float(sys.argv[2]) if len(sys.argv) > 2 else 6.0
    ports = glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/ttyACM*")
    if not ports:
        sys.exit("kart yok (/dev/cu.usbmodem* / /dev/ttyACM*)")
    p = serial.Serial(ports[0], 115200, timeout=0.2)
    print(f"port {ports[0]} — kart açılıyor…", flush=True)
    t0 = time.time()
    ready = False
    while time.time() - t0 < 25:                       # açılış logunu bekle
        l = strip(p.readline().decode(errors="replace")).strip()
        if "Command service" in l or "cmd ready" in l or "[cmd]" in l or "CmdSvc" in l:
            ready = True; break
    if not ready:
        time.sleep(2)                                  # açılış işaretini yakalayamadıysak yine de dene
    p.reset_input_buffer()
    for i, c in enumerate(x.strip() for x in cmd.split(";")):
        if not c:
            continue
        if i:
            time.sleep(0.5)                            # önceki komut işlensin
        p.write((c + "\r\n").encode()); p.flush()
    t0 = time.time()
    while time.time() - t0 < wait:
        l = strip(p.readline().decode(errors="replace")).strip()
        if not l:
            continue
        if any(k in l for k in ("[cmd]", "[mic]", "[pad]", "[imu]", "MicSvc")):
            print(l, flush=True)
    p.close()

if __name__ == "__main__":
    main()
