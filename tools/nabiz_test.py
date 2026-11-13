#!/usr/bin/env python3
"""
nabiz_test.py: ölçüm algoritmasının (alg A3) sınaması. Karta yüklemeden çalışır. numpy ve bir C derleyicisi (cc) gerekir.

Ne yapar:
  1) Kart algoritmasını (src/services/nabiz_alg.c) bilgisayarda derler (tools/nabiz_host.c ile, -Wall -Wextra -Werror).
  2) Cevabı bilinen yapay sinyaller üretir (tools/nabiz_sentetik.py): kedi/köpek/insan nabzı, düzensiz ritim, mırlama,
     soluma, solunum, gürültü, sessizlik, doygunluk.
  3) Her sinyali hem bağımsız Python referansıyla (tools/nabiz_ref.py) hem derlenen kart koduyla çözer:
       BEKLENEN  : sonuç bilinen cevapla tutuyor mu
       UYUM      : kart kodu ile referans aynı sonucu veriyor mu
  4) Yanlış kabul taraması: içinde kalp OLMAYAN yapay kayıtlar (beyaz gürültü, sürtünme, rastgele tıkırtı). A3'ün düşük
     güvenli kuralları ("pencere", "tutarli") bunların hiçbirinde sonuç vermemeli. Eski kuralların (ac / vurus) bu
     kayıtlarda verdiği sonuçlar sayılır ve bilgi olarak yazılır (A2'den beri bilinen zayıflık; sınamayı düşürmez).
  5) Varsa gerçek kayıtlarda (~/Desktop/NAR-kayitlar/olcum/*.wav ve komut satırında verilenler) yalnız UYUM'a bakar.

Kullanım:  python3 tools/nabiz_test.py [ek.wav[:profil] ...]        çıkış kodu 0 = hepsi geçti
Algoritmada (nabiz_alg.c ya da nabiz_ref.py) her değişiklikten sonra çalıştırın.
Yapay sinyaller gerçek hayvan kaydının yerini tutmaz; bu test mantığı ve iki uygulamanın eşliğini doğrular.
"""
import glob, os, subprocess, sys, tempfile

BURASI = os.path.dirname(os.path.abspath(__file__)); KOK = os.path.dirname(BURASI)
sys.path.insert(0, BURASI)
import nabiz_ref as nr          # noqa: E402
import nabiz_sentetik as ns     # noqa: E402

