#!/usr/bin/env python3
"""Seri köprü: portu sürekli açık tutar (her open cihazı resetliyor).
TCP 127.0.0.1:5577 — istemci 'cmd\n' yollar, köprü cihaza iletir, wait sn
boyunca gelen her şeyi döndürür (ilk satır 'WAIT=x' ile süre ayarlanabilir).
Tüm seri çıkışı ayrıca wbridge.log'a yazar."""
import serial, socket, threading, time, sys, glob, os
def find_port():
    # LCD128 kartı CH34x USB-UART köprüsü kullanıyor (cu.wchusbserial*);
    # AMOLED kartında USB-CDC (cu.usbmodem*) çıkar — ikisini de tara.
    return (glob.glob('/dev/cu.wchusbserial*') or glob.glob('/dev/cu.usbmodem*') or [None])[0]
PORT = find_port()
if not PORT:
    print("port yok — USB kabloyu kontrol et"); sys.exit(1)
LOG = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'wbridge.log')
ser = serial.Serial(None, 115200, timeout=0.05)
ser.port = PORT; ser.dtr = False; ser.rts = False; ser.open()
lock = threading.Lock()
buf = bytearray()
logf = open(LOG, 'ab', buffering=0)
def open_port():
    global ser
    while True:
        ports = [p for p in [find_port()] if p]
        if ports:
            try:
                s2 = serial.Serial(None, 115200, timeout=0.05)
                s2.port = ports[0]; s2.dtr = False; s2.rts = False; s2.open()
                ser = s2; print("bridge (re)opened", ports[0], flush=True); return
            except Exception as e:
                print("open fail", e, flush=True)
        time.sleep(0.5)
def reader():
    global ser
    while True:
        try:
            d = ser.read(4096)
        except Exception as e:
            print("port lost:", e, "- waiting for re-enumeration (device light sleep?)", flush=True)
            logf.write(b"\n[bridge] PORT LOST\n")
            try: ser.close()
            except Exception: pass
            time.sleep(1.0); open_port(); continue
        if d:
            logf.write(d)
            with lock: buf.extend(d)
threading.Thread(target=reader, daemon=True).start()
srv = socket.socket(); srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(('127.0.0.1', 5577)); srv.listen(4)
print("bridge on", PORT, flush=True)
while True:
    c, _ = srv.accept()
    data = b''
    while not data.endswith(b'\n\n'):
        d = c.recv(4096)
        if not d: break
        data += d
    lines = data.decode().strip().splitlines()
    wait = 1.0
    if lines and lines[0].startswith('WAIT='):
        wait = float(lines.pop(0)[5:])
    out = b''
    for ln in lines:
        with lock: buf.clear()
        try: ser.write((ln + '\n').encode())
        except Exception as e: out += ('[bridge] write fail: %s\n' % e).encode(); continue
        time.sleep(wait)
        with lock: chunk = bytes(buf); buf.clear()
        out += b'>>> ' + ln.encode() + b'\n' + chunk
    c.sendall(out); c.close()
