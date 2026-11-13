Bu sayfa daha önce hiç gömülü yazılım derlememiş biri için yazıldı. Sırayla takip ederseniz elinizdeki karta NarRTOS kurulmuş olur. İlk seferinde 20-30 dakika sürer; bunun büyük kısmı bir kereye mahsus program kurulumlarıdır. İkinci seferde iki komutla bitirirsiniz.

## Neye ihtiyacınız var

|  |  |
|---|---|
| **Bir kart** | Desteklenen ESP32-S3 kartlarından biri (bkz. Donanım ve Board Varyantları) |
| **USB kablosu** | Karta uygun uçta ve **veri aktaran** bir kablo |
| **Bilgisayar** | Windows, macOS ya da Linux; üçü de olur |

<div style="border-left:4px solid #12A0AD;background:rgba(18,160,173,.10);border-radius:0 10px 10px 0;padding:14px 18px;margin:18px 0"><div style="font-weight:800;margin-bottom:6px;color:inherit">Kablo tuzağı</div><div style="line-height:1.6;color:inherit">Telefon şarj kablolarının bir kısmı yalnız elektrik taşır, veri taşımaz. Böyle bir kabloyla kart bilgisayarda hiç görünmez ve "kart bozuk" sanırsınız. Kart görünmüyorsa denenmesi en kolay ve en sık işe yarayan şey kabloyu değiştirmektir.</div></div>

## 1. Terminali açın

Bütün kurulum yazıyla komut girilen bir pencereden yapılır. Buna terminal denir.

- **macOS:** Spotlight'ı açın (⌘ + boşluk), `Terminal` yazıp Enter

- **Windows:** Başlat menüsüne `PowerShell` yazıp Enter

- **Linux:** Ctrl + Alt + T

Açılan siyah (ya da beyaz) pencereye komutları tek tek yazıp her birinin sonunda Enter'a basacaksınız. Bir komut çalışırken bekleyin, iş bitince satır başına geri döner.

