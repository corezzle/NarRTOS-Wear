# OTA Güncelleme

NarRTOS cihazları yeni yazılımı **kablosuz olarak** indirip kendilerini güncelleyebilir —
fiziksel müdahale gerekmez.

## Akış

```
1. Manifest sorgusu  ->  sunucudaki güncel sürüm nedir?
2. Karşılaştırma     ->  OS_VERSION_CODE'dan büyük mü?
3. İndirme           ->  yeni imaj WiFi üzerinden yedek bölüme yazılır
4. Doğrulama         ->  bütünlük kontrolü
5. Yeniden başlatma  ->  yeni sürüm aktif olur
```

Kullanıcı arayüzü: **Ayarlar → General → Software Updates**. Otomatik kontrol açıksa arka
planda periyodik olarak manifest sorgulanır.

## Sürüm karşılaştırması

Sürüm bilgisi tek bir tam sayıya indirgenir — string ayrıştırma tuzaklarına düşmemek için:

```c
#define OS_VERSION_MAJOR 1
#define OS_VERSION_MINOR 4
#define OS_VERSION_PATCH 81
#define OS_VERSION_STR   "1.4.81"
// OS_VERSION_CODE = MAJOR*10000 + MINOR*100 + PATCH   -> 10481
```

> String karşılaştırmada `"0.10" < "0.2"` çıkar — integer karşılaştırma bu hatayı önler.

## Manifest formatı

```json
{
  "version":      "1.5.0",
  "version_code": 10500,
  "url":          "https://sunucu/firmware/narrtos-1.5.0.bin",
  "size":         2480000,
  "notes":        "Hata düzeltmeleri ve yeni özellikler"
}
```

Manifest adresi derleme bayrağıyla verilir; cihaz kimliği ve mevcut sürüm sorgu parametresi
olarak eklenir — sunucu uygun sürümü seçebilir ya da cihaz zaten güncelse `204 No Content`
döndürebilir.

## A/B partisyon düzeni

```
app0  →  çalışan sürüm
app1  →  yeni imaj buraya yazılır
```

Güncelleme başarısız olursa ya da yeni sürüm açılışta çökerse cihaz **eski sürüme döner**
(anti-crash-loop koruması). Partisyon tablosu `partitions_custom.csv` içinde tanımlıdır:
iki uygulama bölümü + NVS + varlık (assets) bölümü.

## Yapılandırma

```ini
# platformio.ini
build_flags =
    '-DOTA_BACKEND_HOST="https://sunucunuz.example.com"'
```

`os_config.h` içindeki tüm OTA adresleri `#ifndef` korumalıdır; env üzerinden ezilebilir.

## Güncelleme yaşam döngüsü telemetrisi

Cihaz, güncelleme olaylarını (başladı / başarılı / başarısız) sunucuya raporlar. Bu
best-effort'tur: sunucuya erişilemezse OTA akışı bozulmaz, olay sessizce atlanır.

## Güvenlik notu

Üretim ortamında manifest ve imaj dağıtımı **HTTPS** üzerinden yapılmalıdır. Geliştirme
sırasında HTTP'ye izin veren yapılandırma kullanılır; sertifika sabitleme (pinning) yol
haritasındadır.
