Cihaz, dışarıdan metin komutlarıyla yönetilebilir — test, demo, otomasyon ve uzaktan kontrol için. Tek bir **taşıyıcı-bağımsız parser çekirdeği** iki kanaldan beslenir; komutlar yeni ekran kodu içermez, mevcut iç API'lere köprüdür.

```
USB seri (UART0 RX) ──┐        yanıtlar reply-sink ile geri döner:
BLE "NarRTOS CMD"   ──┴─► parser ──► USB'den gelen -> seriye (printf)
                                     BLE'den gelen -> telefona (notify)
```

## Komut seti

| Komut | Etkisi |
|---|---|
| `help` / `info` | Komut listesi / durum JSON'u (sürüm, uptime, heap, wifi, aktif uygulama) |
| `app launch <isim\|id>` | Uygulama açar (`EVENT_APP_SWITCH` ile — LVGL görevinde, thread-safe) |
| `app back` / `current` / `list` | Geri • aktif uygulama • isim→id tablosu |
| `swipe left\|right\|up\|down` | Jest enjeksiyonu — ekranlar arası gezinme |
| `brightness 0-255` | Ekran parlaklığı |
| `set wifi\|bt on\|off` | WiFi tercihi (NVS, kalıcı) + bağlan/kopar • BLE advertise/kes |
| `get wifi\|bt` / `toggle wifi\|bt` | Durum okuma / tersine çevirme |
| `set rail <isim> on\|off` / `get rails` | AXP2101 güç rayları (aldo1-4, dcdc2-4, bldo1-2) |
| `wake` / `reboot` | Ekranı uyandır / yeniden başlat |

<div style="border-left:4px solid #12A0AD;background:rgba(18,160,173,.10);border-radius:0 10px 10px 0;padding:14px 18px;margin:18px 0"><div style="line-height:1.6;color:inherit"><code>set rail</code> yalnızca PMIC'li board'da çalışır; PMIC yoksa komut dürüst bir hata döner. <code>DCDC1</code> (sistem rayı) bilinçli olarak listede yoktur — kapatmak cihazı öldürür.</div></div>

## USB kullanımı

Seri terminale (115200) satır yazın:

```
pio device monitor
# sonra:
help
app launch weather
brightness 30
```

Yanıtlar `[cmd]` önekiyle döner:

```
> app launch weather
[cmd] OK: launch weather (id=6)
> brightness 30
[cmd] OK: brightness=30
> get wifi
[cmd] OK: wifi connected=yes pref=on
```

<div style="border-left:4px solid #12A0AD;background:rgba(18,160,173,.10);border-radius:0 10px 10px 0;padding:14px 18px;margin:18px 0"><div style="line-height:1.6;color:inherit">⚠ <strong>macOS + CH34x köprüsü:</strong> port her açılışta cihaza reset atar. Portu <strong>bir kez</strong> açıp aynı oturumda çalışın; her komut için yeniden bağlanmayın.</div></div>

## Telefon kullanımı (BLE — özel uygulama gerekmez)

```
GATT servisi "NarRTOS CMD":
  Service : 4E010063-6D64-004E-4152-52544F530001
  RX      : ...530002  (Write / WriteNoRsp)  <- UTF-8 komut satırı yaz
  TX      : ...530003  (Notify)              <- yanıtlar buraya düşer
```

**nRF Connect ile:**

1. "NarRTOS Watch"a bağlan

2. `...530003` characteristic'inde **Notify**'ı aç (yanıtları görmek için)

3. `...530002`'ye **UTF-8 text** olarak komut yaz: `app launch weather`

4. Saatte uygulama açılır; yanıt notify'da düşer

BLE'de USB'deki port-reset sorunu yoktur; cihaz koldayken bile komut gönderilebilir.

<div style="border-left:4px solid #12A0AD;background:rgba(18,160,173,.10);border-radius:0 10px 10px 0;padding:14px 18px;margin:18px 0"><div style="line-height:1.6;color:inherit">⚠ <code>set bt off</code> komutunu <strong>telefondan</strong> gönderirseniz kendi bağlantınızı da kesersiniz. Eşleştirme bozulursa (iOS "Peer removed pairing information"): telefonda cihazı unut → yeniden bağlan.</div></div>

## Mimari notlar

- **Thread-safety:** Uygulama açma doğrudan `app_manager_launch` çağırmaz — `EVENT_APP_SWITCH` yayınlar, `app_manager` kendi LVGL görevinde açar. Geri işlemi `lv_async_call` ile LVGL görevine devredilir.

- **Ekran uyandırma:** Komut öncesi `power_service_user_activity` çağrılır; ekran kapalıyken geçiş animasyonu tamamlanmaz.

- **Eşzamanlılık:** USB ve BLE aynı anda komut gönderebilir — parser mutex ile serileşir.

- **Dosyalar:** `services/command_service.{h,cpp}` (çekirdek + USB), `services/ble_cmd_service.{h,cpp}` (GATT taşıyıcı).

## Yeni taşıyıcı eklemek

Satırı `command_service_exec_ex(line, reply_fn, ctx)`'e besleyen küçük bir adaptör yeterlidir — parser ve komut seti aynen çalışır (ör. HTTP endpoint, MQTT konusu).
