#!/usr/bin/env python3
"""NarRTOS Wear sürüm paketleyici.

Her board ortamı için üç şey üretir:
  1. OTA uygulama imajı        firmware-<sürüm>-<env>.bin   (cihazın kendi güncellediği dosya)
  2. Tek dosya tam flash imajı  narrtos-wear-<env>-<sürüm>-full.bin
     bootloader + partisyon tablosu + OTA verisi + uygulama (+ lcd128'de galeri) 0x0'dan
     yazılır; esptool merge-bin ile. Imager / tarayıcıdan yükleme bunu kullanır.
  3. Manifestler: manifest.json (bizim), web/<env>/manifest.json + parçalar (ESP Web Tools
     biçimi, tarayıcıdan yükleme), release/latest.json (yükleme sayfasının indeksi), SHA256SUMS.

Yükleme adresleri ve flash bayrakları elle tutulmaz: PlatformIO'nun kendi upload komutu
(sahte porta kuru deneme) okunur; galeri bölümü ortamın partisyon CSV'sinden gelir.

Kullanım:
  python3 scripts/release.py                       # iki ortamı derle, release/v<sürüm>/ üret
  python3 scripts/release.py --env esp32s3_wearable --no-build
  python3 scripts/release.py --upload https://ota.corezzle.com   # OTA imajını backend'e gönder
     (OTA_ADMIN_TOKEN ortam değişkeni gerekir; yalnız --upload-env ortamının imajı gider,
      backend sürüm başına tek dosya tutar)
"""
import argparse, configparser, hashlib, json, os, re, shutil, subprocess, sys, datetime, mimetypes, uuid, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PIO = os.path.expanduser("~/.platformio/penv/bin/pio")
PIO_PY = os.path.expanduser("~/.platformio/penv/bin/python")
if not os.path.exists(PIO):
    PIO = shutil.which("pio") or PIO
if not os.path.exists(PIO_PY):
    PIO_PY = sys.executable
DEFAULT_ENVS = ["esp32s3_wearable", "esp32s3_lcd128"]
# Imager ve tarayıcı sayfasında görünen kart adları (arayüz İngilizce).
BOARD_NAMES = {"esp32s3_wearable": "NarRTOS Wear · 410×502 AMOLED",
               "esp32s3_lcd128": "NarRTOS Wear · 240×240 round LCD",
               "esp32s3_18inch": "NarRTOS Wear · 1.8\" AMOLED"}
CHIP = "esp32s3"


def sh(cmd, **kw):
    print("  $", " ".join(str(c) for c in cmd))
    subprocess.run(cmd, check=True, **kw)


def os_version():
    src = open(os.path.join(ROOT, "src/core/os_config.h"), encoding="utf-8").read()
    v = {k: int(re.search(rf"#define OS_VERSION_{k}\s+(\d+)", src).group(1)) for k in ("MAJOR", "MINOR", "PATCH")}
    return f"{v['MAJOR']}.{v['MINOR']}.{v['PATCH']}"


def version_code(ver):
    a, b, c = (int(x) for x in ver.split("."))
    return a * 10000 + b * 100 + c


def env_partitions(env):
    """platformio.ini'den ortamın partisyon CSV'sini bul (extends zincirini izler)."""
    cp = configparser.ConfigParser(interpolation=None)
    cp.read(os.path.join(ROOT, "platformio.ini"), encoding="utf-8")
    sec = f"env:{env}"
    seen = set()
    while sec in cp and sec not in seen:
        seen.add(sec)
        if cp.has_option(sec, "board_build.partitions"):
            csv = cp.get(sec, "board_build.partitions").strip()
            break
        sec = cp.get(sec, "extends", fallback="")
    else:
        csv = "partitions.csv"
    table = {}
    for line in open(os.path.join(ROOT, csv), encoding="utf-8"):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        cols = [c.strip() for c in line.split(",")]
        if len(cols) >= 5 and cols[3]:
            table[cols[0]] = (int(cols[3], 16), int(cols[4], 16))
    return csv, table


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for blk in iter(lambda: f.read(1 << 20), b""):
            h.update(blk)
    return h.hexdigest()


def build(env):
    sh([PIO, "run", "-e", env], cwd=ROOT)


ESPTOOL = os.path.expanduser("~/.platformio/packages/tool-esptoolpy/esptool.py")


def pio_flash_plan(env):
    """PlatformIO'nun karta yazacağı dosya listesini ve flash bayraklarını, sahte bir porta
    kuru upload denemesi yaparak komut satırından okur. Böylece burada hiçbir adres veya
    bayrak elle tutulmaz; PIO ne yazıyorsa tam imaj da o olur."""
    r = subprocess.run([PIO, "run", "-v", "-e", env, "-t", "upload", "--upload-port", "/dev/cu.release-py-yok"],
                       cwd=ROOT, capture_output=True, text=True)
    line = next((l for l in (r.stdout + r.stderr).splitlines() if "write_flash" in l and "--flash_mode" in l), None)
    if not line:
        sys.exit(f"!! {env}: PlatformIO upload komutu okunamadı. Çıktı sonu:\n" + (r.stdout + r.stderr)[-1500:])
    flags = {k: re.search(rf"--{k} (\S+)", line).group(1) for k in ("flash_mode", "flash_freq", "flash_size")}
    tail = line.split("write_flash", 1)[1]
    pairs = re.findall(r"(0x[0-9a-fA-F]+|\b0\b) (\S+)", tail)
    parts = {}
    for addr, path in pairs:
        path = path.strip('"')
        if not os.path.isabs(path):
            path = os.path.join(ROOT, path)
        parts[int(addr, 16) if addr.startswith("0x") else int(addr)] = path
    return flags, parts


