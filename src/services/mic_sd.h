#pragma once
/**
 * @file mic_sd.h
 * @brief Mikrofon kayıtlarını SD karta WAV olarak yazma (2026-11-09).
 *
 * C dosyasında: ESP-IDF'in SDMMC yapılandırma makroları C++'ta derlenmiyor (selftest_sd ile aynı sebep).
 * Bağlantı selftest_sd / narrtos-sdplayer-test ile aynı: SDMMC 1-bit, CLK GPIO2 / CMD GPIO1 / D0 GPIO3,
 * DAT3 (GPIO17) pull-up'lı giriş (kart SPI moduna düşmesin). Kart ASLA biçimlendirilmez.
 *
 * Dosya düzeni (FATFS uzun ad kapalı → 8.3 isimler):
 *   /sdcard/KAYIT/K000001.WAV   sıra numaralı kayıtlar (mono PCM, 48 kHz, 16 ya da 24 bit)
 *   /sdcard/KAYIT/LISTE.TXT     her kayıt için bir satır: dosya, tarih-saat, süre, ayar
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Kartı /sdcard'a bağlar. Başarısızsa false ve detail'e kısa neden ("kart yok", "FAT yok", ...). */
bool mic_sd_mount(char* detail, size_t n);
/** Bağlıysa ayırır; bağlı değilse hiçbir şey yapmaz. */
void mic_sd_unmount(void);
/** Bağlı mı? */
bool mic_sd_mounted(void);
/** Kart kapasitesi (MB), bağlı değilse 0. */
uint32_t mic_sd_capacity_mb(void);

/** KAYIT klasöründe bir sonraki boş K######.WAV dosyasını açar, 44 baytlık geçici başlığı yazar.
 *  name'e yalnız dosya adı (K000001.WAV) yazılır. NULL = açılamadı. */
FILE* mic_sd_wav_open(int sample_rate, int bits, char* name, size_t n);
/** Başlığı gerçek boyutla tamamlar, dosyayı kapatır, LISTE.TXT'ye satır ekler (note: serbest metin). */
bool mic_sd_wav_close(FILE* f, const char* name, uint32_t data_bytes, int sample_rate, int bits, const char* note);
/** Hiç veri yazılamadıysa: kapat ve dosyayı sil (LISTE.TXT'ye satır eklenmez). */
void mic_sd_wav_discard(FILE* f, const char* name);

/** KAYIT klasöründeki dosyaları emit ile listeler (ad, KB). Dönüş: dosya sayısı, -1 = klasör yok. */
int mic_sd_list(void (*emit)(const char*));

#ifdef __cplusplus
}
#endif
