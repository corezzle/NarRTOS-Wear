#!/usr/bin/env python3
"""Doküman → sessiz, altyazılı YouTube videosu (HyperFrames kompozisyonu üretici).

Girdi:  <proje>/scenes.json  →  {"title","subtitle","brand","doc","scenes":[
            {"step":"1","title":"…","image":"assets/x.png","captions":["…","…"]}, …]}
Çıktı:  <proje>/index.html  (1920×1080, 30 fps). Süreler okuma hızından türetilir:
        her altyazı parçası max(2.8 s, kelime×0.45 s) + 0.35 s ara; sahne = giriş 0.6 + parçalar + 0.7.
Kullanım: python3 build.py imager   →  cd imager && hyperframes check && hyperframes render …
"""
import json, sys, os, html, re

W, H, FPS = 1920, 1080, 30
CARD = 5.0            # başlık ve kapanış kartı süresi
INTRO, OUTRO = 4.5, 10.0   # logo girişi, YouTube son ekranı (bitiş öğeleri için boş alan)
LOGO = "assets/logo.png"
A410 = ["watchface-analog", "launcher", "weather", "step-counter"]   # başlık kartındaki saat ekranları (gif-watch/assets)
R240 = ["watchface-digital", "launcher", "weather", "step-counter"]
GIF_ASSETS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "wiki-html", "gif-watch", "assets")
IN, OUT, GAP = 0.6, 0.7, 0.35

def cap_dur(text):
    return max(2.8, len(text.split()) * 0.45)

import subprocess, shutil
BOX_X, BOX_Y, BOX_W, BOX_H = 80, 215, 1540, 630   # başlık üstte, içerik altında tam genişlik; sağ altta avatar
AVATAR = "assets/avatar.svg"   # sağ alt köşe avatarı (yer tutucu; ileride kendi fotoğrafımız)

def media_size(path):
    out = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                          "stream=width,height", "-of", "csv=p=0", path], capture_output=True, text=True).stdout
    w, h = (int(x) for x in out.strip().split(",")[:2]); return w, h

def media_dur(path):
    out = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", path],
                         capture_output=True, text=True).stdout
    return float(out.strip() or 0)

def fit_box(w, h, max_scale=None):
    """Görseli 1540×630 alanına oranını koruyarak sığdır, ortala. max_scale: küçük kaynak fazla büyümesin."""
    s = min(BOX_W / w, BOX_H / h)
    if max_scale: s = min(s, max_scale)
    bw, bh = round(w * s), round(h * s)
    return BOX_X + (BOX_W - bw) // 2, BOX_Y + (BOX_H - bh) // 2, bw, bh

def loop_video(proj, src, need):
    """Video sahneden kısaysa SON KARESİNİ TUTARAK uzatır: komut bir kez yazılır, çıktı ekranda kalır.
    (Önceki sürüm döngülüyordu; terminal sahne ortasında boşalıp yeniden yazıyordu.)"""
    d = media_dur(os.path.join(proj, src))
    if d >= need: return src
    dst = re.sub(r"\.mp4$", f"-hold{int(need)}.mp4", src)
    if not os.path.exists(os.path.join(proj, dst)):
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", os.path.join(proj, src),
                        "-vf", f"tpad=stop_mode=clone:stop_duration={need - d + 0.5:.2f}",
                        "-c:v", "libx264", "-crf", "16", "-pix_fmt", "yuv420p", "-movflags", "+faststart",
                        os.path.join(proj, dst)], check=True)
    return dst