KEDI = dict(f1=90, f2=120, d1=0.05, d2=0.035)
# (ad, profil, üretim argümanları, beklenen)   beklenen: durum, nabiz=(değer, tolerans), yontem, solunum=(değer, tol), sdurum, mirlama, soluma
SENARYO = [
    ("kedi 180 düzenli",              "kedi",  dict(kalp_dk=180, **KEDI),                               dict(durum="OK", nabiz=(180, 2))),
    ("kedi 200 + nefes 30",           "kedi",  dict(kalp_dk=200, nefes_dk=30, **KEDI),                  dict(durum="OK", nabiz=(200, 2), sdurum="OK", solunum=(30, 1))),
    ("kedi 220 gürültülü",            "kedi",  dict(kalp_dk=220, gurultu_db=-38, **KEDI),               dict(durum="OK", nabiz=(220, 2))),
    ("kedi 140, S2 zayıf",            "kedi",  dict(kalp_dk=140, a2=0.25, **KEDI),                      dict(durum="OK", nabiz=(140, 2))),
    ("kedi 240 stresli",              "kedi",  dict(kalp_dk=240, f1=90, f2=120, d1=0.045, d2=0.03),     dict(durum="OK", nabiz=(240, 3))),
    ("kedi 160 + nefes 22 (uyku)",    "kedi",  dict(kalp_dk=160, nefes_dk=22, **KEDI),                  dict(durum="OK", nabiz=(160, 2), sdurum="OK", solunum=(22, 1))),
    ("kedi 200, ardışık vuruş farklı","kedi",  dict(kalp_dk=200, alternans=0.6, **KEDI),                dict(durum="OK", nabiz=(200, 2))),      # yarı hız (100) tuzağı
    ("kedi 190 + mırlama 25 Hz",      "kedi",  dict(kalp_dk=190, mirlama_hz=25, **KEDI),                dict(durum="MIRLAMA", mirlama=1, sdurum="MIRLAMA")),
    ("kedi 190 + zayıf mırlama 22 Hz","kedi",  dict(kalp_dk=190, mirlama_hz=22, mirlama_db=-34, **KEDI), dict(durum="MIRLAMA", mirlama=1)),
    ("kedi 200 + 50 Hz şebeke",       "kedi",  dict(kalp_dk=200, sebeke_db=-36, **KEDI),                dict(durum="OK", nabiz=(200, 2), mirlama=0)),
    ("köpek 90, düzensiz (RSA 0,25)", "kopek", dict(kalp_dk=90, rsa=0.25, nefes=20, nefes_dk=20),       dict(durum="OK", yontem="vurus", nabiz=("gercek", 0.06), sdurum="OK", solunum=(20, 1), soluma=0)),
    ("köpek 70, düzensiz (RSA 0,35)", "kopek", dict(kalp_dk=70, rsa=0.35, nefes=16, nefes_dk=16),       dict(durum="OK", yontem="vurus", nabiz=("gercek", 0.08), sdurum="OK", solunum=(16, 1))),
    ("köpek 90 düzensiz + gürültü",   "kopek", dict(kalp_dk=90, rsa=0.25, nefes=20, nefes_dk=20, gurultu_db=-38), dict(durum="OK", yontem="vurus", nabiz=("gercek", 0.06))),
    ("köpek 120 + nefes 28",          "kopek", dict(kalp_dk=120, nefes_dk=28),                          dict(durum="OK", nabiz=(120, 2), sdurum="OK", solunum=(28, 1), soluma=0)),
    ("köpek 160 küçük ırk",           "kopek", dict(kalp_dk=160, f1=70, f2=95, d1=0.06, d2=0.04),       dict(durum="OK", nabiz=(160, 2), soluma=0)),
    ("köpek 110 + soluma 300/dk",     "kopek", dict(kalp_dk=110, soluma_dk=300),                        dict(durum="OK", nabiz=(110, 2), soluma=1, sdurum="SOLUMA")),
    ("köpek 60 + nefes 24",           "kopek", dict(kalp_dk=60, nefes_dk=24),                           dict(durum="OK", nabiz=(60, 2), sdurum="OK", solunum=(24, 1), soluma=0)),
    ("köpek 45 (dev ırk) + nefes 12", "kopek", dict(kalp_dk=45, nefes_dk=12),                           dict(durum="OK", nabiz=(45, 2), sdurum="OK", solunum=(12, 1))),
    ("insan 72 + nefes 14",           "insan", dict(kalp_dk=72, nefes_dk=14),                           dict(durum="OK", nabiz=(72, 2), sdurum="OK", solunum=(14, 1))),
    # A3: düşük güvenli kabul yolları. Kalp sesi gürültüye göre zayıf (vuruş sayımı güvenilmez), ritim var.
    ("insan 72→61 yavaşlıyor, zayıf",  "insan", dict(kalp_dk=72, kayma=-0.15, kalp_db=-47, gurultu_db=-33), dict(durum="OK", yontem="pencere", nabiz=("gercek", 0.03))),
    ("köpek 100→82 yavaşlıyor, zayıf", "kopek", dict(kalp_dk=100, kayma=-0.18, kalp_db=-47, gurultu_db=-33), dict(durum="OK", yontem="pencere", nabiz=("gercek", 0.03))),
    ("kedi 190→156 yavaşlıyor, zayıf", "kedi",  dict(kalp_dk=190, kayma=-0.18, kalp_db=-50, gurultu_db=-33, **KEDI), dict(durum="OK", yontem="pencere", nabiz=("gercek", 0.03))),
    ("insan 70 hışırtılı, titrek",     "insan", dict(kalp_dk=70, seg=0.07, kalp_db=-50, gurultu_db=-33),   dict(durum="OK", yontem="tutarli", nabiz=("gercek", 0.03))),
    ("köpek 100 hışırtılı, titrek",    "kopek", dict(kalp_dk=100, seg=0.05, kalp_db=-53, gurultu_db=-33),  dict(durum="OK", yontem="tutarli", nabiz=("gercek", 0.03))),
    ("kedi 190 hışırtılı, titrek",     "kedi",  dict(kalp_dk=190, seg=0.05, kalp_db=-53, gurultu_db=-33, **KEDI), dict(durum="OK", yontem="tutarli", nabiz=("gercek", 0.03))),
    ("insan yavaşlıyor + 3 sn gürültü","insan", dict(kalp_dk=72, kayma=-0.18, kalp_db=-47, gurultu_db=-33, patlama=(13.0, 3.0, -22.0)), dict(durum="BELIRSIZ")),   # net pencereler çelişiyor (69 ve 139): sayı verilmez
    ("yalnız nefes 40, hışırtılı",     "insan", dict(nefes_dk=40, nefes_db=-45, gurultu_db=-49, tohum=3),   dict(durum="BELIRSIZ")),   # nefes çapraz kontrolü olmasa "tutarli" 40/dk derdi
    ("yalnız nefes 40, hışırtılı",     "kopek", dict(nefes_dk=40, nefes_db=-45, gurultu_db=-49, tohum=3),   dict(durum="BELIRSIZ")),
    ("yalnız gürültü",                "kedi",  dict(gurultu_db=-40),                                    dict(durum="BELIRSIZ", mirlama=0)),
    ("yalnız gürültü",                "kopek", dict(gurultu_db=-40),                                    dict(durum="BELIRSIZ", soluma=0)),
    ("yalnız nefes 24 (kalp yok)",    "kopek", dict(nefes_dk=24),                                       dict(durum="BELIRSIZ", sdurum="OK", solunum=(24, 1), soluma=0)),
    ("sessizlik (mikrofon ölü)",      "kedi",  dict(sessiz=True),                                       dict(durum="SINYAL_YOK")),
    ("doygun kayıt",                  "kopek", dict(kalp_dk=100, kalp_db=14.0),                         dict(durum="DOYGUN")),
    ("çok kısa kayıt (3 sn)",         "insan", dict(kalp_dk=72, sure=3.0),                              dict(durum="KISA")),
]


