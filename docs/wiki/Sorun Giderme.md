# Sorun Giderme

## Derleme ve yükleme

| Belirti | Neden / Çözüm |
|---|---|
| Yükleme sırasında checksum hatası (`Failed to write to target RAM`) | Bazı USB-seri köprüler 921600'de kararsızdır → `upload_speed`'i **460800**'e düşürün |
| `pio` bulunamıyor | PlatformIO Core kurulu değil: `pip install platformio` |
| Port bulunamıyor | USB-seri sürücüsü (ör. CH34x) kurulu mu? `ls /dev/cu.*` ile kontrol edin |
| Yanlış board derleniyor | `-e <env>` parametresini verin: `pio run -e esp32s3_lcd128` |
| Yaptığım değişiklik cihaza yansımıyor | Önce temizleyin, sonra derleyin: `pio run -e esp32s3_wearable -t clean`, ardından aynı komutun `-t upload` hali |

## Seri monitör

| Belirti | Neden / Çözüm |
|---|---|
| Monitörde hiç çıktı yok (0 bayt) | Okuma aracı DTR/RTS hatlarını yanlış sürüyor olabilir → portu `DTR=false` ile açıp RTS ile reset darbesi verin |
| Her bağlanışta cihaz yeniden başlıyor | macOS + CH34x köprüsünün bilinen davranışı. Portu **bir kez** açıp aynı oturumda çalışın |
| Loglar bozuk/anlamsız | Baud hızı 115200 olmalı |

DTR/RTS sorunu için çalışan Python örneği:

```python
import serial, time
ser = serial.Serial()
ser.port, ser.baudrate, ser.timeout = "/dev/cu.usbserial-XXXX", 115200, 1
ser.dtr = False; ser.rts = False      # açılışta hatları ASSERT ETME
ser.open()
ser.setDTR(False)                     # GPIO0 = high -> normal boot
ser.setRTS(True);  time.sleep(0.15)   # EN = low  -> reset'te tut
ser.setRTS(False)                     # bırak -> cihaz app'i boot eder
while True:
    print(ser.readline().decode("utf-8", "replace"), end="")
```

## Açılış (boot)

| Belirti | Neden / Çözüm |
|---|---|
| Boot'ta dokunmatik I2C hatası | Normal olabilir — cihaz geç hazırlanıyor; sürücü yine de kaydedilir ve ilk dokunuşla toparlar |
| PMIC bulunamadı uyarısı | PMIC'siz board'da beklenen davranış; sistem boot'a devam eder |
| RTC bulunamadı | Dahili saat kullanılır; NTP ile senkronlanır |
| Ekran açık ama görüntü yok | Parlaklık ramp'i başlamadan hata olmuş olabilir; boot loglarında ekran sürücüsü ve `ui_service` satırlarına bakın |

> **Hata toleransı ilkesi:** Çevre birim init'leri başarısız olsa bile sistem boot'a devam
> eder. Tek bir çevre biriminin arızası cihazı açılamaz duruma düşürmez.

## WiFi

| Belirti | Neden / Çözüm |
|---|---|
| Açılışta bağlanmıyor | Kullanıcı tercihi NVS'te OFF olabilir (bilinçli davranış) — Ayarlar'dan açılır |
| Sürekli `retry 1/5 … 5/5` | Ağ menzil dışında ya da şifre yanlış; Ayarlar → WiFi'dan yeniden bağlanın |
| Tarama sırasında bağlantı kopuyor | Beklenen — aktif tarama bağlantıyı kısa süre böler (bkz. [[Telemetri ve Konumlandırma]]) |

## Telemetri akışı durdu

Kontrol sırası:

1. **Sunucunun IP'si değişti mi?** En sık neden. Geliştirme makinesinin DHCP IP'si değişince
   cihaz ölü adrese POST atar. Kalıcı çözüm: router'da **statik DHCP rezervasyonu**.
   Geçici çözüm: derleme bayrağındaki `OTA_BACKEND_HOST`'u güncelleyip yeniden flash.
2. **Sunucu ayakta mı?** `curl http://<ip>:3001/api/wear-os/health`
3. **Cihaz açık ve WiFi'da mı?** Seri log kontrolü.

## Komut arayüzü

| Belirti | Neden / Çözüm |
|---|---|
| Komut yazıyorum, yanıt yok | Cihaz boot ortasında olabilir — `CmdSvc: init OK` satırını bekleyin |
| `app current` → `id=-1` döndü | Uygulama geçişi tamamlanmadı; ekran kapalıysa animasyon bitmez (komut öncesi `wake` gönderin) |
| BLE'de "Peer removed pairing information" | Bond uyuşmazlığı → telefonda cihazı unut, uygulamayı yeniden başlat, tekrar bağlan |
| `set rail` → `NOT_SUPPORTED` | Board'da PMIC yok — beklenen dürüst hata |

## UI / düzen

| Belirti | Neden / Çözüm |
|---|---|
| Küçük ekranda ikonlar orantısız | İkon/daire oranını koruyun (40 px dairede 24 px sembol → 26 px dairede 16 px) |
| Satır araları aşırı açık | Liste `STEP` değeri satır yüksekliği + ~8 px olmalı; büyük ekran değerini taşımayın |
| Başlık ile saat çakışıyor | Yuvarlak ekranda üst kenarda görünür genişlik dardır — öğeleri yan yana değil alt alta yerleştirin |
| Metin taşıyor / kırpılıyor | Ölçü sabitlerini varyant bloğunda tanımlayın (bkz. [[Uygulama Geliştirme]]) |

## Yardım alma

Sorun devam ediyorsa: seri boot logunu, kullandığınız build ortamını (`-e ...`) ve board
modelini ekleyerek issue açın.
