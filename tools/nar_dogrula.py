#!/usr/bin/env python3
"""
nar_dogrula.py: toplanan etiketli kayıtlar üzerinde algoritmanın DOĞRULUK tablosu. numpy gerekir.

tools/nar_olcum.py'nin yazdığı olcumler.csv'yi okur; her satırın ham WAV kaydını GÜNCEL algoritmayla (tools/nabiz_ref.py;
kart koduyla eşliği tools/nabiz_test.py güvence altına alır) yeniden çözer ve elle sayılan referans değerlerle
karşılaştırır. Algoritma ya da eşik değiştiğinde yeniden çalıştırın: sonuç iyileşti mi kötüleşti mi buradan görülür.

Kullanım:
    python3 tools/nar_dogrula.py                         ~/Desktop/NAR-kayitlar/olcum/olcumler.csv
    python3 tools/nar_dogrula.py <klasör ya da csv>

Tanımlar:
    sonuç verdi : algoritma durum=OK dedi (sayı gösterdi)
    doğru       : sonuç verdi ve referansla farkı tolerans içinde (nabız ±5/dk ya da ±%5; solunum ±3/dk ya da ±%10)
    YANLIŞ      : sonuç verdi ama tolerans dışında  ← en tehlikeli durum; hedef SIFIR
    vermedi     : BELIRSIZ / MIRLAMA / KISA vb. (yanlış sayı vermektense susmak kabul edilir; oranı "verim"i düşürür)
Çıktı: ekrana tablo + aynı klasöre dogrulama.csv (satır bazında).
"""
import csv, os, sys

BURASI = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, BURASI)


def tolerans(ref, mutlak, oran): return max(mutlak, oran * ref)


def sayi(x):
    try: return float(str(x).replace(",", "."))
    except (TypeError, ValueError): return None


def ozet(baslik, satirlar, anahtar, ref_anahtar, durum_anahtar, mutlak, oran):
    r = [s for s in satirlar if s[ref_anahtar] is not None]
    if not r: print(f"  {baslik}: referanslı kayıt yok"); return
    verdi = [s for s in r if s[durum_anahtar] == "OK"]
    dogru = [s for s in verdi if abs(s[anahtar] - s[ref_anahtar]) <= tolerans(s[ref_anahtar], mutlak, oran)]
    yanlis = [s for s in verdi if s not in dogru]
    mh = sum(abs(s[anahtar] - s[ref_anahtar]) for s in verdi) / len(verdi) if verdi else 0.0
    print(f"  {baslik}: referanslı {len(r)} | sonuç verdi {len(verdi)} (verim %{100 * len(verdi) / len(r):.0f}) | doğru {len(dogru)}"
          + (f" (%{100 * len(dogru) / len(verdi):.0f})" if verdi else "") + f" | YANLIŞ {len(yanlis)} | ortalama mutlak hata {mh:.1f}/dk")
    for s in yanlis:
        print(f"      YANLIŞ: {s['zaman']} {s['hayvan']} ({s['yer']}, {s['durum_hayvan']}): {s[anahtar]:.0f} (referans {s[ref_anahtar]:.0f})  {s['ham_wav']}")
    neden = {}
    for s in r:
        if s[durum_anahtar] != "OK": neden[s[durum_anahtar]] = neden.get(s[durum_anahtar], 0) + 1
    if neden: print("      sonuç vermeme nedenleri: " + ", ".join(f"{k} {v}" for k, v in sorted(neden.items(), key=lambda kv: -kv[1])))