def derle(hedef):
    cmd = ["cc", "-O2", "-std=c99", "-Wall", "-Wextra", "-Werror", "-I", os.path.join(KOK, "src", "services"),
           os.path.join(BURASI, "nabiz_host.c"), os.path.join(KOK, "src", "services", "nabiz_alg.c"), "-lm", "-o", hedef]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0: sys.exit("kart kodu bilgisayarda DERLENEMEDİ:\n" + r.stderr)


def kart_calistir(host, wav, profil):
    r = subprocess.run([host, wav, profil], capture_output=True, text=True)
    if r.returncode != 0: return None, r.stderr.strip()
    return dict(t.split("=", 1) for t in r.stdout.splitlines()[0].split() if "=" in t), ""


def esit(ad, a, b, tol):
    try: return abs(float(a) - float(b)) <= tol
    except (TypeError, ValueError): return str(a) == str(b)


def uyum(py, c):
    """Kart kodu ile referans aynı mı? Dönüş: fark listesi (boş = uyumlu)."""
    fark = []
    for k in ("durum", "yontem", "sdurum"):
        if str(py[k]) != c.get(k): fark.append(f"{k}: ref {py[k]} / kart {c.get(k)}")
    for k, tol in (("nabiz", 1.0), ("nabiz_ac", 1.0), ("nabiz_vurus", 1.0), ("r", 0.02), ("r2", 0.03), ("nvurus", 1), ("yayilim", 0.03),
                   ("belirgin", 0.3), ("vguven", 0), ("solunum", 1.0), ("rs", 0.03), ("mirlama", 0), ("soluma", 0),
                   ("doygun", 0), ("tepe", 0.1), ("seviye", 0.3), ("sseviye", 0.3), ("sure", 0.05),
                   ("pen_n", 0), ("pen_iyi", 0), ("pen_uyan", 0), ("r2t", 0.03)):
        if k not in c or not esit(k, py[k], c[k], tol + 1e-9): fark.append(f"{k}: ref {py[k]:.3f} / kart {c.get(k)}")
    return fark