def build(proj):
    d = json.load(open(os.path.join(proj, "scenes.json"), encoding="utf-8"))
    scenes = d["scenes"]
    yt = d.get("youtube", {})
    if not os.path.exists(os.path.join(proj, LOGO)): shutil.copy("logo.png", os.path.join(proj, LOGO))
    for f in [f"a410-{x}.png" for x in A410] + [f"r240-{x}.png" for x in R240]:
        if not os.path.exists(os.path.join(proj, "assets", f)): shutil.copy(os.path.join(GIF_ASSETS, f), os.path.join(proj, "assets", f))
    t = INTRO + CARD
    plan = []                      # (start, dur, scene, [(cstart, cdur, text)])
    for s in scenes:
        caps, ct = [], t + IN
        for c in s["captions"]:
            cd = cap_dur(c); caps.append((ct, cd, c)); ct += cd + GAP
        dur = IN + sum(cd + GAP for _, cd, _ in caps) - GAP + OUT
        # Liste kartı: maddeler de okunacak; sahne en az maddelerin okuma süresi kadar sürsün.
        if s.get("items") or s.get("gallery") or s.get("code"):
            texts = s.get("items") or ([g["label"] for g in s["gallery"]] if s.get("gallery") else [s["code"][:400]])
            need = IN + sum(max(2.0, len(x.split()) * 0.40) for x in texts) + OUT
            if need > dur:
                extra = need - dur
                caps[-1] = (caps[-1][0], caps[-1][1] + extra, caps[-1][2])   # son altyazı ekranda kalır
                dur = need
        # Video sahnesi: klip bitmeden sahne bitmesin (son altyazı ekranda kalır).
        if s.get("image", "").lower().endswith(".mp4"):
            need = IN * 0.5 + media_dur(os.path.join(proj, s["image"])) + OUT + 0.3
            if need > dur:
                caps[-1] = (caps[-1][0], caps[-1][1] + (need - dur), caps[-1][2]); dur = need
        plan.append((t, dur, s, caps)); t += dur
    end_start = t; outro_start = round(t + CARD, 2); total = round(outro_start + OUTRO, 2)
    n = len(scenes)
    e = html.escape

    clips, tl = [], []
    # arka plan (tüm süre) + ilerleme çizgisi
    clips.append(f'''<div id="bg" class="clip" data-start="0" data-duration="{total}" data-track-index="0">
      <div class="grid"></div><div class="glow"></div>
      <div class="brand">{e(d["brand"])}</div><div class="doc">{e(d["doc"])}</div>
      <div class="progress"><div id="bar" class="bar"></div></div>
      <div class="avatar" id="avatar"><img src="{AVATAR}" alt=""></div></div>''')
    tl.append(f'tl.set("#avatar", {{opacity: 0}}, 0); tl.to("#avatar", {{opacity: 1, duration: .5}}, {INTRO + CARD - .2}); tl.to("#avatar", {{opacity: 0, duration: .4}}, {outro_start - .4:.2f});')
    # intro: logo + seri adı
    ep = yt.get("series_no")
    clips.append(f'''<div id="intro" class="clip" data-start="0" data-duration="{INTRO}" data-track-index="1">
      <img class="ilogo" id="intro-logo" src="{LOGO}" alt="">
      <div class="iseries" id="intro-series"><b>{e(d["brand"])}</b><span>{e(yt.get("series", "Dökümantasyon Serisi"))}{(" · Bölüm %s" % ep) if ep else ""}</span></div></div>''')
    tl.append('tl.fromTo("#intro-logo", {scale: .86, opacity: 0}, {scale: 1, opacity: 1, duration: 1.1, ease: "power3.out"}, .15);')
    tl.append('tl.to("#intro-logo", {y: -70, duration: .8, ease: "power3.inOut"}, 1.6);')
    tl.append('tl.fromTo("#intro-series", {y: 30, opacity: 0}, {y: 0, opacity: 1, duration: .7, ease: "power3.out"}, 2.0);')
    tl.append(f'tl.to(["#intro-logo","#intro-series"], {{opacity: 0, duration: .45, ease: "power2.in"}}, {INTRO - .5});')
    tl.append(f'tl.fromTo("#bar", {{scaleX: 0}}, {{scaleX: 1, duration: {total}, ease: "none"}}, 0);')
    # başlık kartı
    clips.append(f'''<div id="title" class="clip" data-start="{INTRO}" data-duration="{CARD}" data-track-index="1">
      <div class="card"><div class="k" id="title-k">{e(d["brand"])}</div>
      <h1 id="title-h">{e(d["title"])}</h1><p id="title-s">{e(d["subtitle"])}</p></div>
      <div class="devs" id="title-devs">
        <div class="b1"></div><div class="c1"></div><div class="w1">{"".join(f'<img id="tw1-{k}" src="assets/a410-{x}.png" alt="">' for k, x in enumerate(A410))}</div>
        <div class="b2"></div><div class="w2">{"".join(f'<img id="tw2-{k}" src="assets/r240-{x}.png" alt="">' for k, x in enumerate(R240))}</div>
      </div></div>''')
    tl.append(f'tl.from("#title-devs", {{x: 60, opacity: 0, duration: .9, ease: "power3.out"}}, {INTRO + .6});')
    for k in range(1, 4):
        tl.append(f'tl.to(["#tw1-{k}","#tw2-{k}"], {{opacity: 1, duration: .4, ease: "power2.inOut"}}, {INTRO + .9 + k * 1.05:.2f});')
    tl.append(f'tl.to("#title-devs", {{opacity: 0, duration: .45, ease: "power2.in"}}, {INTRO + CARD - .5});')
    tl.append(f'tl.from("#title-k", {{y: 30, opacity: 0, duration: .6, ease: "power3.out"}}, {INTRO + .2});')
    tl.append(f'tl.from("#title-h", {{y: 40, opacity: 0, duration: .8, ease: "power3.out"}}, {INTRO + .45});')
    tl.append(f'tl.from("#title-s", {{y: 30, opacity: 0, duration: .7, ease: "power3.out"}}, {INTRO + .8});')
    tl.append(f'tl.to(["#title-k","#title-h","#title-s"], {{opacity: 0, duration: .45, ease: "power2.in"}}, {INTRO + CARD - .5});')
    # sahneler
    for i, (start, dur, s, caps) in enumerate(plan, 1):
        sid = f"s{i}"
        capdivs = "".join(f'<div class="cap{" long" if len(txt) > 125 else ""}{" xlong" if len(txt) > 185 else ""}" id="{sid}-c{j}">{e(txt)}</div>' for j, (_, _, txt) in enumerate(caps))
        if s.get("code"):
            lines = s["code"].rstrip("\n").split("\n")
            big = len(lines) <= 11 and max(len(l) for l in lines) <= 92
            shot = (f'<div class="shot code{"" if big else " small"}" id="{sid}-shot" style="left:{BOX_X}px;top:{BOX_Y}px;width:{BOX_W}px;min-height:300px;max-height:{BOX_H}px">'
                    f'<pre>{e(s["code"].rstrip(chr(10)))}</pre></div>')
        elif s.get("gallery"):
            n_g = len(s["gallery"]); cols = 4 if n_g > 6 else 3
            cells = "".join(f'<figure><img src="{e(g["image"])}" alt="{e(g["label"])}" /><b>{e(g["label"])}</b><span>{e(g.get("sub",""))}</span></figure>' for g in s["gallery"])
            shot = (f'<div class="shot gallery" id="{sid}-shot" style="left:{BOX_X}px;top:{BOX_Y}px;width:{BOX_W}px;height:{BOX_H}px;'
                    f'grid-template-columns:repeat({cols},1fr)">{cells}</div>')
        elif s.get("items"):
            lis = "".join(f"<li>{e(x)}</li>" for x in s["items"])
            # Uzun listelerde yazı bir kademe küçülür ki son madde altyazının altına kaçmasın.
            dense = " dense" if sum(len(x) for x in s["items"]) > 420 or len(s["items"]) > 5 else ""
            shot = f'<div class="shot list{dense}" id="{sid}-shot" style="left:{BOX_X}px;top:{BOX_Y}px;width:{BOX_W}px;min-height:300px;max-height:{BOX_H}px"><ul>{lis}</ul></div>'
        else:
            src = s["image"]
            w, h = media_size(os.path.join(proj, src))
            x, y, bw, bh = fit_box(w, h, max_scale=2.4 if src.lower().endswith(".mp4") else 2.2)
            if src.lower().endswith(".mp4"):
                vstart, vdur = start + IN * 0.5, dur - IN * 0.5
                src = loop_video(proj, src, vdur + 0.5)
                media = (f'<video id="{sid}-v" src="{e(src)}" muted playsinline data-start="{vstart:.2f}" '
                         f'data-duration="{vdur:.2f}" data-track-index="{2 + i}"></video>')
            else:
                media = f'<img src="{e(src)}" alt="{e(s["title"])}" />'
            shot = f'<div class="shot" id="{sid}-shot" style="left:{x}px;top:{y}px;width:{bw}px;height:{bh}px">{media}</div>'
        clips.append(f'''<div id="{sid}" class="clip" data-start="{start:.2f}" data-duration="{dur:.2f}" data-track-index="1">
      <div class="inner" id="{sid}-in">
        <div class="left"><div class="steptitle">{e(s["title"])}</div></div><div class="stepmeta">{i} / {n}</div>
        {shot}
        <div class="caps">{capdivs}</div>
      </div></div>''')
        tl.append(f'tl.from("#{sid}-in", {{opacity: 0, duration: .45, ease: "power2.out"}}, {start:.2f});')
        tl.append(f'tl.from("#{sid}-shot", {{y: 36, duration: .7, ease: "power3.out"}}, {start:.2f});')
        tl.append(f'tl.to("#{sid}-in", {{opacity: 0, duration: .4, ease: "power2.in"}}, {start + dur - .42:.2f});')
        for j, (cs, cd, _) in enumerate(caps):
            tl.append(f'tl.fromTo("#{sid}-c{j}", {{opacity: 0, y: 14}}, {{opacity: 1, y: 0, duration: .35, ease: "power2.out"}}, {cs:.2f});')
            if j < len(caps) - 1:
                tl.append(f'tl.to("#{sid}-c{j}", {{opacity: 0, duration: .25, ease: "power2.in"}}, {cs + cd:.2f});')
    # kapanış
    clips.append(f'''<div id="end" class="clip" data-start="{end_start:.2f}" data-duration="{CARD}" data-track-index="1">
      <div class="card"><div class="k" id="end-k">{e(d["brand"])}</div>
      <h1 id="end-h">{e(d.get("end_title", "Devamı dokümanda"))}</h1><p id="end-s">{e(d.get("end_subtitle", ""))}</p></div></div>''')
    tl.append(f'tl.from(["#end-k","#end-h","#end-s"], {{y: 30, opacity: 0, duration: .7, stagger: .18, ease: "power3.out"}}, {end_start + .2:.2f});')

    tl.append(f'tl.to(["#end-k","#end-h","#end-s"], {{opacity: 0, duration: .45, ease: "power2.in"}}, {outro_start - .5:.2f});')
    nxt = yt.get("next", "Sıradaki video"); pl = yt.get("playlist", "Oynatma listesi")
    clips.append(f'''<div id="outro" class="clip" data-start="{outro_start:.2f}" data-duration="{OUTRO}" data-track-index="1">
      <div class="oh" id="outro-h">İzlediğiniz için teşekkürler</div>
      <div class="obox" id="outro-a" style="left:120px"><span>Sıradaki video</span><b>{e(nxt)}</b></div>
      <div class="obox" id="outro-b" style="left:1060px"><span>Oynatma listesi</span><b>{e(pl)}</b></div>
      <div class="osub" id="outro-s"><div class="ocircle"></div><div><b>Abone olun</b><span>Yeni bölümlerden haberdar olun</span></div></div>
      <img class="ologo" id="outro-logo" src="{LOGO}" alt="">
      <div class="olinks" id="outro-l">{e(yt.get("site", "corezzle.com"))} &nbsp;·&nbsp; {e(yt.get("github", "github.com/corezzle/NarRTOS-Wear"))}</div></div>''')
    tl.append(f'tl.from(["#outro-h","#outro-a","#outro-b","#outro-s","#outro-l","#outro-logo"], {{y: 30, opacity: 0, duration: .7, stagger: .14, ease: "power3.out"}}, {outro_start + .15:.2f});')
    # YouTube paketi için zamanlama verisi (altyazı SRT, bölümler)
    os.makedirs(os.path.join(proj, "out"), exist_ok=True)
    meta = {"title": d["title"], "doc": d["doc"], "brand": d["brand"], "total": total, "intro": INTRO, "youtube": yt,
            "chapters": [{"t": 0, "title": "Giriş"}] + [{"t": round(st, 2), "title": s["title"]} for st, _, s, _ in plan] + [{"t": round(end_start, 2), "title": "Kapanış"}],
            "captions": [{"start": round(cs, 2), "end": round(cs + cd, 2), "text": c} for _, _, _, caps in plan for cs, cd, c in caps]}
    json.dump(meta, open(os.path.join(proj, "out", "meta.json"), "w", encoding="utf-8"), ensure_ascii=False, indent=1)

    page = f'''<!doctype html>
<html lang="tr">
<head>
<meta charset="UTF-8" />
<meta name="viewport" content="width={W}, height={H}" />
<title>{e(d["title"])}</title>
<script src="https://cdn.jsdelivr.net/npm/gsap@3.14.2/dist/gsap.min.js"></script>
<style>
  body {{ margin:0; background:#00102E; font-family:Inter, system-ui, sans-serif; color:#E4F0EE; -webkit-font-smoothing:antialiased; }}
  #root {{ position:relative; width:{W}px; height:{H}px; overflow:hidden; }}
  .clip {{ position:absolute; inset:0; }}
  #bg {{ background:linear-gradient(140deg,#00102E 0%,#00234E 55%,#001B3F 100%); }}
  .grid {{ position:absolute; inset:0; opacity:.22;
    background-image:linear-gradient(#093159 1px,transparent 1px),linear-gradient(90deg,#093159 1px,transparent 1px); background-size:80px 80px; }}
  .glow {{ position:absolute; left:1100px; top:-260px; width:900px; height:900px; border-radius:50%;
    background:radial-gradient(circle,rgba(0,202,215,.16) 0%,rgba(0,202,215,0) 65%); }}
  .brand {{ position:absolute; left:80px; top:52px; font-size:20px; font-weight:800; letter-spacing:3px; color:#00CAD7; }}
  .doc {{ position:absolute; right:80px; top:50px; font-size:22px; color:#93AEC7; }}
  .progress {{ position:absolute; left:0; bottom:0; width:{W}px; height:6px; background:rgba(255,255,255,.08); }}
  .bar {{ width:{W}px; height:6px; background:#00CAD7; transform-origin:left center; }}
  .card {{ position:absolute; left:160px; top:0; width:1600px; height:{H}px; display:flex; flex-direction:column; justify-content:center; }}
  .k {{ font-size:24px; font-weight:800; letter-spacing:4px; color:#00CAD7; }}
  h1 {{ margin:22px 0 0; font-size:88px; font-weight:800; line-height:1.08; letter-spacing:-1.5px; max-width:1400px; }}
  #title h1 {{ max-width:960px; }}  #title .card p {{ max-width:900px; }}
  .devs {{ position:absolute; left:1120px; top:0; width:800px; height:{H}px; }}
  .devs .b1 {{ position:absolute; left:0; top:262px; width:400px; height:476px; border-radius:86px; background:linear-gradient(150deg,#3A3F46 0%,#171A1F 55%,#2A2E34 100%); box-shadow:0 34px 80px rgba(0,0,0,.65); }}
  .devs .c1 {{ position:absolute; left:400px; top:426px; width:14px; height:68px; border-radius:7px; background:linear-gradient(180deg,#4A4F57,#23262B); }}
  .devs .w1 {{ position:absolute; left:30px; top:294px; width:340px; height:412px; border-radius:68px; overflow:hidden; background:#000; }}
  .devs .b2 {{ position:absolute; left:400px; top:500px; width:300px; height:300px; border-radius:50%; background:linear-gradient(150deg,#3A3F46 0%,#171A1F 55%,#2A2E34 100%); box-shadow:0 28px 70px rgba(0,0,0,.65); }}
  .devs .w2 {{ position:absolute; left:417px; top:517px; width:266px; height:266px; border-radius:50%; overflow:hidden; background:#000; }}
  .devs img {{ position:absolute; left:0; top:0; width:100%; height:100%; opacity:0; }}
  .devs img[id$="-0"] {{ opacity:1; }}
  .card p {{ margin:28px 0 0; font-size:34px; line-height:1.45; color:#93AEC7; max-width:1200px; }}
  .inner {{ position:absolute; inset:0; }}
  .left {{ position:absolute; left:80px; top:112px; width:1560px; }}
  .steptitle {{ font-size:50px; font-weight:800; line-height:1.15; letter-spacing:-.5px; white-space:nowrap; }}
  .stepmeta {{ position:absolute; right:80px; top:128px; font-size:24px; color:#93AEC7; }}
  .shot {{ position:absolute; border-radius:22px; overflow:hidden;
    box-shadow:0 30px 80px rgba(0,0,0,.55); border:1px solid rgba(255,255,255,.10); background:#0B1622; }}
  .shot img, .shot video {{ width:100%; height:100%; object-fit:contain; display:block; }}
  .shot.list {{ background:rgba(4,14,30,.72); padding:44px 64px; box-sizing:border-box; }}
  .shot.list ul {{ margin:0; padding:0; list-style:none; }}
  .shot.list li {{ position:relative; padding:0 0 0 44px; margin:0 0 26px; font-size:36px; line-height:1.4; color:#E4F0EE; }}
  .shot.list li::before {{ content:""; position:absolute; left:0; top:16px; width:18px; height:18px; border-radius:50%; background:#00CAD7; }}
  .shot.gallery {{ background:rgba(4,14,30,.72); padding:26px 30px; box-sizing:border-box; display:grid;
    grid-template-columns:repeat(4,1fr); grid-template-rows:repeat(2,1fr); gap:14px 18px; }}
  .shot.gallery figure {{ margin:0; text-align:center; display:flex; flex-direction:column; align-items:center; justify-content:flex-start; }}
  .shot.gallery img {{ width:auto; height:200px; object-fit:contain; border-radius:16px; display:block; }}
  .shot.gallery b {{ display:block; margin-top:10px; font-size:22px; font-weight:800; color:#E4F0EE; }}
  .shot.gallery span {{ display:block; margin-top:2px; font-size:16px; color:#93AEC7; }}
  .shot.code {{ background:#0B1622; padding:34px 40px; box-sizing:border-box; display:flex; align-items:center; }}
  .shot.code pre {{ margin:0; font-family:Menlo,Consolas,monospace; font-size:24px; line-height:1.5; color:#D6E4F0; white-space:pre; }}
  .shot.code.small pre {{ font-size:19px; line-height:1.45; }}
  .shot.list.dense {{ padding:44px 56px; }}
  .shot.list.dense li {{ font-size:30px; line-height:1.35; margin-bottom:22px; padding-left:38px; }}
  .shot.list.dense li::before {{ top:12px; width:15px; height:15px; }}
  .caps {{ position:absolute; left:80px; right:330px; bottom:58px; height:150px; }}
  .cap.long {{ font-size:33px; line-height:1.35; padding:18px 30px; }}
  .cap.xlong {{ font-size:29px; }}
  .ilogo {{ position:absolute; left:460px; top:330px; width:1000px; }}
  .iseries {{ position:absolute; left:0; right:0; top:690px; text-align:center; }}
  .iseries b {{ display:block; font-size:30px; font-weight:800; letter-spacing:6px; color:#00CAD7; }}
  .iseries span {{ display:block; margin-top:14px; font-size:26px; color:#93AEC7; }}
  .oh {{ position:absolute; left:0; right:0; top:110px; text-align:center; font-size:44px; font-weight:800; }}
  .obox {{ position:absolute; top:225px; width:740px; height:416px; border-radius:22px; box-sizing:border-box; padding:28px 34px;
    border:2px dashed rgba(0,202,215,.45); background:rgba(4,14,30,.55); display:flex; flex-direction:column; justify-content:flex-end; }}
  .obox span {{ font-size:22px; font-weight:800; letter-spacing:3px; text-transform:uppercase; color:#00CAD7; }}
  .obox b {{ margin-top:10px; font-size:38px; font-weight:800; color:#E4F0EE; }}
  .osub {{ position:absolute; left:0; right:0; top:700px; display:flex; justify-content:center; align-items:center; gap:28px; }}
  .ocircle {{ width:170px; height:170px; border-radius:50%; border:2px dashed rgba(0,202,215,.45); background:rgba(4,14,30,.55); }}
  .osub b {{ display:block; font-size:36px; font-weight:800; }}
  .osub span {{ display:block; margin-top:8px; font-size:24px; color:#93AEC7; }}
  .ologo {{ position:absolute; left:80px; bottom:52px; width:300px; }}
  .olinks {{ position:absolute; right:80px; bottom:60px; font-size:26px; color:#93AEC7; }}
  .avatar {{ position:absolute; right:60px; bottom:34px; width:220px; height:220px; border-radius:50%; overflow:hidden;
    border:4px solid #00CAD7; box-shadow:0 20px 50px rgba(0,0,0,.55); background:#0B1622; }}
  .avatar img {{ width:100%; height:100%; display:block; }}
  .cap {{ position:absolute; left:0; right:0; bottom:0; margin:0 auto; max-width:1400px; padding:22px 34px; border-radius:18px;
    background:rgba(4,14,30,.82); border:1px solid rgba(255,255,255,.08); font-size:38px; line-height:1.4; text-align:center; opacity:0; }}
</style>
</head>
<body>
<div id="root" data-composition-id="main" data-start="0" data-width="{W}" data-height="{H}" data-duration="{total}" data-fps="{FPS}">
{chr(10).join(clips)}
</div>
<script>
  window.__timelines = window.__timelines || {{}};
  const tl = gsap.timeline({{ paused: true }});
  {chr(10).join("  " + x for x in tl)}
  window.__timelines["main"] = tl;
</script>
</body>
</html>'''
    open(os.path.join(proj, "index.html"), "w", encoding="utf-8").write(page)
    print(f"{proj}/index.html  →  {n} sahne, toplam {total} sn")
    for i, (start, dur, s, _) in enumerate(plan, 1):
        print(f"  {i:2d}. {start:6.1f}s  {dur:5.1f}s  {s['title']}")

if __name__ == "__main__":
    build(sys.argv[1] if len(sys.argv) > 1 else "imager")