def esptool_merge(out, flags, parts):
    ver = subprocess.run([PIO_PY, ESPTOOL, "version"], capture_output=True, text=True).stdout
    v5 = bool(re.search(r"\bv?5\.", ver))
    cmd = [PIO_PY, ESPTOOL, "--chip", CHIP]
    if v5:
        cmd += ["merge-bin", "-o", out, "-f", "raw", "--flash-mode", flags["flash_mode"],
                "--flash-size", flags["flash_size"], "--flash-freq", flags["flash_freq"]]
    else:
        cmd += ["merge_bin", "-o", out, "--flash_mode", flags["flash_mode"],
                "--flash_size", flags["flash_size"], "--flash_freq", flags["flash_freq"]]
    for off in sorted(parts):
        cmd += [hex(off), parts[off]]
    sh(cmd)


def package(env, ver, out_dir, gallery):
    bdir = os.path.join(ROOT, ".pio/build", env)
    if not os.path.exists(os.path.join(bdir, "firmware.bin")):
        sys.exit(f"!! {bdir}/firmware.bin yok; önce derleyin (--no-build kaldırın)")
    flags, parts = pio_flash_plan(env)
    csv, table = env_partitions(env)

    # Galeri paketi (lcd128): partisyon tabloda varsa ve dosya üretilmişse tam imaja eklenir.
    if "gallery" in table and gallery and os.path.exists(gallery):
        off, size = table["gallery"]
        if os.path.getsize(gallery) > size:
            sys.exit(f"!! {gallery} galeri bölümüne sığmıyor ({os.path.getsize(gallery)} > {size})")
        parts[off] = gallery
    elif "gallery" in table:
        print(f"  ! {env}: galeri bölümü var ama {gallery} yok; tam imaj galerisiz üretiliyor")

    app_bin = next(p for p in parts.values() if os.path.basename(p) == "firmware.bin")
    os.makedirs(out_dir, exist_ok=True)
    ota_out = os.path.join(out_dir, f"firmware-{ver}-{env}.bin")
    shutil.copyfile(app_bin, ota_out)

    full_out = os.path.join(out_dir, f"narrtos-wear-{env}-{ver}-full.bin")
    esptool_merge(full_out, flags, parts)

    info = {
        "chip": CHIP, "partitions_csv": csv,
        "flash_mode": flags["flash_mode"], "flash_size": flags["flash_size"], "flash_freq": flags["flash_freq"],
        "parts": [{"offset": hex(o), "file": os.path.relpath(p, ROOT)} for o, p in sorted(parts.items())],
        "ota_image": {"file": os.path.basename(ota_out), "size": os.path.getsize(ota_out), "sha256": sha256(ota_out)},
        "full_image": {"file": os.path.basename(full_out), "size": os.path.getsize(full_out), "sha256": sha256(full_out), "offset": "0x0"},
    }
    # ESP Web Tools manifesti — tarayıcıdan yükleme düğmesi bunu okur. Tam imaj yerine
    # parçalar ayrı verilir: 0xFF dolgu indirilmez/yazılmaz (lcd128'de 9 MB → ~3,3 MB) ve
    # tarayıcı 115200 baud'da yazdığı için süre buna göre kısalır.
    web_dir = os.path.join(out_dir, "web", env)
    os.makedirs(web_dir, exist_ok=True)
    web_parts = []
    for off, src in sorted(parts.items()):
        dst = os.path.join(web_dir, os.path.basename(src))
        shutil.copyfile(src, dst)
        web_parts.append({"path": os.path.basename(src), "offset": off})
    ewt = {"name": BOARD_NAMES.get(env, env), "version": ver,
           # Imager'ın ESP32 yolu okur; ESP Web Tools bu ek alanları yok sayar.
           "flash_mode": flags["flash_mode"], "flash_size": flags["flash_size"], "flash_freq": flags["flash_freq"],
           "new_install_prompt_erase": True,      # ilk kurulumda "flash'ı sil" seçeneği sunar
           "new_install_improv_wait_time": 0,     # firmware Improv Wi-Fi konuşmuyor; bekleme yok
           "builds": [{"chipFamily": "ESP32-S3", "parts": web_parts}]}
    json.dump(ewt, open(os.path.join(web_dir, "manifest.json"), "w"), indent=2, ensure_ascii=False)
    info["web_manifest"] = os.path.relpath(os.path.join(web_dir, "manifest.json"), out_dir)
    return info


