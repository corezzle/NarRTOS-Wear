# Tarayıcıdan yükleme (ESP Web Tools) — dağıtım notu

`index.html`, kartı **hiç program kurmadan** Chrome/Edge üzerinden programlayan sayfadır
(Web Serial + Espressif `esptool-js`). İmajları ve sürüm listesini `scripts/release.py`
üretir. Corezzle Imager'a ve panel koduna dokunmaz.

## Sunucu yerleşimi

Sayfa ve imajlar **aynı sunucuda, HTTPS'ten** sunulur (Web Serial yalnız HTTPS ve
localhost'ta çalışır). Önerilen yerleşim, OTA backend'inin statik dosya kökü:

```
<kök>/install/index.html                 ← docs/web-install/index.html
<kök>/releases/latest.json               ← release/latest.json
<kök>/releases/v1.4.81/web/<env>/…       ← release/v1.4.81/web/ (manifest + parçalar)
<kök>/releases/v1.4.81/*.bin, manifest.json, SHA256SUMS   (isteğe bağlı; esptool ile elle yazanlar için)
```

Sayfa `../releases/latest.json`'ı okur; kart adları ve manifest yolları oradan gelir.

## nginx

```nginx
location /install/  { alias /srv/narrtos/install/;  index index.html; }
location /releases/ {
    alias /srv/narrtos/releases/;
    add_header Access-Control-Allow-Origin *;        # manifest ve .bin başka origin'den de okunabilsin
    add_header Cache-Control "no-cache";            # latest.json güncel kalsın
    types { application/octet-stream bin; application/json json; }
}
```

`.bin` dosyaları `application/octet-stream`, `manifest.json` `application/json` olarak
sunulmalı; bazı sunucular `.bin`'i reddeder veya yanlış tipte verir.

## Yeni sürüm yayımlamak

```bash
python3 scripts/release.py                 # release/v<X.Y.Z>/ ve release/latest.json
rsync -av release/v<X.Y.Z> release/latest.json sunucu:/srv/narrtos/releases/
```

Sayfaya dokunmak gerekmez; `latest.json` değişince kartlar yeni sürümü gösterir. Eski
sürüm klasörleri silinmezse eski manifestler adresleriyle erişilebilir kalır.

## Yerel deneme

```bash
mkdir -p /tmp/webtest/install /tmp/webtest/releases
cp docs/web-install/index.html /tmp/webtest/install/
cp -R release/latest.json release/v* /tmp/webtest/releases/
python3 -m http.server 8765 --directory /tmp/webtest    # http://localhost:8765/install/
```

localhost güvenli sayıldığı için düğme çalışır; gerçek kartla yükleme yapılabilir.

## Sınırlar

- Safari ve Firefox'ta Web Serial yok; sayfa bunu algılar ve Chrome/Edge'e yönlendirir.
- Telefon/tablette çalışmaz.
- Yükleme 115200 baud'da yapılır; AMOLED imajı 2-3 dakika, LCD128 (galeri dahil) 4-5 dakika.
- CH343 çipli kartlarda Windows/macOS sürücü isteyebilir (dokümanda anlatılıyor).