def beklenen_mi(s, b, gercek):
    hata = []
    for k in ("durum", "yontem", "sdurum", "mirlama", "soluma"):
        if k in b and str(s[k]) != str(b[k]): hata.append(f"{k}={s[k]} (beklenen {b[k]})")
    if "nabiz" in b:
        d, tol = b["nabiz"]
        if d == "gercek": d = gercek; tol = tol * gercek
        if abs(s["nabiz"] - d) > tol: hata.append(f"nabiz={s['nabiz']:.1f} (beklenen {d:.1f} ±{tol:.1f})")
    if "solunum" in b and abs(s["solunum"] - b["solunum"][0]) > b["solunum"][1]:
        hata.append(f"solunum={s['solunum']:.1f} (beklenen {b['solunum'][0]} ±{b['solunum'][1]})")
    return hata


def main():
    try: import numpy as np
    except ImportError: sys.exit("numpy gerekli: python3 -m pip install --user --break-system-packages numpy")
    gecici = tempfile.mkdtemp(prefix="nabiz_test_"); host = os.path.join(gecici, "nabiz_host")
    derle(host)
    print(f"kart kodu bilgisayarda derlendi (alg {nr.ALG}, katı uyarılar hatasız)\n")
    print(f"{'senaryo':34s} {'profil':6s} {'sonuç':38s} beklenen  kart=ref")
    kaldi = 0
    for i, (ad, profil, arg, bekl) in enumerate(SENARYO):
        arg = dict(arg); sure = arg.pop("sure", 30.0)
        if arg.pop("sessiz", False): v = np.zeros(int(sure * ns.FS), dtype=np.int32)
        else: v = ns.karistir(sure=sure, **arg)
        gercek = 0.0
        if arg.get("kalp_dk"):
            ka = {k: arg[k] for k in ("f1", "f2", "d1", "d2", "a2", "rsa", "nefes", "seg", "alternans", "kayma") if k in arg}
            gercek = ns.gercek_nabiz(ns.kalp(arg["kalp_dk"], sure, tohum=arg.get("tohum", 1), **ka)[1])
        wav = os.path.join(gecici, f"s{i:02d}.wav"); ns.wav_yaz(wav, v)
        py = nr.analiz(v, 24, profil)
        c, err = kart_calistir(host, wav, profil)
        h1 = beklenen_mi(py, bekl, gercek)
        h2 = uyum(py, c) if c else ["kart kodu çalışmadı: " + err]
        if c: h1c = beklenen_mi({**py, **{k: (float(c[k]) if k in ("nabiz", "solunum") else (int(c[k]) if k in ("mirlama", "soluma") else c[k])) for k in ("durum", "yontem", "sdurum", "mirlama", "soluma", "nabiz", "solunum")}}, bekl, gercek)
        else: h1c = []
        ozet = f"{py['durum']} {py['nabiz']:.0f}/dk r={py['r']:.2f} {py['yontem']}"
        if py["sdurum"] not in ("KISA",): ozet += f" | sol {py['solunum']:.0f} {py['sdurum']}"
        print(f"{ad:34s} {profil:6s} {ozet:38s} {'GEÇTİ ' if not (h1 or h1c) else 'KALDI '}   {'GEÇTİ' if not h2 else 'KALDI'}")
        for h in h1: print(f"      beklenen (ref): {h}")
        for h in h1c: print(f"      beklenen (kart): {h}")
        for h in h2: print(f"      uyum: {h}")
        kaldi += bool(h1 or h1c) + bool(h2)

    # yanlış kabul taraması: kalp yok
    print("\nyanlış kabul taraması (kalp yok; düşük güvenli kurallar sonuç vermemeli)")
    TARAMA = [("beyaz gürültü", lambda t: dict(gurultu_db=-45.0 + 4.0 * t)), ("sürtünme", lambda t: dict(surtunme_adet=15 + 12 * t, gurultu_db=-50.0)),
              ("rastgele tıkırtı", lambda t: dict(tikirti_hiz=0.8 + 0.5 * t, gurultu_db=-50.0))]
    eski_kabul = 0
    for ad, uret in TARAMA:
        yeni = 0; fark = 0; adet = 0
        for t in range(5):
            v = ns.karistir(sure=30.0, tohum=10 + t, **uret(t)); wav = os.path.join(gecici, "tarama.wav"); ns.wav_yaz(wav, v)
            for profil in nr.PROFIL:
                py = nr.analiz(v, 24, profil); c, err = kart_calistir(host, wav, profil); adet += 1
                h2 = uyum(py, c) if c else ["kart kodu çalışmadı: " + err]
                for h in h2: print(f"      uyum ({ad}, tohum {10 + t}, {profil}): {h}")
                fark += bool(h2)
                for kim, d, y in (("ref", py["durum"], py["yontem"]), ("kart", (c or {}).get("durum"), (c or {}).get("yontem"))):
                    if d == "OK" and y in ("pencere", "tutarli"): yeni += 1; print(f"      YANLIŞ KABUL ({kim}; {ad}, tohum {10 + t}, {profil}): {y}")
                if py["durum"] == "OK" and py["yontem"] in ("ac", "vurus"): eski_kabul += 1
        print(f"{ad:34s} {adet:3d} kayıt   düşük güvenli yanlış kabul {yeni}   {'GEÇTİ ' if not yeni else 'KALDI '}   {'GEÇTİ' if not fark else 'KALDI'}")
        kaldi += bool(yeni) + bool(fark)
    if eski_kabul: print(f"   bilgi: eski kurallar (ac / vurus) bu kalpsiz kayıtların {eski_kabul} tanesinde sonuç verdi (A2'den beri var)")

    gercekler = []
    for a in sys.argv[1:]:
        yol, _, pr = a.partition(":"); gercekler.append((yol, pr or "insan"))
    for yol in sorted(glob.glob(os.path.expanduser("~/Desktop/NAR-kayitlar/olcum/*.wav"))):
        if yol.endswith("_dinleme.wav"): continue
        pr = next((p for p in nr.PROFIL if f"_{p}_" in os.path.basename(yol)), "insan"); gercekler.append((yol, pr))
    if gercekler: print(f"\ngerçek kayıtlar (yalnız kart = referans uyumu; her kayıt üç profille)")
    for yol, pr in gercekler:
        try: v, bit = nr.wav_oku(yol)
        except Exception as e: print(f"{os.path.basename(yol)[:44]:46s} okunamadı: {e}"); continue
        for p in [pr] + [q for q in nr.PROFIL if q != pr]:
            py = nr.analiz(v, bit, p); c, err = kart_calistir(host, yol, p)
            h2 = uyum(py, c) if c else ["kart kodu çalışmadı: " + err]
            print(f"{os.path.basename(yol)[:44]:46s} {p:6s} {py['durum']:10s} {py['nabiz']:5.0f}/dk r={py['r']:.2f}   {'GEÇTİ' if not h2 else 'KALDI'}")
            for h in h2: print(f"      uyum: {h}")
            kaldi += bool(h2)
    print(f"\n{'HEPSİ GEÇTİ' if not kaldi else str(kaldi) + ' SINAMA KALDI'}")
    sys.exit(1 if kaldi else 0)


if __name__ == "__main__":
    main()
