#pragma once
/**
 * @file gallery_pack.h
 * @brief Galeri görselleri — flash'ın 'gallery' bölümünden mmap ile.
 *        (narRtos-wear-c6 Session 16'dan port, Session 35.)
 *
 * Görseller RAM'e KOPYALANMAZ: esp_partition_mmap ile flash doğrudan adres
 * alanına bağlanır ve LVGL oradan çizer (sıfır RAM, sıfır kopya). Bu kartta
 * PSRAM olmasına rağmen kalıp korundu — 6 görselin tam boyu ~1 MB.
 *
 * DİKKAT: bu projede 'assets' bölümü yüklenen kadran fotoğrafları için SPIFFS
 * olarak KULLANILIYOR; paket AYRI 'gallery' bölümünde (0x800000, yalnız
 * partitions_lcd128.csv'de tanımlı).
 *
 * Paket üretici: scripts/gen_gallery_pack.py → build_assets/gallery_pack.bin
 * Yükleme:      esptool write_flash 0x800000 build_assets/gallery_pack.bin
 */
#include <stdbool.h>
#include <stdint.h>

/** Bölümü bulur, başlığı doğrular, paketi mmap eder. İdempotent.
 *  Paket yoksa/bozuksa false döner (galeri boş görünür, çökmez). */
bool gallery_pack_init(void);

/** Paketteki görsel sayısı (init edilmemişse 0). */
int  gallery_pack_count(void);

/** Izgara kapağı (kırpılmış). Ölçüler out_w/out_h'ye yazılır. */
const void* gallery_pack_thumb(int idx, uint16_t* out_w, uint16_t* out_h);

/** Tam ekran görsel (tamamı sığdırılmış). */
const void* gallery_pack_full(int idx, uint16_t* out_w, uint16_t* out_h);
