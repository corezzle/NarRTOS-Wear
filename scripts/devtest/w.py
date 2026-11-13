#!/usr/bin/env python3
import socket, sys
wait = '1.0'; cmds = []
for a in sys.argv[1:]:
    if a.startswith('--wait='): wait = a[7:]
    else: cmds.append(a)
s = socket.create_connection(('127.0.0.1', 5577))
s.sendall(('WAIT=%s\n' % wait + '\n'.join(cmds) + '\n\n').encode())
out = b''
while True:
    d = s.recv(65536)
    if not d: break
    out += d
txt = out.decode('utf-8', 'replace')
full = '--full' in sys.argv
for l in txt.splitlines():
    if full or l.startswith('>>>') or '[cmd]' in l or '[ui]' in l or 'Guru' in l or 'panic' in l.lower() or 'abort()' in l or 'rst:0x' in l or 'E (' in l:
        print(l)
