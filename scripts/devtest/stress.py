#!/usr/bin/env python3
"""Otonom stres testi — wbridge.py üzerinden rastgele 100+ adım.
Her adımda çökme (Guru/panic/rst:) ve [stall] (>100 ms kare) aranır.
Kullanım:  python scripts/devtest/stress.py [adım_sayısı] [tohum]
Ekran: 240x240 YUVARLAK — tap/drag koordinatları daire içinde üretilir."""
import socket, random, re, sys, math

W = H = 240
CX = CY = 120.0
RAD = 112.0   # bezel payı: daire yarıçapı 120, 8 px içeride kal


def send(cmds, wait=0.9):
    s = socket.create_connection(('127.0.0.1', 5577))
    s.sendall(('WAIT=%s\n' % wait + '\n'.join(cmds) + '\n\n').encode())
    out = b''
    while True:
        d = s.recv(65536)
        if not d:
            break
        out += d
    return out.decode('utf-8', 'replace')


def uptime():
    t = send(['info'], 0.8)
    m = re.search(r'"uptime_s":(\d+).*"heap_free":(\d+)', t)
    return (int(m.group(1)), int(m.group(2))) if m else (None, None)


def pt():
    """Daire içinde rastgele nokta (yuvarlak ekranda köşeler dokunulamaz)."""
    a = random.random() * 2 * math.pi
    r = RAD * math.sqrt(random.random())
    return int(CX + r * math.cos(a)), int(CY + r * math.sin(a))


N = int(sys.argv[1]) if len(sys.argv) > 1 else 100
random.seed(int(sys.argv[2]) if len(sys.argv) > 2 else 51)

apps = ['settings', 'launcher', 'watchface', 'hqmsg', 'timer', 'alarm',
        'weather', 'gallery', 'notifications', 'steps', 'heartrate', 'store']

u0, h0 = uptime()
print("start uptime", u0, "heap", h0, "steps", N)
crash = False
stalls = []
for i in range(N):
    r = random.random()
    if r < 0.35:
        cmd = "app launch %s" % random.choice(apps)
    elif r < 0.50:
        cmd = "app back"
    elif r < 0.65:
        cmd = "swipe %s" % random.choice(['left', 'right', 'up', 'down'])
    elif r < 0.86:
        x, y = pt()
        cmd = "tap %d %d" % (x, y)
    elif r < 0.93:
        cmd = random.choice(["wake", "brightness 200", "brightness 120",
                             "get wifi", "app current"])
    else:
        x0, y0 = pt()
        x1, y1 = pt()
        cmd = "drag %d %d %d %d 250" % (x0, y0, x1, y1)
    t = send([cmd], 0.7)
    if 'Guru' in t or 'abort()' in t or 'rst:0x' in t or 'panic' in t.lower():
        print("CRASH at step", i, cmd)
        print(t[-1500:])
        crash = True
        break
    for l in t.splitlines():
        if '[stall]' in l:
            stalls.append((i, cmd, l.strip()))
            print("STALL step", i, cmd, "|", l.strip())

u1, h1 = uptime()
print("end uptime", u1, "heap", h1,
      "| heap delta", (h1 - h0) if (h0 and h1) else '?',
      "| stalls", len(stalls))
print("CRASH" if crash else
      ("OK (uptime monotonic)" if u1 and u0 and u1 > u0 else "?? reboot suspected"))
