/**
 * mic_sd.c — mikrofon kayıtları için SD kart WAV yazıcı (bkz. mic_sd.h).
 * Yalnız NAR/tasma derlemesinde (TRACKER_FEATURES); saat derlemesine girmez.
 */
#ifdef TRACKER_FEATURES
#include "mic_sd.h"
#include "../core/os_config.h"

#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"

static const char* TAG = "MicSD";
#define MOUNT   "/sdcard"
#define DIR_REC MOUNT "/KAYIT"
#define LISTE   DIR_REC "/LISTE.TXT"
#define PIN_SD_DAT3 17   /* şema: SDCS / DAT3; 1-bit modda yüksekte tutulmalı */

static sdmmc_card_t* s_card = NULL;

bool mic_sd_mounted(void) { return s_card != NULL; }

uint32_t mic_sd_capacity_mb(void)
{
    if (!s_card) return 0;
    return (uint32_t)(((unsigned long long)s_card->csd.capacity * (unsigned long long)s_card->csd.sector_size) / (1024ULL * 1024ULL));
}

bool mic_sd_mount(char* detail, size_t n)
{
    if (s_card) { snprintf(detail, n, "bagli"); return true; }
    if (PIN_SD_CLK < 0 || PIN_SD_CMD < 0 || PIN_SD_D0 < 0) { snprintf(detail, n, "bu kartta SD yuvasi yok"); return false; }

    /* DAT3/CS: derin uyku öncesi power_service bu pini tutuyor (hold); serbest bırakıp pull-up'lı giriş yap. */
    gpio_hold_dis((gpio_num_t)PIN_SD_DAT3);
    gpio_reset_pin((gpio_num_t)PIN_SD_DAT3);
    gpio_set_direction((gpio_num_t)PIN_SD_DAT3, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)PIN_SD_DAT3, GPIO_PULLUP_ONLY);

    esp_vfs_fat_sdmmc_mount_config_t mc = {
        .format_if_mount_failed = false,   /* kullanıcının kartını ASLA biçimlendirme */
        .max_files = 3,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = (gpio_num_t)PIN_SD_CLK;
    slot.cmd = (gpio_num_t)PIN_SD_CMD;
    slot.d0  = (gpio_num_t)PIN_SD_D0;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_err_t e = esp_vfs_fat_sdmmc_mount(MOUNT, &host, &slot, &mc, &s_card);
    if (e != ESP_OK) {
        s_card = NULL;
        ESP_LOGW(TAG, "mount: %s", esp_err_to_name(e));
        if (e == ESP_ERR_TIMEOUT || e == ESP_ERR_NOT_FOUND || e == ESP_ERR_INVALID_RESPONSE || e == ESP_ERR_INVALID_CRC)
            snprintf(detail, n, "kart yok / cevap yok (%s)", esp_err_to_name(e));
        else if (e == ESP_FAIL)
            snprintf(detail, n, "kart var ama FAT bolumu yok (Mac'te FAT32/exFAT degil, FAT32 bicimle)");
        else
            snprintf(detail, n, "baglanamadi (%s)", esp_err_to_name(e));
        return false;
    }
    snprintf(detail, n, "%.8s %lu MB", s_card->cid.name, (unsigned long)mic_sd_capacity_mb());
    return true;
}

void mic_sd_unmount(void)
{
    if (!s_card) return;
    esp_vfs_fat_sdcard_unmount(MOUNT, s_card);
    s_card = NULL;
}

/* 44 baytlık PCM WAV başlığı (mono, little-endian). data_bytes bilinmiyorsa 0 yazılır, kapanışta düzeltilir. */
static void wav_header(uint8_t* h, int sample_rate, int bits, uint32_t data_bytes)
{
    uint32_t byte_rate = (uint32_t)sample_rate * (uint32_t)(bits / 8);
    uint16_t block = (uint16_t)(bits / 8), ch = 1, fmt = 1;
    uint32_t riff = 36 + data_bytes, fmtlen = 16;
    memcpy(h, "RIFF", 4); memcpy(h + 4, &riff, 4); memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4); memcpy(h + 16, &fmtlen, 4); memcpy(h + 20, &fmt, 2); memcpy(h + 22, &ch, 2);
    memcpy(h + 24, &sample_rate, 4); memcpy(h + 28, &byte_rate, 4); memcpy(h + 32, &block, 2);
    uint16_t b16 = (uint16_t)bits; memcpy(h + 34, &b16, 2);
    memcpy(h + 36, "data", 4); memcpy(h + 40, &data_bytes, 4);
}

