#!/usr/bin/env python3
"""
serve_ota.py — Yerel OTA test sunucusu.

Saatin Software Updates ekranı build-time tanımlı OTA_MANIFEST_URL'yi GET
eder. Production'da bu URL gerçek HTTPS backend olur; geliştirme/test için
bu Python scripti aynı LAN üzerinde manifest + firmware sunar.

Kullanım:
    1. Bu klasöre veya başka bir klasöre `firmware.bin`'i kopyala (PlatformIO
       build output: .pio/build/esp32s3_wearable/firmware.bin).
    2. Aynı klasörde `manifest.json` oluştur (örnek aşağıda).
    3. Bilgisayarın LAN IP'sini öğren (macOS: `ipconfig getifaddr en0`,
       Linux: `hostname -I`, Windows: `ipconfig`).
    4. Bu scripti çalıştır:
           python3 tools/serve_ota.py
       (default: 0.0.0.0:8088, --port ile değiştir)
    5. Saatte os_config.h'i güncelle:
           #define OTA_MANIFEST_URL "http://<LAN_IP>:8088/manifest.json"
       Build + flash et.
    6. Saatte Settings → General → Software Updates → "v1.1.0 mevcut" görünür,
       "Indir ve Yukle" → progress bar dolar → reboot.

Manifest.json örneği:
    {
      "version":      "0.2.0",
      "version_code": 200,
      "url":          "http://<LAN_IP>:8088/firmware.bin",
      "size":         2470000,
      "notes":        "Bug fixes and selector mode improvements"
    }

Notlar:
    - manifest.json ile firmware.bin AYNI klasörde olmalı (script cwd'den serve ediyor).
    - "size" alanı manifest'te byte cinsinden; OTA UI bunu progress % hesabı
      için kullanır. `os.path.getsize("firmware.bin")` ile kolayca bul.
    - "version_code" mevcut OS_VERSION_CODE'dan büyük olmalı; aksi halde
      "Sisteminiz guncel" görünür. v0.1.0 = 100, v0.2.0 = 200, v1.0.0 = 10000.
    - HTTP (HTTPS değil) — dev/internal LAN için. Production'da nginx + TLS.
"""

import argparse
import http.server
import json
import os
import socket
import socketserver
import sys
from pathlib import Path


def get_lan_ip() -> str:
    """Default route'taki interface'in IP'sini döner (best-effort)."""
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except Exception:
        return "127.0.0.1"


def autogen_manifest(serve_dir: Path, lan_ip: str, port: int,
                     version: str, version_code: int, notes: str) -> Path:
    """Manifest'i HER ÇALIŞTIRMADA YENİDEN üret — komut satırı argümanları
    authoritative. Önceki versiyon "manifest varsa atla" mantığındaydı; bu
    yüzden kullanıcı --version 0.3.0 --version-code 300 ile başlattığında
    eski manifest.json (v0.2.0/200) kullanılıyor, parametreler görmezden
    geliniyordu → saat "Sisteminiz guncel" diyordu çünkü manifest eski."""
    manifest_path = serve_dir / "manifest.json"
    fw_path       = serve_dir / "firmware.bin"

    if not fw_path.exists():
        print(f"[serve_ota] HATA: firmware.bin yok ({serve_dir})")
        print(f"[serve_ota] Önce şunu çalıştır:")
        print(f"             cp .pio/build/esp32s3_wearable/firmware.bin {serve_dir}/")
        sys.exit(1)

    size = fw_path.stat().st_size
    manifest = {
        "version":      version,
        "version_code": version_code,
        "url":          f"http://{lan_ip}:{port}/firmware.bin",
        "size":         size,
        "notes":        notes,
    }
    # ensure_ascii=False: Türkçe karakter / em-dash gibi non-ASCII içeriği
    # \uXXXX escape'i yerine literal UTF-8 byte olarak yaz. Saatteki strstr
    # tabanlı parser unicode escape decode edemiyor; literal UTF-8 cihazda
    # font'un desteklediği aralığa düşerse direkt görünür (Mont fonts ASCII
    # only — Türkçe glyph'leri kutu/eksik karakter olarak gösterir; bu yüzden
    # notes alanında Türkçe karakter kullanmamak en sağlıklı).
    manifest_path.write_text(json.dumps(manifest, indent=2, ensure_ascii=False),
                             encoding="utf-8")
    print(f"[serve_ota] Manifest yazıldı: {manifest_path}")
    print(json.dumps(manifest, indent=2, ensure_ascii=False))
    return manifest_path


class QuietHandler(http.server.SimpleHTTPRequestHandler):
    """Erişim log'larını terminale güzel yazar."""

    def log_message(self, fmt: str, *args) -> None:
        sys.stderr.write(f"  ↳ {self.address_string()} → {fmt % args}\n")


def main() -> None:
    ap = argparse.ArgumentParser(description="Yerel OTA test sunucusu")
    ap.add_argument("--dir", default="tools/ota_serve",
                    help="Serve edilecek klasör (default: tools/ota_serve)")
    ap.add_argument("--port", type=int, default=8088, help="Port (default: 8088)")
    ap.add_argument("--bind", default="0.0.0.0", help="Bind addr (default: 0.0.0.0)")
    ap.add_argument("--version", default="0.2.0", help="Manifest version string")
    ap.add_argument("--version-code", type=int, default=200,
                    help="Manifest version_code (>OS_VERSION_CODE olmalı; mevcut firmware 0.1.0=100)")
    ap.add_argument("--notes", default="Bug fixes and improvements",
                    help="Release notes (max 128 char)")
    args = ap.parse_args()

    serve_dir = Path(args.dir).resolve()
    serve_dir.mkdir(parents=True, exist_ok=True)

    # IP seçimi: kullanıcı --bind ile somut IP verdiyse onu kullan; yoksa
    # default route üzerinden auto-detect (VPN aktifse VPN IP'sini bulabilir).
    if args.bind not in ("0.0.0.0", ""):
        lan_ip = args.bind
    else:
        lan_ip = get_lan_ip()
    autogen_manifest(serve_dir, lan_ip, args.port,
                     args.version, args.version_code, args.notes)

    os.chdir(serve_dir)

    socketserver.TCPServer.allow_reuse_address = True
    with socketserver.TCPServer((args.bind, args.port), QuietHandler) as httpd:
        print()
        print("═══════════════════════════════════════════════════════════════")
        print(f"  OTA test sunucusu çalışıyor")
        print(f"  Klasör   : {serve_dir}")
        print(f"  Bind     : {args.bind}:{args.port}")
        print(f"  Manifest : http://{lan_ip}:{args.port}/manifest.json")
        print(f"  Firmware : http://{lan_ip}:{args.port}/firmware.bin")
        print()
        print(f"  Saat tarafında:")
        print(f"    src/core/os_config.h içinde OTA_MANIFEST_URL'yi güncelle:")
        print(f'    #define OTA_MANIFEST_URL "http://{lan_ip}:{args.port}/manifest.json"')
        print(f"  Sonra build + flash et, Settings → General → Software Updates'e gir.")
        print()
        print("  Ctrl+C ile kapat.")
        print("═══════════════════════════════════════════════════════════════")
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\n[serve_ota] Kapatılıyor.")


if __name__ == "__main__":
    main()
