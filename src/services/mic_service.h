#pragma once
/**
 * @file mic_service.h
 * @brief Mikrofon ham ses yakalama (stetoskop deneyi): ES7210 → I2S → USB.
 *
 * Amaç: kalp/nefes seslerini (20-1000 Hz) bir stetoskop başlığı içindeki mikrofondan
 * (IM73A135, ES7210 MIC girişi) HAM olarak Mac'e akıtmak; analiz Mac'te. Piezo
 * `piezo capture` ile aynı kalıp: "[mic] <t_ms> v v v ..." satırları, 20 örnek/satır.
 *
 * Yol: ES7210 (I2C 0x40) 16 kHz / 16 bit / TDM 4 kanal, ESP I2S0 master (MCLK 16,
 * BCLK 41, WS 45, DIN 42 — self-test ile aynı). İstenen kanal seçilir (1=MIC1, 2=MIC2,
 * 3=MIC3 hoparlör geri beslemesi, 4=MIC4), DC çıkarılır, `fs` Hz'e ortalama alarak
 * indirgenir (USB-JTAG metin bant genişliği için; 1000 Hz → kalp sesleri 20-150 Hz rahat).
 * Yakalama bitince I2S kanalı silinir; kodek kurulumu kalır.
 */
#include <esp_err.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** seconds 1-60, fs 100-4000 (16000'in böleni olmayan değerler en yakına yuvarlanır),
 *  channel 1-4. emit her satır için çağrılır. */
esp_err_t mic_service_capture(int seconds, int fs, int channel, void (*emit)(const char*));
/** 48 kHz ikili yakalama: seconds 1-300, channel 1-4, bits 16|24, variant 0 normal | 3 HPF kapalı (kalp).
 *  USB'ye çerçeve basar (tools/mic_raw.py); emit yalnız başlangıç ve bitiş satırı için çağrılır. */
esp_err_t mic_service_raw(int seconds, int channel, int bits, int variant, void (*emit)(const char*));
/** 2026-11-09 SD kayıt: mic_service_raw kart takılıysa aynı sesi /sdcard/KAYIT/K######.WAV olarak da yazar.
 *  mic_service_sdrec yalnız SD'ye yazar (USB akışı yok; bilgisayar gerekmez). Kart yok/FAT yok → ESP_ERR_NOT_FOUND.
 *  mic_service_sd_list kayıt klasörünü listeler. Ayrıntı: mic_sd.h */
esp_err_t mic_service_sdrec(int seconds, int channel, int bits, int variant, void (*emit)(const char*));
esp_err_t mic_service_sd_list(void (*emit)(const char*));
/** 2026-11-09 kartta nabız: seconds (5-300) kayıt alır (USB akışı yok), 20-300 Hz zarf otokorelasyonu ile
 *  atım/dk + güven (r) yazar; to_sd ise WAV'ı da karta yazar; to_usb ise ham akış USB'ye de gider
 *  (tools/nar_nabiz.sh ile bilgisayarda MP3 + sonuç). Son sonuç: mic_service_last_bpm(). */
esp_err_t mic_service_nabiz(int seconds, int channel, int bits, int variant, bool to_sd, bool to_usb, int profil, void (*emit)(const char*));
/** 2026-11-10 tür profili ve alg A2 (nabiz_alg.c): "insan" 20-150 Hz 35-200/dk, "kopek" 20-150 Hz 40-250/dk (soluma
 *  tespiti), "kedi" 30-300 Hz 100-300/dk (mırlama tespiti). Nabız iki yöntemle (otokorelasyon + vuruş sayımı), ayrıca
 *  solunum sayısı. profil_bul: ad → dizin (-1 = yok); profil_sure: varsayılan kayıt süresi (sn).
 *  Sınama: tools/nabiz_test.py (kart kodunu bilgisayarda derleyip tools/nabiz_ref.py ile karşılaştırır). */
int         mic_service_profil_bul(const char* ad);
int         mic_service_profil_sure(int profil);
const char* mic_service_profil_ad(int profil);
/** Kayıt etiketi (serbest metin, en çok 63 karakter): LISTE.TXT ve "[mic] sonuc:" satırına yazılır; yeni etikete kadar kalır. */
void        mic_service_set_label(const char* text);
const char* mic_service_get_label(void);
float     mic_service_last_bpm(float* r);
/** Kodek kurulum varyantı (teşhis): 0 = selftest dizisi (varsayılan), 1 = esp-bsp es7210.c dizisi
 *  (REG40=C3, REG06=04, REG4B/4C=0F), 2 = esp-bsp ama REG4B/4C=00. */
void mic_service_set_variant(int v);
/** Saat teşhisi (yazılımla "multimetre"): I2S RX master açıkken GPIO16/41/45/42'deki kenar
 *  sayısı PCNT ile ölçülür (Hz), sonra ES7210 master yapılıp ESP slave iken BCLK/WS'nin
 *  kodekten gelip gelmediği ölçülür (gelirse MCLK kodeğe ulaşıyor ve çip canlı demektir). */
esp_err_t mic_service_clock_test(void (*emit)(const char*));
/* Keyfi GPIO listesi için ped sürme + ayrım testi (`pad test 41 40 15 16`). I2C pinleri kilit altında
 * test edilip geri bağlanır; USB/flash/GSM UART/strap pinleri reddedilir. */
esp_err_t mic_service_pad_test(const int* pins, int n, void (*emit)(const char*));

#ifdef __cplusplus
}
#endif