def main():
    try: import numpy  # noqa: F401
    except ImportError: sys.exit("numpy gerekli: python3 -m pip install --user --break-system-packages numpy")
    import nabiz_ref as nr
    hedef = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser("~/Desktop/NAR-kayitlar/olcum")
    yol = os.path.join(hedef, "olcumler.csv") if os.path.isdir(hedef) else hedef
    if not os.path.exists(yol): sys.exit(f"bulunamadı: {yol}  (önce tools/nar_olcum.py ile ölçüm alın)")
    dizin = os.path.dirname(os.path.abspath(yol))
    with open(yol, newline="", encoding="utf-8-sig") as f: girdi = list(csv.DictReader(f))
    print(f"{yol}: {len(girdi)} ölçüm, algoritma {nr.ALG}\n")
    print(f"{'zaman':19s} {'tür':5s} {'hayvan':10s} {'yer':9s} {'durum':8s} | {'nabız':>22s} {'ref':>4s} {'fark':>5s} | {'solunum':>18s} {'ref':>4s} {'fark':>5s} | not")
    sonuc = []
    for g in girdi:
        tur = g.get("tur", "insan") if g.get("tur") in nr.PROFIL else "insan"
        wav = os.path.join(dizin, g.get("ham_wav", "") or "_yok_")
        s = dict(zaman=g.get("zaman", ""), tur=tur, hayvan=g.get("hayvan", ""), yer=g.get("yer", ""), durum_hayvan=g.get("durum_hayvan", ""),
                 ham_wav=g.get("ham_wav", ""), ref_nabiz=sayi(g.get("ref_nabiz")), ref_solunum=sayi(g.get("ref_solunum")),
                 kart_alg=g.get("alg", ""), kart_durum=g.get("cihaz_durum", ""), kart_nabiz=g.get("cihaz_nabiz", ""))
        if not os.path.exists(wav):
            print(f"{s['zaman']:19s} {tur:5s} {s['hayvan'][:10]:10s} ham kayıt yok ({g.get('ham_wav', '')})"); continue
        try: v, bit = nr.wav_oku(wav)
        except Exception as e: print(f"{s['zaman']:19s} okunamadı: {e}"); continue
        a = nr.analiz(v, bit, tur)
        s.update(durum=a["durum"], nabiz=a["nabiz"], r=a["r"], yontem=a["yontem"], sdurum=a["sdurum"], solunum=a["solunum"], rs=a["rs"],
                 mirlama=a["mirlama"], soluma=a["soluma"], belirgin=a["belirgin"], seviye=a["seviye"], alg=a["alg"])
        nf = f"{a['nabiz'] - s['ref_nabiz']:+.0f}" if (s["ref_nabiz"] is not None and a["durum"] == "OK") else ""
        sf = f"{a['solunum'] - s['ref_solunum']:+.0f}" if (s["ref_solunum"] is not None and a["sdurum"] == "OK") else ""
        nt = f"{a['nabiz']:.0f} {a['durum']} r={a['r']:.2f} {a['yontem']}" if a["durum"] == "OK" else f"{a['durum']} (aday {a['nabiz_ac']:.0f})"
        stx = f"{a['solunum']:.0f} OK r={a['rs']:.2f}" if a["sdurum"] == "OK" else a["sdurum"]
        ek = []
        if a["mirlama"]: ek.append("mırlama algılandı")
        if a["soluma"]: ek.append("soluma algılandı")
        if s["durum_hayvan"] == "mirlama" and not a["mirlama"]: ek.append("ETİKET mırlama, algılanmadı")
        if s["durum_hayvan"] == "soluma" and not a["soluma"]: ek.append("ETİKET soluma, algılanmadı")
        if a["mirlama"] and s["durum_hayvan"] != "mirlama": ek.append("mırlama YANLIŞ ALARM?")
        if a["soluma"] and s["durum_hayvan"] != "soluma": ek.append("soluma YANLIŞ ALARM?")
        print(f"{s['zaman']:19s} {tur:5s} {s['hayvan'][:10]:10s} {s['yer'][:9]:9s} {s['durum_hayvan'][:8]:8s} | {nt:>22s} "
              f"{'' if s['ref_nabiz'] is None else format(s['ref_nabiz'], '.0f'):>4s} {nf:>5s} | {stx:>18s} "
              f"{'' if s['ref_solunum'] is None else format(s['ref_solunum'], '.0f'):>4s} {sf:>5s} | {'; '.join(ek)}")
        sonuc.append(s)

    if not sonuc: sys.exit("\nçözülecek kayıt yok")
    print("\n──────── özet ────────")
    for tur in ["hepsi"] + [t for t in nr.PROFIL if any(s["tur"] == t for s in sonuc)]:
        alt = sonuc if tur == "hepsi" else [s for s in sonuc if s["tur"] == tur]
        print(f"{tur} ({len(alt)} kayıt)")
        ozet("nabız  ", alt, "nabiz", "ref_nabiz", "durum", 5.0, 0.05)
        ozet("solunum", alt, "solunum", "ref_solunum", "sdurum", 3.0, 0.10)
    for etiket, alan in (("mirlama", "mirlama"), ("soluma", "soluma")):
        var = [s for s in sonuc if s["durum_hayvan"] == etiket]; yok = [s for s in sonuc if s["durum_hayvan"] != etiket]
        if var or any(s[alan] for s in sonuc):
            print(f"{etiket} tespiti: etiketli {len(var)} kayıttan {sum(1 for s in var if s[alan])} yakalandı; "
                  f"etiketsiz {len(yok)} kayıtta {sum(1 for s in yok if s[alan])} alarm")
    refsiz = sum(1 for s in sonuc if s["ref_nabiz"] is None)
    if refsiz: print(f"\nNOT: {refsiz} kaydın referans nabzı yok; doğruluk hesabına girmez (elle sayılan değeri girin).")
    cik = os.path.join(dizin, "dogrulama.csv"); alanlar = list(sonuc[0].keys())
    with open(cik, "w", newline="", encoding="utf-8-sig") as f:
        w = csv.DictWriter(f, fieldnames=alanlar); w.writeheader()
        for s in sonuc: w.writerow({k: ("" if s.get(k) is None else (f"{s[k]:.2f}" if isinstance(s[k], float) else s[k])) for k in alanlar})
    print(f"\nsatır bazında sonuç: {cik}")


if __name__ == "__main__":
    main()