/* KAYIT klasöründeki en büyük K###### numarasını bulur (yoksa 0). */
static unsigned next_index(void)
{
    unsigned mx = 0;
    DIR* d = opendir(DIR_REC);
    if (!d) return 0;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        unsigned k = 0; char ext[8] = {0};
        if (sscanf(e->d_name, "K%6u.%3s", &k, ext) == 2 && (strcasecmp(ext, "WAV") == 0) && k > mx) mx = k;
    }
    closedir(d);
    return mx;
}

FILE* mic_sd_wav_open(int sample_rate, int bits, char* name, size_t n)
{
    if (!s_card) return NULL;
    if (mkdir(DIR_REC, 0775) != 0 && errno != EEXIST) { ESP_LOGW(TAG, "mkdir: %d", errno); return NULL; }
    unsigned idx = next_index() + 1;
    snprintf(name, n, "K%06u.WAV", idx);
    char path[48]; snprintf(path, sizeof path, DIR_REC "/%s", name);
    FILE* f = fopen(path, "wb");
    if (!f) { ESP_LOGW(TAG, "fopen %s: %d", path, errno); return NULL; }
    uint8_t h[44]; wav_header(h, sample_rate, bits, 0);
    if (fwrite(h, 1, 44, f) != 44) { fclose(f); remove(path); return NULL; }
    return f;
}

bool mic_sd_wav_close(FILE* f, const char* name, uint32_t data_bytes, int sample_rate, int bits, const char* note)
{
    if (!f) return false;
    uint8_t h[44]; wav_header(h, sample_rate, bits, data_bytes);
    bool ok = (fseek(f, 0, SEEK_SET) == 0) && (fwrite(h, 1, 44, f) == 44);
    ok = (fclose(f) == 0) && ok;

    /* LISTE.TXT: kayıt ne zaman, ne kadar, hangi ayarla. Saat yoksa "saat yok" yazılır. */
    FILE* l = fopen(LISTE, "a");
    if (l) {
        time_t now = time(NULL); struct tm tmv; localtime_r(&now, &tmv);
        char ts[24];
        if (tmv.tm_year + 1900 >= 2024) strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tmv);
        else snprintf(ts, sizeof ts, "saat yok");
        uint32_t sec = (bits / 8) > 0 && sample_rate > 0 ? data_bytes / ((uint32_t)(bits / 8) * (uint32_t)sample_rate) : 0;
        fprintf(l, "%s  %s  %lu sn  %d Hz  %d bit  %s\r\n", name, ts, (unsigned long)sec, sample_rate, bits, note ? note : "");
        fclose(l);
    }
    return ok;
}

void mic_sd_wav_discard(FILE* f, const char* name)
{
    if (f) fclose(f);
    char path[48]; snprintf(path, sizeof path, DIR_REC "/%.14s", name ? name : "");
    remove(path);
}

int mic_sd_list(void (*emit)(const char*))
{
    if (!s_card) return -1;
    DIR* d = opendir(DIR_REC);
    if (!d) return -1;
    struct dirent* e; int count = 0; char line[96];
    while ((e = readdir(d)) != NULL) {
        char nm[16]; struct stat st; char path[64];
        strlcpy(nm, e->d_name, sizeof nm);                 /* 8.3 isim; sabit uzunluk (format-truncation uyarısı) */
        snprintf(path, sizeof path, DIR_REC "/%s", nm);
        long kb = (stat(path, &st) == 0) ? (long)(st.st_size / 1024) : -1;
        snprintf(line, sizeof line, "[mic]   %-14s %8ld KB", nm, kb);
        if (emit) emit(line);
        count++;
    }
    closedir(d);
    return count;
}

#endif /* TRACKER_FEATURES */
