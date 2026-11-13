/** @file gallery_pack.cpp — bkz. header. */
#include "gallery_pack.h"
#include <esp_partition.h>
#include <esp_log.h>
#include <string.h>

static const char* TAG = "GalleryPack";

#define PACK_MAGIC       0x5047524E   /* 'N','R','G','P' little-endian */
#define PACK_MAX_ITEMS   32

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    uint16_t thumb_w, thumb_h;
    uint16_t full_w,  full_h;
    uint32_t reserved;
} pack_head_t;

typedef struct __attribute__((packed)) {
    uint32_t thumb_off, thumb_size;
    uint32_t full_off,  full_size;
} pack_entry_t;

static const uint8_t*            s_base = NULL;   /* mmap'lenmiş paket başı */
static esp_partition_mmap_handle_t s_map = 0;
static pack_head_t               s_head = {};
static const pack_entry_t*       s_entries = NULL;
static bool                      s_ready = false;

bool gallery_pack_init(void)
{
    if (s_ready) return true;

    const esp_partition_t* p = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "gallery");
    if (!p) { ESP_LOGW(TAG, "'gallery' bölümü yok"); return false; }

    /* Başlığı önce normal okumayla al — mmap boyutunu bilmek için. */
    pack_head_t h = {};
    esp_err_t rerr = esp_partition_read(p, 0, &h, sizeof(h));
    ESP_LOGI(TAG, "bölüm @0x%06lX size=0x%06lX read=%s",
             (unsigned long)p->address, (unsigned long)p->size, esp_err_to_name(rerr));
    if (rerr != ESP_OK) {
        ESP_LOGW(TAG, "başlık okunamadı");
        return false;
    }
    if (h.magic != PACK_MAGIC || h.count == 0 || h.count > PACK_MAX_ITEMS) {
        ESP_LOGW(TAG, "paket yok/bozuk (magic=0x%08lX count=%u)",
                 (unsigned long)h.magic, (unsigned)h.count);
        return false;
    }

    /* Paketin gerçek boyutu = son girişin sonu. Index'i oku, en büyük bitişi bul. */
    const size_t index_bytes = (size_t)h.count * sizeof(pack_entry_t);
    pack_entry_t idx[PACK_MAX_ITEMS];
    if (esp_partition_read(p, sizeof(pack_head_t), idx, index_bytes) != ESP_OK) {
        ESP_LOGW(TAG, "index okunamadı");
        return false;
    }
    size_t end = sizeof(pack_head_t) + index_bytes;
    for (int i = 0; i < h.count; i++) {
        size_t e1 = (size_t)idx[i].thumb_off + idx[i].thumb_size;
        size_t e2 = (size_t)idx[i].full_off  + idx[i].full_size;
        if (e1 > end) end = e1;
        if (e2 > end) end = e2;
    }
    if (end > p->size) { ESP_LOGE(TAG, "paket bölüme sığmıyor (%u > %u)",
                                  (unsigned)end, (unsigned)p->size); return false; }

    const void* ptr = NULL;
    esp_err_t err = esp_partition_mmap(p, 0, end, ESP_PARTITION_MMAP_DATA, &ptr, &s_map);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mmap başarısız: %s (%u bayt)", esp_err_to_name(err), (unsigned)end);
        return false;
    }

    s_base    = (const uint8_t*)ptr;
    s_head    = h;
    s_entries = (const pack_entry_t*)(s_base + sizeof(pack_head_t));
    s_ready   = true;
    ESP_LOGI(TAG, "%u görsel hazır (kapak %ux%u, tam %ux%u, %u KB mmap)",
             (unsigned)h.count, (unsigned)h.thumb_w, (unsigned)h.thumb_h,
             (unsigned)h.full_w, (unsigned)h.full_h, (unsigned)(end / 1024));
    return true;
}

int gallery_pack_count(void)
{
    return s_ready ? (int)s_head.count : 0;
}

const void* gallery_pack_thumb(int idx, uint16_t* out_w, uint16_t* out_h)
{
    if (!s_ready || idx < 0 || idx >= (int)s_head.count) return NULL;
    if (out_w) *out_w = s_head.thumb_w;
    if (out_h) *out_h = s_head.thumb_h;
    return s_base + s_entries[idx].thumb_off;
}

const void* gallery_pack_full(int idx, uint16_t* out_w, uint16_t* out_h)
{
    if (!s_ready || idx < 0 || idx >= (int)s_head.count) return NULL;
    if (out_w) *out_w = s_head.full_w;
    if (out_h) *out_h = s_head.full_h;
    return s_base + s_entries[idx].full_off;
}
