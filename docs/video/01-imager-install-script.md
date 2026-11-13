# Video senaryosu 1 — Corezzle Imager ile NarRTOS Yükleme

Kaynak doküman: `panel-content/17-corezzle-imager-install.md` (panel: NarRTOS Wear - Corezzle Imager ile Yükleme)
Hedef süre: ~90 saniye · Avatar: HeyGen, Türkçe · Ekran: `wiki-html/assets/img/imager-0N-*.png`
Dil: "biz" dili, kısa cümleler. Ekranda yazan her İngilizce düğme adı aynen söylenir (NEXT, FLASH!).

Kullanım: her sahne HeyGen'de ayrı bir sahne/klip. "Ekranda" sütunu arka plan görselini,
"Avatar" sütunu okunacak metni verir. Süreler ~150 kelime/dakika hızına göre.

---

## Sahne 1 — Giriş · ~10 s
**Ekranda:** kart fotoğrafı ya da `imager-01-welcome.png` · **Yazı:** "NarRTOS Wear · Corezzle Imager ile Yükleme"

> Bu videoda NarRTOS'u karta, hiç kod derlemeden, Corezzle Imager ile yükleyeceğiz. Bir bilgisayar, kart ve veri aktaran bir USB kablosu yeter. On dakikadan kısa sürer.

## Sahne 2 — Kurulum · ~14 s
**Ekranda:** `imager-01-welcome.png` · **Yazı:** "1 · Corezzle Imager'ı kurun"

> Önce Corezzle Imager'ı indirip kuruyoruz. Windows'ta kurulum dosyasını çalıştırıyoruz; "Windows protected your PC" uyarısı çıkarsa More info, sonra Run anyway diyoruz. Mac'te uygulamayı Applications klasörüne sürüklüyoruz. Açılınca bu ekranı görüyoruz.

## Sahne 3 — Kablo · ~10 s
**Ekranda:** kart + kablo fotoğrafı (yoksa `imager-01-welcome.png` kalır) · **Yazı:** "2 · Kartı takın"

> Kartı USB kablosuyla bilgisayara takıyoruz. Kartın ışığı yanmalı. Önemli nokta: kablo veri taşıyan bir kablo olmalı. Yalnız şarj kablolarıyla kart hiç görünmez.

## Sahne 4 — Device · ~8 s
**Ekranda:** `imager-02-device.png` · **Yazı:** "3 · Device"

> NEXT diyoruz. Kart listesinde "NarRTOS Wear, ESP32-S3" kartını seçiyoruz ve NEXT.

## Sahne 5 — OS · ~14 s
**Ekranda:** `imager-03-os.png` · **Yazı:** "4 · OS"

> Şimdi kartımıza uygun sürümü seçiyoruz. Ekranı dikdörtgense 410'a 502 AMOLED, yuvarlaksa 240'a 240 round LCD. Biz yuvarlak kartı seçiyoruz. Yanlış seçersek zarar olmaz; ekran karanlık kalır, doğrusuyla bir kez daha yazarız. NEXT.

## Sahne 6 — USB Port · ~16 s
**Ekranda:** `imager-04-usb-port.png` · **Yazı:** "5 · USB Port"

> Listede kartımız "USB Single Serial" adıyla görünüyor. Ona tıklıyoruz. Görünmüyorsa önce kabloyu değiştirip Refresh List diyoruz. Alttaki kutuyu işaretliyoruz; kartın belleği tamamen silinip yeniden yazılacak, ilk kurulumda olması gereken bu. Çıkan soruya OK. Sonra FLASH!, tekrar OK.

## Sahne 7 — Writing · ~12 s
**Ekranda:** `imager-05-writing.png` · **Yazı:** "6 · Writing"

> Yazma başladı. Önce dosyalar iniyor, sonra karta bağlanıyor, siliyor ve yazıyor. İlk yarım dakika yüzde sıfırda durur, bu normal. Bir iki dakika sürer; bu sırada kabloyu çekmiyoruz.

## Sahne 8 — Done · ~10 s
**Ekranda:** `imager-06-done.png` → kartta saat kadranı (fotoğraf/video) · **Yazı:** "7 · Bitti"

> Done ekranı geldi. Kart kendini yeniden başlatıyor; birkaç saniye sonra saat kadranı ekranda. Ekranda sağa kaydırırsak uygulama menüsü açılıyor.

## Sahne 9 — Kapanış · ~8 s
**Ekranda:** `imager-06-done.png` ya da kart · **Yazı:** "NarRTOS Wear dökümanı"

> Hepsi bu. Bundan sonraki güncellemeleri cihaz kendisi indirir. Takıldığınız bir yer olursa dokümandaki "en sık takılınan yerler" tablosuna bakın.

---

## Notlar (HeyGen'e girmeden önce)

- Avatar sayıları Türkçe okumalı: "410'a 502", "240'a 240" yazımı bunun için. Yanlış okursa "dört yüz on'a beş yüz iki" diye açık yaz.
- İngilizce düğme adları (NEXT, FLASH!, Refresh List, USB Single Serial, More info, Run anyway) İngilizce kalır; avatar Türkçe sesle İngilizce okur, bu istenen davranış.
- Sahne 3 ve 8 için kart fotoğrafı/kısa video gerekiyor (kablo takılı ışık yanan kart; kadran açılan kart). Yoksa welcome/done görüntüleri kalır.
- Toplam metin ~230 kelime → ~95 saniye.
