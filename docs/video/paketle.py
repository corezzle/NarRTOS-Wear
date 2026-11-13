#!/usr/bin/env python3
"""YouTube paketi: python3 paketle.py <proje>
Girdi : <proje>/out/narrtos-<proje>.mp4 (render), <proje>/out/meta.json (build.py yazar), <proje>/scenes.json (youtube bloğu)
Çıktı : out/narrtos-<proje>-youtube.mp4 (müzik + intro çanı miksli), out/<proje>.srt, out/<proje>-youtube.md, out/<proje>-thumbnail.png"""
import json, sys, os, subprocess
proj = sys.argv[1]; out = os.path.join(proj, "out")
meta = json.load(open(os.path.join(out, "meta.json"), encoding="utf-8")); yt = meta.get("youtube", {})
BGM = "bgm/ambient.wav"; CHIME = os.path.expanduser("~/.claude/skills/media-use/audio/assets/sfx/chime.mp3")
BGM_DB = -13   # müzik seviyesi (~-33 LUFS; seslendirme gelince -18 yapılır)

def ts(sec, srt=False):
    h, m = int(sec // 3600), int(sec % 3600 // 60); s = sec % 60
    if srt: return f"{h:02d}:{m:02d}:{int(s):02d},{int(round((s - int(s)) * 1000)):03d}"
    return f"{h:02d}:{m:02d}:{int(s):02d}" if h else f"{m:02d}:{int(s):02d}"

# 1) SRT
lines = []
for i, c in enumerate(meta["captions"], 1):
    lines += [str(i), f"{ts(c['start'], True)} --> {ts(c['end'], True)}", c["text"], ""]
open(os.path.join(out, f"{proj}.srt"), "w", encoding="utf-8").write("\n".join(lines))

# 2) Başlık / açıklama / bölümler / etiketler
chapters = "\n".join(f"{ts(ch['t'])} {ch['title']}" for ch in meta["chapters"])
title = yt.get("yt_title", f"NarRTOS Wear · {meta['title']}")
desc = f"""{yt.get("description", "")}

📄 Yazılı döküman: {yt.get("doc_url", "https://www.corezzle.com")}
💻 Kaynak kod: https://{yt.get("github", "github.com/corezzle/NarRTOS-Wear")}
🌐 {yt.get("site", "corezzle.com")}

Bölümler
{chapters}

NarRTOS Wear, Corezzle Electronics tarafından geliştirilen, ESP32-S3 tabanlı akıllı saatler için açık kaynak gömülü işletim sistemidir. Bu seri, resmi dökümantasyonun video haline getirilmiş sürümüdür.

{" ".join("#" + "".join(ch for ch in t if ch.isalnum()) for t in yt.get("tags", [])[:8])}"""
md = f"""# YouTube paketi — {meta['title']}

## Başlık
{title}

## Açıklama
{desc}

## Etiketler
{", ".join(yt.get("tags", []))}

## Yükleme ayarları
- Kategori: Bilim ve Teknoloji · Dil: Türkçe · Altyazı: `{proj}.srt` dosyasını "Türkçe" olarak yükleyin
- Küçük resim: `{proj}-thumbnail.png` (1280×720)
- Oynatma listesi: {yt.get("playlist", "NarRTOS Wear Dökümantasyon")}
- Son ekran (son 10 sn): sol kutuya "{yt.get("next", "sıradaki video")}" videosu, sağ kutuya oynatma listesi, ortadaki daireye abone ol öğesi
- Sabit yorum: yazılı dökümanın linki
"""
open(os.path.join(out, f"{proj}-youtube.md"), "w", encoding="utf-8").write(md)

# 3) Ses miksi: döngülü ambient + intro çanı; video kopyalanır (yeniden kodlanmaz)
src = os.path.join(out, f"narrtos-{proj}.mp4"); dst = os.path.join(out, f"narrtos-{proj}-youtube.mp4")
T = meta["total"]
fc = (f"[1:a]volume={BGM_DB}dB,afade=t=in:d=2.5,afade=t=out:st={T-4:.2f}:d=4,atrim=0:{T:.2f}[m];"
      f"[2:a]adelay=350|350,volume=-9dB[c];[m][c]amix=inputs=2:duration=first:normalize=0[a]")
subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", src, "-stream_loop", "-1", "-i", BGM, "-i", CHIME,
                "-filter_complex", fc, "-map", "0:v", "-map", "[a]", "-c:v", "copy", "-c:a", "aac", "-b:a", "192k",
                "-movflags", "+faststart", "-shortest", dst], check=True)

# 4) Küçük resim
subprocess.run([sys.executable, "thumb.py", proj], check=True)
for f in sorted(os.listdir(out)): print(f"  {f:44s} {os.path.getsize(os.path.join(out, f)) / 1e6:7.2f} MB")