def upload(base, env, ver, out_dir, channel, notes):
    token = os.environ.get("OTA_ADMIN_TOKEN")
    if not token:
        sys.exit("!! OTA_ADMIN_TOKEN ortam değişkeni yok")
    path = os.path.join(out_dir, f"firmware-{ver}-{env}.bin")
    boundary = uuid.uuid4().hex
    fields = {"version": ver, "channel": channel, "release_notes": notes or f"NarRTOS Wear {ver} ({env})"}
    body = b""
    for k, v in fields.items():
        body += f"--{boundary}\r\nContent-Disposition: form-data; name=\"{k}\"\r\n\r\n{v}\r\n".encode()
    body += (f"--{boundary}\r\nContent-Disposition: form-data; name=\"binary\"; filename=\"firmware-{ver}.bin\"\r\n"
             "Content-Type: application/octet-stream\r\n\r\n").encode() + open(path, "rb").read() + f"\r\n--{boundary}--\r\n".encode()
    req = urllib.request.Request(base.rstrip("/") + "/releases/upload", data=body, method="POST",
                                 headers={"Content-Type": f"multipart/form-data; boundary={boundary}", "Authorization": f"Bearer {token}"})
    with urllib.request.urlopen(req, timeout=120) as r:
        print("  backend:", r.status, r.read()[:300].decode(errors="replace"))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--env", action="append", help="ortam (tekrarlanabilir); varsayılan: " + ", ".join(DEFAULT_ENVS))
    ap.add_argument("--no-build", action="store_true", help="derleme yapma, mevcut .pio/build çıktısını kullan")
    ap.add_argument("--out", help="çıktı klasörü (varsayılan release/v<sürüm>)")
    ap.add_argument("--gallery", default=os.path.join(ROOT, "build_assets/gallery_pack.bin"), help="lcd128 galeri paketi")
    ap.add_argument("--upload", metavar="BACKEND_URL", help="OTA uygulama imajını backend'e gönder (/releases/upload)")
    ap.add_argument("--upload-env", default="esp32s3_wearable", help="backend'e hangi ortamın imajı gitsin")
    ap.add_argument("--channel", default="stable")
    ap.add_argument("--notes", default="")
    a = ap.parse_args()

    ver = os_version()
    envs = a.env or DEFAULT_ENVS
    out_dir = a.out or os.path.join(ROOT, "release", f"v{ver}")
    commit = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=ROOT, capture_output=True, text=True).stdout.strip()
    print(f"NarRTOS Wear v{ver} ({commit}) → {os.path.relpath(out_dir, ROOT)}/")

    manifest = {"product": "NarRTOS Wear", "version": ver, "version_code": version_code(ver), "commit": commit,
                "date": datetime.datetime.now().astimezone().isoformat(timespec="seconds"), "builds": {}}
    # Aynı sürüm için ortamlar ayrı çalıştırmalarda paketlenebilir; önceki manifesti koru,
    # yalnız bu çalıştırmanın ortamlarını güncelle.
    prev = os.path.join(out_dir, "manifest.json")
    if os.path.exists(prev):
        try:
            old = json.load(open(prev, encoding="utf-8"))
            if old.get("version") == ver:
                manifest["builds"] = old.get("builds", {})
        except (ValueError, OSError):
            pass
    for env in envs:
        print(f"\n== {env}")
        if not a.no_build:
            build(env)
        manifest["builds"][env] = package(env, ver, out_dir, a.gallery)

    json.dump(manifest, open(os.path.join(out_dir, "manifest.json"), "w"), indent=2, ensure_ascii=False)
    with open(os.path.join(out_dir, "SHA256SUMS"), "w") as f:
        for env, b in manifest["builds"].items():
            for k in ("ota_image", "full_image"):
                f.write(f"{b[k]['sha256']}  {b[k]['file']}\n")

    latest = {"version": ver, "version_code": manifest["version_code"], "date": manifest["date"],
              "boards": {env: {"name": BOARD_NAMES.get(env, env), "manifest": f"v{ver}/{b['web_manifest']}"}
                         for env, b in manifest["builds"].items() if "web_manifest" in b}}
    json.dump(latest, open(os.path.join(os.path.dirname(out_dir), "latest.json"), "w"), indent=2, ensure_ascii=False)

    print("\nÜretilenler:")
    for env, b in manifest["builds"].items():
        print(f"  {env}: OTA {b['ota_image']['file']} ({b['ota_image']['size']//1024} KB), "
              f"tam imaj {b['full_image']['file']} ({b['full_image']['size']//1024} KB), "
              f"{b['flash_mode']}/{b['flash_size']}/{b['flash_freq']}")
    print(f"  manifest.json, SHA256SUMS, web/<env>/manifest.json (+parçalar), ../latest.json")

    if a.upload:
        print(f"\n== backend'e yükle: {a.upload} ({a.upload_env})")
        upload(a.upload, a.upload_env, ver, out_dir, a.channel, a.notes)


if __name__ == "__main__":
    main()