![Spotlight'ta Terminal](GORSEL: spotlight-terminal.png)

*macOS: Spotlight'a `Terminal` yazın; ilk sonuç Terminal uygulamasıdır.*

![Windows Başlat aramasında PowerShell](GORSEL: windows-baslat-powershell.png)

*Windows: Başlat menüsüne `PowerShell` yazın; ilk sonuç Windows PowerShell (çizim).*

## 2. Python kurulu mu, kontrol edin

Kullanacağımız derleme aracı Python ile yazılmış, o yüzden önce Python lazım. Terminale şunu yazın:

```
python3 --version
```

`Python 3.9.6` gibi bir sürüm numarası görüyorsanız kurulu demektir, 3. adıma geçin. `command not found` benzeri bir hata görüyorsanız [python.org/downloads](https://www.python.org/downloads/) adresinden indirip kurun.

<div style="border-left:4px solid #D9922F;background:rgba(217,146,47,.12);border-radius:0 10px 10px 0;padding:14px 18px;margin:18px 0"><div style="font-weight:800;margin-bottom:6px;color:inherit">Windows'ta kurulum sırasında dikkat</div><div style="line-height:1.6;color:inherit">Kurulum penceresinin en altındaki <strong>"Add Python to PATH"</strong> kutusunu mutlaka işaretleyin. İşaretlemezseniz Python kurulur ama terminal onu bulamaz ve sonraki adımlarda hata alırsınız.</div></div>

Windows'ta komut `python3` yerine `python` olabilir; ikisini de deneyin.

## 3. PlatformIO'yu kurun

PlatformIO, kaynak kodu karta yüklenebilir bir dosyaya çeviren araçtır. Derleyiciyi ve ESP32 araç zincirini kendisi indirir, siz ayrıca bir şey kurmazsınız.

```
pip3 install platformio
```

Kurulum birkaç dakika sürebilir. Bitince doğrulayın:

```
pio --version
```

`PlatformIO Core, version 6.x.x` gibi bir çıktı görmelisiniz.

<div style="border-left:4px solid #12A0AD;background:rgba(18,160,173,.10);border-radius:0 10px 10px 0;padding:14px 18px;margin:18px 0"><div style="font-weight:800;margin-bottom:6px;color:inherit"><code>pio: command not found</code> diyorsa</div><div style="line-height:1.6;color:inherit">program kurulmuştur ama terminal nereye kurulduğunu bilmiyordur. Tam yoluyla çağırarak devam edebilirsiniz: macOS ve Linux'ta <code>~/.platformio/penv/bin/pio</code>, Windows'ta <code>%USERPROFILE%\.platformio\penv\Scripts\pio.exe</code>. Bundan sonraki her komutta <code>pio</code> yerine bu yolu yazın.</div></div>

![Python kontrolü ve PlatformIO kurulumu](GORSEL: terminal-kurulum.gif)

*2. ve 3. adım terminalde: Python sürümü, PlatformIO kurulumu ve doğrulama. Sürüm numaraları sizde farklı olabilir.*

## 4. USB sürücüsünü kurun

Kartın üzerinde, USB'yi kartın anlayacağı sinyale çeviren küçük bir yonga vardır. Bilgisayarın bu yongayla konuşabilmesi için bir sürücü gerekir.

Çoğu bilgisayarda bu sürücü zaten vardır. **Önce 6. adımı deneyin.** Kart görünüyorsa bu adımı atlayın. Görünmüyorsa kartınızdaki yongaya göre sürücüyü kurun; en yaygın olanı CH340/CH341 ailesidir ve üreticinin sitesinden indirilebilir. Kurulumdan sonra bilgisayarı yeniden başlatın.

## 5. Proje dosyalarını indirin

```
git clone https://github.com/corezzle/NarRTOS-Wear.git
cd NarRTOS-Wear
```

İlk satır projeyi bilgisayarınıza kopyalar, ikincisi o klasörün içine girer. Bundan sonraki bütün komutları bu klasörün içindeyken çalıştıracaksınız.

`git: command not found` hatası alırsanız git kurulu değildir. Kurmak istemiyorsanız GitLab sayfasındaki **Code → Download source code → zip** bağlantısından indirip klasörü açabilir, sonra terminalde o klasöre girebilirsiniz.

## 6. Kartı bağlayın ve göründüğünü doğrulayın

Kartı USB kablosuyla bilgisayara takın. Kartın üzerinde küçük bir ışık yanmalıdır.

Bilgisayarın kartı gördüğünü doğrulayın:

```
pio device list
```

Çıktıda `/dev/cu.usbserial-...` (macOS), `/dev/ttyUSB0` (Linux) ya da `COM3` (Windows) gibi bir satır görmelisiniz. Bu, kartın adresidir.

Hiçbir şey çıkmıyorsa sırayla deneyin: **başka bir USB kablosu** (en sık sebep), başka bir USB portu, 4. adımdaki sürücü kurulumu.

![pio device list çıktısı](GORSEL: terminal-cihaz-listesi.gif)

*Kart görünüyorsa `pio device list` böyle bir satır verir. Adres (`/dev/cu.usbserial-…`, `COM3`) sizde farklı olur.*

## 7. Derleyin ve karta yükleyin

Tek komut hem derler hem yükler:

```
pio run -e esp32s3_wearable -t upload
```

**İlk derleme uzun sürer.** Derleyici ve kütüphaneler indirileceği için birkaç dakika alır. Ekranda uzun uzun satırlar akar, bu normaldir. Sonraki derlemeler, temizlemeden yapılırsa 15 saniye kadar sürer.

Sonunda `SUCCESS` yazısını görürseniz olmuştur; kart kendini yeniden başlatır ve NarRTOS açılır.

İleride kodda bir değişiklik yapıp yeniden yüklerken önce temizleyin, sonra derleyin:

```
pio run -e esp32s3_wearable -t clean
pio run -e esp32s3_wearable -t upload
```

İlk komut önceki derlemenin ürettiği dosyaları siler, ikincisi sıfırdan derleyip karta yükler. Böylece eski dosyalardan kaynaklanan "değişiklik cihaza yansımadı" sorunu yaşanmaz. Temiz derleme, ilk seferki gibi birkaç dakika sürer.

Kartınız 1.28 inç yuvarlak ekranlıysa komutun ortasındaki ortam adını değiştirin:

```
pio run -e esp32s3_lcd128 -t upload
```

Hangi kartın hangi ada karşılık geldiği Donanım ve Board Varyantları sayfasındadır.

![Derleme ve karta yükleme](GORSEL: terminal-derleme.gif)

*Derleme ve yükleme akışı. Uzun satırlar akar, sonda `SUCCESS` görünür ve kart yeniden başlar.*

## 8. Çalıştığını görün

Ekranda saat kadranı belirmelidir. Parmağınızı ekranda sağa kaydırırsanız uygulama menüsü açılır.

Cihazın içeride ne yaptığını görmek isterseniz seri monitörü açın:

```
pio device monitor
```

Açılış kayıtları akmaya başlar. Monitör açıkken klavyeden `help` yazıp Enter'a basarsanız cihazı komutlarla sürebilirsiniz (bkz. Komut Arayüzü). Monitörden çıkmak için Ctrl + C.

![Seri monitörde açılış kaydı](GORSEL: terminal-monitor.gif)

*Seri monitörde açılış kaydı. Son satır arayüzün başladığını gösterir; çıkmak için Ctrl + C.*

## İlk denemede en sık takılınan yerler

| Gördüğünüz | Sebebi ve çözümü |
|---|---|
| `pio: command not found` | PlatformIO kuruldu ama terminal bulamıyor; 3. adımdaki tam yolu kullanın |
| `pio device list` boş çıkıyor | Büyük ihtimalle kablo yalnız şarj kablosudur; önce kabloyu değiştirin, sonra sürücüyü kurun |
| Yükleme sırasında checksum hatası | Bazı USB köprüleri yüksek hızda kararsızdır (bkz. Sorun Giderme) |
| Monitörde hiç yazı yok | Yanlış hız seçilmiş olabilir; `pio device monitor` komutu doğru hızı kendisi ayarlar |
| Ekran açık ama görüntü yok | Yanlış kart ortamıyla derlenmiş olabilir; 7. adımdaki ortam adını kontrol edin |

Listede olmayan bir sorunla karşılaşırsanız Sorun Giderme sayfasına bakın.

## İlk açılışta ne olur?

1. Sistem servisleri sırayla ayağa kalkar (NVS → I2C → PMIC → olay veriyolu → …)

2. Ekran parlaklığı 0'dan yumuşak açılır (görsel "açılış" anı)

3. Saat kadranı (WatchFace) yüklenir, dokunmatik ve butonlar aktiftir

4. WiFi kullanıcı tercihi açıksa kayıtlı ağa otomatik bağlanır

---

## Build ortamları

Yukarıdaki komutlarda geçen `-e esp32s3_wearable` kısmı, hangi karta derleneceğini söyler. Desteklenen ortamlar:

| Env | Board | Öne çıkan ayarlar |
|---|---|---|
| `esp32s3_wearable` | ESP32-S3R8 + CO5300 AMOLED | 8 MB flash QIO, Octal PSRAM, upload 921600 |
| `esp32s3_lcd128` | 240×240 yuvarlak LCD kartı | 16 MB flash, Quad PSRAM, upload 460800, 240×240 |

Her ortamın kendi `sdkconfig.<env>` dosyası vardır (PSRAM tipi, flash boyutu gibi ESP-IDF ayarları). Partisyon düzeni A/B OTA'ya göre kurgulanmıştır: iki uygulama bölümü (app0/app1) + NVS + varlık (assets) bölümü.

## Yapılandırma

Sistemin tek merkezi yapılandırma dosyası `src/core/os_config.h`: sürüm, pin haritası, görev çekirdeği/öncelikleri, ekran tamponu, dış servis adresleri.

Tüm tanımlar `#ifndef` korumalıdır, yani `platformio.ini`'den derleme bayrağıyla ezilebilir:

```
build_flags =
    '-DOTA_BACKEND_HOST="http://192.168.1.50:3001"'
```

## Sonraki adımlar

- Sistemin nasıl çalıştığını anlamak → Mimari

- Kendi uygulamanızı yazmak → Uygulama Geliştirme

- Cihazı uzaktan sürmek → Komut Arayüzü

- Takıldıysanız → Sorun Giderme
