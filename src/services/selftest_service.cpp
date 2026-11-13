/**
 * @file selftest_service.cpp
 * @brief Açılış donanım checklist'i — yalnız seri monitör.
 *
 * narrtos-board-selftest (ayrı doğrulama projesi) otomatik testlerinin OS
 * portu. Farklar:
 *  - Ekrana hiçbir şey çizilmez; satırlar ve özet tablo printf ile seriye.
 *  - Görsel/etkileşimli testler yok (ekran rengi, dokunmatik yüzey, butonlar):
 *    ekran, dokunmatik ve butonlar OS'un kullanımında.
 *  - Panel kimliği okunmaz (QSPI/SPI hattı LVGL flush'ıyla çakışır); ekran
 *    satırı "sürücü başladı" bilgisini verir (co5300_init ESP_ERROR_CHECK'li,
 *    buraya gelindiyse başarılı).
 *  - OS sürücülerinin ESP_ERROR_CHECK'li okuma fonksiyonları ÇAĞRILMAZ
 *    (çip yoksa abort eder); register'lar doğrudan, mutex'li i2c_bus ile okunur.
 *  - Beklentiler board varyantına göre (AMOLED 2.06 / 1.8 / LCD128).
 *
 * GÜVENLİK: raylar asla kapatılmaz, RTC'ye yazılmaz, SD biçimlendirilmez.
 * Tek geçici yazma: PMIC TS ADC kanalı (okuyup eski değerine döner) ve OS'un
 * kullanmadığı QMI8658'de hızlanma ölçümü (kontrol register'ları geri yüklenir).
 */

#include "selftest_service.h"
#include "selftest_sd.h"
#include "selftest_ext.h"
#include "wifi_service.h"
#include "../core/os_config.h"
#include "../hal/i2c_bus.h"
#include "../drivers/power/axp2101_driver.h"
#include "../drivers/display/co5300_driver.h"

#include <esp_flash.h>
#include <esp_psram.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char* TAG = "Selftest";

/* ─── Board beklentileri ───────────────────────────────────────────────────── */

typedef struct { uint8_t addr; const char* name; } i2c_dev_t;

#ifdef BOARD_VARIANT_LCD128
static const char* const k_board = "LCD128 240x240";
static const i2c_dev_t k_expect[] = {
    { 0x15, "CST816S dokunmatik" },
    { 0x6B, "QMI8658 IMU" },
};
#define HAS_PMIC        0
#define HAS_RTC         0
#define HAS_CODEC       0
#define EXP_PSRAM_MB    2
#define EXP_FLASH_MB    16
#define TOUCH_ADDR      0x15
#define TOUCH_NAME      "CST816S"
#define DISPLAY_DESC    "GC9A01A 240x240 surucu baslatildi"
#elif defined(BOARD_VARIANT_NAR)
/* NAR-ESP32S3: 2.06 ile aynı çipler, RTC yok; 16 MB PSRAM + 32 MB flash */
static const char* const k_board = "NAR-ESP32S3";
static const i2c_dev_t k_expect[] = {
    { 0x18, "ES8311 kodek" },
    { 0x34, "AXP2101 PMIC" },
    { 0x15, "CST820 dokunmatik" },
    { 0x40, "ES7210 mikrofon ADC" },
    { 0x51, "PCF85063 RTC (SiP ici)" },
    { 0x68, "ICM42670 IMU" },
};
#define HAS_PMIC        1
#define HAS_RTC         1
#define HAS_CODEC       1
#define EXP_PSRAM_MB    16
#define EXP_FLASH_MB    32
#define TOUCH_ADDR      I2C_ADDR_CST820
#define TOUCH_NAME      "CST820"
#define DISPLAY_DESC    "CO5300 410x502 surucu baslatildi"
#else
#ifdef DISPLAY_VARIANT_18INCH
static const char* const k_board = "AMOLED 1.8";
#define DISPLAY_DESC    "SH8601 368x448 surucu baslatildi"
#else
static const char* const k_board = "AMOLED 2.06";
#define DISPLAY_DESC    "CO5300 410x502 surucu baslatildi"
#endif
static const i2c_dev_t k_expect[] = {
    { 0x18, "ES8311 kodek" },
    { 0x34, "AXP2101 PMIC" },
    { 0x38, "FT3168 dokunmatik" },
    { 0x40, "ES7210 mikrofon ADC" },
    { 0x51, "PCF85063 RTC" },
};
#define HAS_PMIC        1
#define HAS_RTC         1
#define HAS_CODEC       1
#define EXP_PSRAM_MB    8
#define EXP_FLASH_MB    8
#define TOUCH_ADDR      I2C_ADDR_FT3168
#define TOUCH_NAME      "FT3168"
#endif

/* ─── Satırlar ─────────────────────────────────────────────────────────────── */

typedef enum { ST_WAIT, ST_PASS, ST_FAIL, ST_WARN, ST_SKIP, ST_ASK_OR_SKIP } st_res_t;

typedef enum {
    R_MEM, R_I2C, R_PMIC, R_RAILS, R_POWER, R_TEMP, R_RTC, R_IMU, R_TOUCH,
    R_CODEC, R_MICADC, R_DISPLAY, R_SD,
#ifdef BOARD_VARIANT_NAR
    R_INFO, R_I2CLVL, R_SPK, R_MICS, R_MOTOR, R_WIFI, R_LTE,   /* Session 46q: selftest_ext */
#endif
    R_COUNT
} row_id_t;

static const char* const k_row_name[R_COUNT] = {
    "PSRAM/Flash", "I2C hatti", "PMIC AXP2101", "Guc raylari", "USB / Pil",
    "Sicaklik", "RTC PCF85063", "IMU", "Dokunmatik cip", "Kodek ES8311", "Mik ADC ES7210",
    "Ekran", "SD kart",
#ifdef BOARD_VARIANT_NAR
    "Kart bilgisi", "I2C hat seviyesi", "Hoparlor yolu", "Mikrofonlar", "Titresim motoru", "Wi-Fi", "GSM modulu",
#endif
};

typedef struct { st_res_t res; char detail[104]; } row_t;

static row_t s_rows[R_COUNT];
static bool  s_found[128];

/* IMU iç sıcaklığı — t_imu ölçer, t_temp raporlar */
static bool  s_imu_temp_ok = false;
static float s_imu_temp_c  = 0.0f;

/* ─── Yardımcılar ──────────────────────────────────────────────────────────── */

static const char* res_tag(st_res_t r)
{
    switch (r) {
        case ST_PASS: return "PASS";
        case ST_FAIL: return "FAIL";
        case ST_WARN: return "UYARI";
        case ST_SKIP: return "ATLA";
        case ST_ASK_OR_SKIP: return " ?? ";
        default:      return " .. ";
    }
}

/* Seri satır — USB-JTAG FIFO'su 64 bayt; art arda çok satırda host'a
 * boşaltma payı bırak (board-selftest / C6 dersi). */
static void sp(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(4));
}

static void set_row(row_id_t id, st_res_t res, const char* fmt, ...)
{
    s_rows[id].res = res;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_rows[id].detail, sizeof(s_rows[id].detail), fmt, ap);
    va_end(ap);
    sp("[%-5s] %-17s %s\n", res_tag(res), k_row_name[id], s_rows[id].detail);
}

static bool rd(uint8_t addr, uint8_t reg, uint8_t* out, size_t n = 1)
{
    return i2c_bus_read_reg(addr, reg, out, n) == ESP_OK;
}

static bool wr(uint8_t addr, uint8_t reg, uint8_t v)
{
    return i2c_bus_write_reg(addr, reg, v) == ESP_OK;
}

static bool probe(uint8_t addr)
{
    uint8_t v;
    return rd(addr, 0x00, &v, 1);
}

static inline int bcd(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }

/* ─── Testler ──────────────────────────────────────────────────────────────── */

static void t_mem(void)
{
    size_t   ps = esp_psram_get_size();
    uint32_t fl = 0;
    esp_flash_get_size(NULL, &fl);

    bool pat_ok = false;
    const size_t N = 256 * 1024;
    uint8_t* p = (uint8_t*)heap_caps_malloc(N, MALLOC_CAP_SPIRAM);
    if (p) {
        for (size_t i = 0; i < N; i++) p[i] = (uint8_t)(i * 31 + 7);
        pat_ok = true;
        for (size_t i = 0; i < N; i++) if (p[i] != (uint8_t)(i * 31 + 7)) { pat_ok = false; break; }
        heap_caps_free(p);
    }

    unsigned ps_mb = (unsigned)(ps / (1024 * 1024));
    unsigned fl_mb = (unsigned)(fl / (1024 * 1024));
    if (ps == 0)
        set_row(R_MEM, ST_FAIL, "PSRAM yok (en az %d MB bekleniyor)", EXP_PSRAM_MB);
    else if (!pat_ok)
        set_row(R_MEM, ST_FAIL, "PSRAM %u MB, yaz/oku HATALI (bos PSRAM yetersiz?)", ps_mb);
    else if (ps_mb < EXP_PSRAM_MB || fl_mb < EXP_FLASH_MB)
        set_row(R_MEM, ST_WARN, "PSRAM %u MB, flash %u MB (en az %d/%d bekleniyor)",
                ps_mb, fl_mb, EXP_PSRAM_MB, EXP_FLASH_MB);
    else
        set_row(R_MEM, ST_PASS, "PSRAM %u MB, flash %u MB", ps_mb, fl_mb);
}

static const char* guess_name(uint8_t a)
{
    for (size_t i = 0; i < sizeof(k_expect) / sizeof(k_expect[0]); i++)
        if (k_expect[i].addr == a) return k_expect[i].name;
    switch (a) {
        case 0x15: return "CST816S dokunmatik olabilir";
        case 0x18: return "ES8311 kodek olabilir";
        case 0x34: return "AXP2101 PMIC olabilir";
        case 0x38: return "FT3168 dokunmatik olabilir";
        case 0x40: return "ES7210 mikrofon ADC olabilir";
        case 0x51: return "PCF85063 RTC olabilir";
        case 0x6A: case 0x6B: return "QMI8658 IMU olabilir";
        case 0x68: case 0x69: return "ICM42670 IMU olabilir";
        default:   return "bilinmiyor";
    }
}

static void t_i2c(void)
{
    memset(s_found, 0, sizeof(s_found));
    /* Olmayan adreslerde sürücü hata logu basabilir — tarama boyunca sustur
     * (yaklaşık bir saniye), sonra projenin varsayılan seviyesine dön. */
    esp_log_level_set("*", ESP_LOG_NONE);
    int cnt = 0;
    for (int a = 0x08; a <= 0x77; a++) {
        if (probe((uint8_t)a)) { s_found[a] = true; cnt++; }
    }
    esp_log_level_set("*", (esp_log_level_t)CONFIG_LOG_DEFAULT_LEVEL);

    sp("   I2C bulunanlar (SDA %d / SCL %d):\n", PIN_I2C_SDA, PIN_I2C_SCL);
    for (int a = 0x08; a <= 0x77; a++)
        if (s_found[a]) sp("     0x%02X  %s\n", a, guess_name((uint8_t)a));

    /* ES8311 adresi CE pinine bagli: LOW 0x18, HIGH 0x19 (CE pull-down'u R12 eksik/lehimsiz kartlar) */
    bool es_alt = !s_found[0x18] && s_found[0x19];
    if (es_alt) { s_found[0x18] = true; sp("   NOT: ES8311 0x18 yerine 0x19'da (CE pini HIGH - R12 10K pull-down?)\n"); }
    char missing[48] = "";
    for (size_t i = 0; i < sizeof(k_expect) / sizeof(k_expect[0]); i++) {
        if (!s_found[k_expect[i].addr]) {
            char t[8];
            snprintf(t, sizeof(t), " %02X", k_expect[i].addr);
            strlcat(missing, t, sizeof(missing));
        }
    }
    if (cnt == 0)
        set_row(R_I2C, ST_FAIL, "hic cihaz cevap vermedi (SDA/SCL?)");
    else if (missing[0])
        set_row(R_I2C, ST_WARN, "%d cihaz; eksik:%s", cnt, missing);
    else
        set_row(R_I2C, ST_PASS, "%d cihaz, beklenenlerin hepsi var", cnt);
}

#if HAS_PMIC
static void t_pmic(void)
{
    uint8_t id = 0;
    if (!rd(I2C_ADDR_AXP2101, 0x03, &id))
        set_row(R_PMIC, ST_FAIL, "cevap yok (0x34)");
    else if (id != 0x4A)
        set_row(R_PMIC, id == 0x47 ? ST_PASS : ST_FAIL,
                id == 0x47 ? "kimlik 0x%02X (NAR varyanti)" : "kimlik 0x%02X (0x4A ya da 0x47 bekleniyor)", id);
    else
        set_row(R_PMIC, ST_PASS, "kimlik 0x4A");
}

typedef enum { K_DC1, K_DC2, K_DC3, K_DC4, K_DC5, K_L100, K_L50 } vkind_t;
typedef struct {
    const char* name; uint8_t en_reg; uint8_t en_bit; uint8_t vol_reg; vkind_t kind; const char* use;
} rail_t;

static const rail_t k_rails[] = {
    { "DCDC1",   0x80, 0, 0x82, K_DC1,  "VCC3V3 sistem (kapanmaz)" },
    { "DCDC2",   0x80, 1, 0x83, K_DC2,  "0.9V ?" },
    { "DCDC3",   0x80, 2, 0x84, K_DC3,  "1.2V ?" },
    { "DCDC4",   0x80, 3, 0x85, K_DC4,  "1.8V (IO?)" },
    { "DCDC5",   0x80, 4, 0x86, K_DC5,  "-" },
    { "ALDO1",   0x90, 0, 0x92, K_L100, "dokunmatik/IMU/RTC" },
    { "ALDO2",   0x90, 1, 0x93, K_L100, "EKRAN" },
    { "ALDO3",   0x90, 2, 0x94, K_L100, "VCC3V3 + motor" },
    { "ALDO4",   0x90, 3, 0x95, K_L100, "1.8V ?" },
    { "BLDO1",   0x90, 4, 0x96, K_L100, "1.2V ?" },
    { "BLDO2",   0x90, 5, 0x97, K_L100, "2.8V ?" },
    { "CPUSLDO", 0x90, 6, 0x98, K_L50,  "ESP dahili" },
    { "DLDO1",   0x90, 7, 0x99, K_L100, "-" },
    { "DLDO2",   0x91, 0, 0x9A, K_L50,  "-" },
};

/* AXP2101 voltaj kodları (datasheet aralıkları) */
static int rail_mv(vkind_t k, uint8_t raw)
{
    switch (k) {
        case K_DC1: return 1500 + 100 * (raw & 0x1F);
        case K_DC5: return 1400 + 100 * (raw & 0x1F);
        case K_DC2:
        case K_DC4: {
            uint8_t v = raw & 0x7F;
            if (v <= 0x46) return 500 + 10 * v;
            return 1220 + 20 * (v - 0x47);
        }
        case K_DC3: {
            uint8_t v = raw & 0x7F;
            if (v <= 0x46) return 500 + 10 * v;
            if (v <= 0x57) return 1220 + 20 * (v - 0x47);
            return 1600 + 100 * (v - 0x58);
        }
        case K_L100: return 500 + 100 * (raw & 0x1F);
        case K_L50:  return 500 + 50 * (raw & 0x1F);
    }
    return 0;
}

static void t_rails(void)
{
    uint8_t en80 = 0, en90 = 0, en91 = 0;
    if (!rd(I2C_ADDR_AXP2101, 0x80, &en80) || !rd(I2C_ADDR_AXP2101, 0x90, &en90) ||
        !rd(I2C_ADDR_AXP2101, 0x91, &en91)) {
        set_row(R_RAILS, ST_FAIL, "PMIC okunamadi");
        return;
    }
    sp("   Ray        Durum  Ayarli     Besledigi\n");
    int dc1_mv = 0; bool dc1_on = false, a1_on = false, a2_on = false, a4_on = true;
    for (size_t i = 0; i < sizeof(k_rails) / sizeof(k_rails[0]); i++) {
        const rail_t& r = k_rails[i];
        uint8_t en = (r.en_reg == 0x80) ? en80 : (r.en_reg == 0x90) ? en90 : en91;
        bool on = (en >> r.en_bit) & 1;
        uint8_t raw = 0;
        int mv = rd(I2C_ADDR_AXP2101, r.vol_reg, &raw) ? rail_mv(r.kind, raw) : -1;
        sp("   %-8s   %-6s %5d mV   %s\n", r.name, on ? "ACIK" : "kapali", mv, r.use);
        if (!strcmp(r.name, "DCDC1")) { dc1_on = on; dc1_mv = mv; }
        if (!strcmp(r.name, "ALDO1")) a1_on = on;
        if (!strcmp(r.name, "ALDO2")) a2_on = on;
        if (!strcmp(r.name, "ALDO4")) a4_on = on;
    }
    if (!dc1_on || dc1_mv < 3000 || dc1_mv > 3600)
        set_row(R_RAILS, ST_FAIL, "DCDC1 %s %d mV (3.3V bekleniyor)", dc1_on ? "acik" : "KAPALI", dc1_mv);
    else if (!a2_on)
        set_row(R_RAILS, ST_FAIL, "ALDO2 (ekran) KAPALI");
    else if (!a1_on)
        set_row(R_RAILS, ST_FAIL, "ALDO1 (dokunmatik/RTC) KAPALI");
#ifdef BOARD_VARIANT_NAR
    else if (!a4_on)
        set_row(R_RAILS, ST_FAIL, "ALDO4 (IMU VDD) KAPALI - I2C hatti kirpilir");
#endif
    else
        set_row(R_RAILS, ST_PASS, "DCDC1 %d mV, ALDO1+ALDO2 acik", dc1_mv);
}

static uint16_t adc14(uint8_t reg)
{
    uint8_t b[2] = {0, 0};
    if (!rd(I2C_ADDR_AXP2101, reg, b, 2)) return 0;
    return (uint16_t)(((b[0] & 0x3F) << 8) | b[1]);
}

static void t_power(void)
{
    uint8_t s0 = 0;
    if (!rd(I2C_ADDR_AXP2101, 0x00, &s0)) { set_row(R_POWER, ST_FAIL, "PMIC okunamadi"); return; }
    bool vbus = s0 & 0x20;
    bool batp = s0 & 0x08;
    uint16_t vbus_mv = adc14(0x38);
    uint16_t vsys_mv = adc14(0x3A);
    axp2101_bat_status_t bs = {};
    axp2101_get_bat_status(&bs);
    batp = bs.bat_present;   /* STAT0 biti + VBAT >= 2.5 V (bostaki BAT pini pil sayilmasin) */

    sp("   STAT0=0x%02X  VBUS %s %u mV  VSYS %u mV\n", s0, vbus ? "var" : "yok", vbus_mv, vsys_mv);
    sp("   pil %s  %u mV  %%%u  %d mA  %s\n", batp ? "takili" : "yok", bs.voltage_mv,
       bs.percent, bs.current_ma, (s0 & 0x04) ? "sarj oluyor" : "");

    /* 0x47 varyanti: pil TAKILI DEGILKEN de BAT pini ~0.76 V okunur ve cip "pil var" der.
     * VBAT < 2.5 V iken "takili degil" ile "takili ama bos / voltaj ulasmiyor" AYIRT EDILEMEZ
     * -> "pil yok" demek yerine olculen voltaj ve olasi nedenler yazilir. */
    char bat[80];
    if (batp)            snprintf(bat, sizeof(bat), "pil %%%u %u mV%s", bs.percent, bs.voltage_mv, (s0 & 0x04) ? "" : "");
    else if (s0 & 0x08)  snprintf(bat, sizeof(bat), "pil gorulmuyor (VBAT %u mV: takili degil / tamamen bos / baglanti)", bs.voltage_mv);
    else                 snprintf(bat, sizeof(bat), "pil yok");
    if (vbus) set_row(R_POWER, ST_PASS, "USB var, %s", bat);
    else      set_row(R_POWER, ST_WARN, "USB yok (pilden calisiyor), %s", bat);
}
#endif /* HAS_PMIC */

static void t_imu(void)
{
    s_imu_temp_ok = false;

    /* QMI8658: WHO_AM_I reg 0x00 = 0x05. OS bu çipi sürmüyor; ölçüm için
     * hızlanmayı geçici aç, kontrol register'larını eski değerlerine döndür. */
    static const uint8_t k_qmi_addr[] = { 0x6B, 0x6A };
    for (uint8_t a : k_qmi_addr) {
        uint8_t who = 0;
        if (!rd(a, 0x00, &who)) continue;
        if (who != 0x05) {
            set_row(R_IMU, ST_WARN, "0x%02X cevap var, kimlik 0x%02X (0x05?)", a, who);
            return;
        }
        uint8_t c1 = 0, c2 = 0, c7 = 0;
        rd(a, 0x02, &c1); rd(a, 0x03, &c2); rd(a, 0x08, &c7);
        wr(a, 0x02, (uint8_t)(c1 | 0x40));   /* adres oto-artış */
        wr(a, 0x03, 0x03);                   /* ±2g */
        wr(a, 0x08, (uint8_t)(c7 | 0x01));   /* akselerometre aç */
        vTaskDelay(pdMS_TO_TICKS(150));
        uint8_t raw[6] = {0}, t[2] = {0, 0};
        bool ok = rd(a, 0x35, raw, 6);
        if (rd(a, 0x33, t, 2)) {
            s_imu_temp_c  = (float)(int8_t)t[1] + (float)t[0] / 256.0f;
            s_imu_temp_ok = true;
        }
        wr(a, 0x08, c7); wr(a, 0x03, c2); wr(a, 0x02, c1);
        if (!ok) { set_row(R_IMU, ST_WARN, "QMI8658 kimlik OK, veri okunamadi"); return; }
        int16_t x = (int16_t)(raw[0] | (raw[1] << 8));
        int16_t y = (int16_t)(raw[2] | (raw[3] << 8));
        int16_t z = (int16_t)(raw[4] | (raw[5] << 8));
        float g = sqrtf((float)x * x + (float)y * y + (float)z * z) / 16384.0f;
        sp("   QMI8658 @0x%02X  ham x=%d y=%d z=%d  toplam %.2f g\n", a, x, y, z, g);
        if (g > 0.75f && g < 1.3f)
            set_row(R_IMU, ST_PASS, "QMI8658 @0x%02X, %.2f g", a, g);
        else
            set_row(R_IMU, ST_WARN, "QMI8658 kimlik OK, olcum garip %.2f g", g);
        return;
    }

    /* ICM42670: WHO_AM_I reg 0x75 = 0x67. OS sürücüsü çalıştırıyor — yalnız okunur. */
    static const uint8_t k_icm_addr[] = { 0x68, 0x69 };
    for (uint8_t a : k_icm_addr) {
        uint8_t who = 0;
        if (!rd(a, 0x75, &who)) continue;
        if (who != 0x67) { set_row(R_IMU, ST_WARN, "0x%02X cevap var, kimlik 0x%02X", a, who); return; }

        uint8_t t[2] = {0, 0};
        /* 0x8000 = geçersiz örnek (sensör o an kapalı) → -231 C gibi saçma değer */
        if (rd(a, 0x09, t, 2) && !(t[0] == 0x80 && t[1] == 0x00)) {
            s_imu_temp_c  = (float)(int16_t)((t[0] << 8) | t[1]) / 128.0f + 25.0f;
            s_imu_temp_ok = true;
        }
        uint8_t pwr = 0, cfg = 0, raw[6] = {0};
        if (rd(a, 0x1F, &pwr) && (pwr & 0x03) && rd(a, 0x21, &cfg) && rd(a, 0x0B, raw, 6)) {
            static const float k_lsb[4] = { 2048.0f, 4096.0f, 8192.0f, 16384.0f };
            float lsb = k_lsb[(cfg >> 5) & 0x03];
            int16_t x = (int16_t)((raw[0] << 8) | raw[1]);
            int16_t y = (int16_t)((raw[2] << 8) | raw[3]);
            int16_t z = (int16_t)((raw[4] << 8) | raw[5]);
            float g = sqrtf((float)x * x + (float)y * y + (float)z * z) / lsb;
            sp("   ICM42670 @0x%02X  ham x=%d y=%d z=%d  toplam %.2f g\n", a, x, y, z, g);
            if (g > 0.75f && g < 1.3f) set_row(R_IMU, ST_PASS, "ICM42670 @0x%02X, %.2f g", a, g);
            else                       set_row(R_IMU, ST_WARN, "ICM42670 kimlik OK, olcum garip %.2f g", g);
        } else {
            set_row(R_IMU, ST_PASS, "ICM42670 @0x%02X (akselerometre su an kapali)", a);
        }
        return;
    }
    set_row(R_IMU, ST_FAIL, "cevap yok (0x6B/0x6A/0x68/0x69)");
}

/* Sıcaklık: kart NTC'si (AXP2101 TS ucu) + IMU iç sıcaklığı. PMIC çip sıcaklığı
 * bilinçli olarak yok (kullanıcı isteği: NTC yeterli).
 * TS'ye bir şey bağlı değilse gerilim uçlara yapışır; NTC'li pil takılırsa
 * anlamlı bir değere oturur. */
static void t_temp(void)
{
    char imu_part[24];
    if (s_imu_temp_ok) snprintf(imu_part, sizeof(imu_part), "IMU %.1f C", s_imu_temp_c);
    else               snprintf(imu_part, sizeof(imu_part), "IMU okunamadi");

#if HAS_PMIC
    /* Kart NTC'si (AXP2101 TS ucu, 10K): akım kaynağı varsayılan KAPALI (0x50=0x12)
     * → okuma tavana yapışır. Geçici olarak 0x50=0x1E (şarjı etkilemez, kaynak
     * sürekli açık, 50 uA) + TS ADC kanalı açılır; ölçülür; ikisi de eski haline
     * döner. ADC TS'yi yavaş tazeler → 1.2 s beklenir (kısa beklemede bayat örnek). */
    uint8_t  ch = 0, tsctl = 0;
    bool     ts_present = false;
    uint16_t ts_mv = 0;
    float    ntc_c = 0.0f;
    if (rd(I2C_ADDR_AXP2101, 0x30, &ch) && rd(I2C_ADDR_AXP2101, 0x50, &tsctl)) {
        wr(I2C_ADDR_AXP2101, 0x50, 0x1E);
        wr(I2C_ADDR_AXP2101, 0x30, (uint8_t)(ch | 0x02));
        vTaskDelay(pdMS_TO_TICKS(1200));
        ts_mv = (uint16_t)(adc14(0x36) / 2);   /* 0.5 mV/LSB */
        wr(I2C_ADDR_AXP2101, 0x30, ch);
        wr(I2C_ADDR_AXP2101, 0x50, tsctl);
        ts_present = (ts_mv > 30 && ts_mv < 3000);
        if (ts_present) {
            float r = (float)ts_mv / 1000.0f / 50e-6f;           /* R = V / 50 uA */
            ntc_c = 1.0f / (1.0f / 298.15f + logf(r / 10000.0f) / 3435.0f) - 273.15f;   /* 10K, B=3435 */
        }
    }

    char det[104];
    if (ts_present) snprintf(det, sizeof(det), "NTC %.1f C (%u mV)   %s", ntc_c, ts_mv, imu_part);
    else            snprintf(det, sizeof(det), "NTC yok (TS bosta)   %s", imu_part);
    bool ntc_bad = ts_present && (ntc_c < 0.0f || ntc_c > 85.0f);
    bool imu_bad = s_imu_temp_ok && (s_imu_temp_c < 0.0f || s_imu_temp_c > 85.0f);
    if (ntc_bad || imu_bad)            set_row(R_TEMP, ST_WARN, "%s", det);
    else if (ts_present || s_imu_temp_ok)     set_row(R_TEMP, ST_PASS, "%s", det);
    else                               set_row(R_TEMP, ST_SKIP, "%s", det);
#else
    if (!s_imu_temp_ok)                               set_row(R_TEMP, ST_SKIP, "PMIC yok, IMU sicakligi okunamadi");
    else if (s_imu_temp_c < 0.0f || s_imu_temp_c > 85.0f) set_row(R_TEMP, ST_WARN, "%s (PMIC yok)", imu_part);
    else                                              set_row(R_TEMP, ST_PASS, "%s (PMIC yok)", imu_part);
#endif
}

#if HAS_RTC
/* Register'lar doğrudan okunur (sürücü fonksiyonu log basıyor). Saate YAZILMAZ. */
static bool rtc_read(uint8_t* sec, uint8_t* min, uint8_t* hour)
{
    uint8_t b[3] = {0};
    if (!rd(I2C_ADDR_PCF85063, 0x04, b, 3)) return false;
    *sec = b[0]; *min = b[1]; *hour = b[2];
    return true;
}

static void t_rtc(void)
{
    uint8_t ctrl1 = 0, s1, m1, h1, s2, m2, h2;
    if (!rd(I2C_ADDR_PCF85063, 0x00, &ctrl1) || !rtc_read(&s1, &m1, &h1)) {
        set_row(R_RTC, ST_FAIL, "cevap yok (0x51)");
        return;
    }
    vTaskDelay(pdMS_TO_TICKS(2100));
    if (!rtc_read(&s2, &m2, &h2)) { set_row(R_RTC, ST_FAIL, "ikinci okuma basarisiz"); return; }

    int a = bcd(s1 & 0x7F), b = bcd(s2 & 0x7F);
    int d = (b - a + 60) % 60;
    sp("   %02d:%02d:%02d -> %02d:%02d:%02d (fark %d sn), CTRL1=0x%02X\n",
       bcd(h1 & 0x3F), bcd(m1 & 0x7F), a, bcd(h2 & 0x3F), bcd(m2 & 0x7F), b, d, ctrl1);

    if (ctrl1 & 0x20)
        set_row(R_RTC, ST_FAIL, "saat durdurulmus (STOP biti)");
    else if (d < 1 || d > 4)
        set_row(R_RTC, ST_FAIL, "saat ilerlemiyor (kristal?)");
    else if (s2 & 0x80)
        set_row(R_RTC, ST_WARN, "calisiyor, ama saat hic ayarlanmamis");
    else
        set_row(R_RTC, ST_PASS, "calisiyor %02d:%02d:%02d", bcd(h2 & 0x3F), bcd(m2 & 0x7F), b);
}
#endif /* HAS_RTC */

static void t_touch_chip(void)
{
    if (!probe(TOUCH_ADDR)) {
        set_row(R_TOUCH, ST_FAIL, "%s cevap yok (0x%02X) - flex kablo?", TOUCH_NAME, TOUCH_ADDR);
        return;
    }
#if defined(BOARD_VARIANT_LCD128) || defined(BOARD_VARIANT_NAR)
    uint8_t chip = 0, fw = 0;
    rd(TOUCH_ADDR, 0xA7, &chip);
    rd(TOUCH_ADDR, 0xA9, &fw);
    set_row(R_TOUCH, ST_PASS, "%s cevap var (chip 0x%02X fw %u)", TOUCH_NAME, chip, fw);
#else
    uint8_t chip = 0, vendor = 0;
    rd(TOUCH_ADDR, 0xA3, &chip);
    rd(TOUCH_ADDR, 0xA8, &vendor);
    set_row(R_TOUCH, ST_PASS, "FT3168 cevap var (chip 0x%02X vendor 0x%02X)", chip, vendor);
#endif
}

#if HAS_CODEC
static void t_id_chip(row_id_t row, uint8_t addr, uint8_t r1, uint8_t v1, uint8_t r2, uint8_t v2)
{
    if (!probe(addr)) { set_row(row, ST_FAIL, "cevap yok (0x%02X)", addr); return; }
    uint8_t a = 0, b = 0;
    rd(addr, r1, &a);
    rd(addr, r2, &b);
    if (a == v1 && b == v2 && addr == 0x19) set_row(row, ST_WARN, "kimlik %02X %02X ama adres 0x19 (0x18 bekleniyor - CE pini/R12?)", a, b);
    else if (a == v1 && b == v2) set_row(row, ST_PASS, "kimlik %02X %02X", a, b);
    else                    set_row(row, ST_WARN, "cevap var, kimlik %02X %02X (%02X %02X?)", a, b, v1, v2);
}
#endif

static void t_sd(void)
{
    char d[80];
    int r = selftest_sd_run(d, sizeof(d));
    set_row(R_SD, r == 0 ? ST_PASS : r == 2 ? ST_SKIP : ST_FAIL, "%s", d);
}

/* ─── Akış ─────────────────────────────────────────────────────────────────── */

static void print_table(void)
{
    int pass = 0, fail = 0, warn = 0, other = 0;
    sp("\n================ CHECKLIST ================\n");
    sp("Kart: %s   NarRTOS %s\n", k_board, OS_VERSION_STR);
    for (int i = 0; i < R_COUNT; i++) {
        sp("[%-5s] %-17s %s\n", res_tag(s_rows[i].res), k_row_name[i], s_rows[i].detail);
        switch (s_rows[i].res) {
            case ST_PASS: pass++; break;
            case ST_FAIL: fail++; break;
            case ST_WARN: warn++; break;
            default:      other++; break;
        }
    }
    sp("-------------------------------------------\n");
    sp("PASS %d  FAIL %d  UYARI %d  atlanan %d\n", pass, fail, warn, other);
    sp("===========================================\n\n");
}

#ifdef BOARD_VARIANT_NAR
static void t_ext(void);
#endif

static void selftest_task(void* arg)
{
    (void)arg;
    /* Açılış logları (Wi-Fi, BLE, UI) otursun; tablo onların arasında kaybolmasın */
    vTaskDelay(pdMS_TO_TICKS(3000));

    sp("\n=== NarRTOS donanim checklist (%s) basliyor ===\n", k_board);

    t_mem();
    t_i2c();
#if HAS_PMIC
    t_pmic();
    t_rails();
    t_power();
#else
    set_row(R_PMIC,  ST_SKIP, "bu kartta PMIC yok");
    set_row(R_RAILS, ST_SKIP, "bu kartta PMIC yok");
    set_row(R_POWER, ST_SKIP, "bu kartta PMIC yok");
#endif
    t_imu();                 /* IMU sıcaklığını t_temp için ölçer */
    t_temp();
#if HAS_RTC
    t_rtc();
#else
    set_row(R_RTC, ST_SKIP, "bu kartta RTC yok");
#endif
    t_touch_chip();
#if HAS_CODEC
    t_id_chip(R_CODEC,  (uint8_t)((!probe(0x18) && probe(0x19)) ? 0x19 : 0x18), 0xFD, 0x83, 0xFE, 0x11);   /* ES8311 CHIP ID1/ID2 */
    t_id_chip(R_MICADC, 0x40, 0x3D, 0x72, 0x3E, 0x10);   /* ES7210 CHIP ID1/ID0 */
#else
    set_row(R_CODEC,  ST_SKIP, "bu kartta kodek yok");
    set_row(R_MICADC, ST_SKIP, "bu kartta mikrofon ADC yok");
#endif
#if defined(BOARD_VARIANT_LCD128) || defined(DISPLAY_VARIANT_18INCH)
    set_row(R_DISPLAY, ST_PASS, DISPLAY_DESC);
#else
    /* CO5300: sürücü kuruldu ama panel açılışta kimlik okumasına cevap verdi mi?
     * (co5300_init [panel-id]). Cevap yoksa "??": flex/konnektör/besleme şüphesi. */
    if (co5300_panel_responded()) set_row(R_DISPLAY, ST_PASS, "%s, panel kimlik verdi", DISPLAY_DESC);
    else set_row(R_DISPLAY, ST_ASK_OR_SKIP, "surucu kuruldu ama panel kimlik vermiyor (FF) - flex/konnektor?");
#endif
    t_sd();

#ifdef BOARD_VARIANT_NAR
    t_ext();
#endif

    print_table();
    vTaskDelete(NULL);
}

#ifdef BOARD_VARIANT_NAR
/* Ek testler (selftest_ext) — narrtos-board-selftest ile ortak kod. BLE satırı OS'ta yok:
 * NimBLE yığını OS'un kullanımında, ikinci bir tarayıcı açılmaz. Wi-Fi satırı OS'un
 * kendi servisinden (wifi_service_scan_bssids) okunur; servis kapalıysa ATLA. */
static st_res_t from_stx(stx_res_t r)
{
    return r == STX_PASS ? ST_PASS : r == STX_FAIL ? ST_FAIL : r == STX_WARN ? ST_WARN : ST_SKIP;
}

static void ext_log(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(4));
}

static void t_ext(void)
{
    char d[100], d2[100];
    stx_set_logger(ext_log);
    set_row(R_INFO,   from_stx(stx_info(d, sizeof(d))), "%s", d);
    set_row(R_I2CLVL, from_stx(stx_i2c_level(d, sizeof(d))), "%s", d);

    stx_res_t mr = STX_SKIP;
    stx_res_t sr = stx_audio(d, sizeof(d), &mr, d2, sizeof(d2));
    set_row(R_SPK,  from_stx(sr), "%s", d);
    set_row(R_MICS, from_stx(mr), "%s", d2);

    set_row(R_MOTOR, from_stx(stx_motor(d, sizeof(d))), "%s", d);

    static wifi_bssid_ap_t aps[16];
    uint8_t cnt = 0;
    esp_err_t we = wifi_service_scan_bssids(aps, 16, &cnt);
    if (we == ESP_ERR_INVALID_STATE) set_row(R_WIFI, ST_SKIP, "Wi-Fi kapali ya da baska tarama suruyor");
    else if (we != ESP_OK)           set_row(R_WIFI, ST_FAIL, "tarama hatasi: %s", esp_err_to_name(we));
    else {
        int best = -127;
        for (int i = 0; i < cnt && i < 16; i++) if (aps[i].rssi > best) best = aps[i].rssi;
        if (cnt == 0)        set_row(R_WIFI, ST_WARN, "hic ag bulunamadi - anten takili mi?");
        else if (best < -85) set_row(R_WIFI, ST_WARN, "%u ag, en iyi %d dBm - sinyal cok zayif (anten?)", cnt, best);
        else                 set_row(R_WIFI, ST_PASS, "%u ag, en iyi %d dBm", cnt, best);
    }

    set_row(R_LTE, from_stx(stx_lte(d, sizeof(d))), "%s", d);
}
#endif /* BOARD_VARIANT_NAR */

esp_err_t selftest_service_start(void)
{
    /* Yalnız soğuk açılışta: deep sleep'ten her uyanışta tekrar koşmasın */
    if (esp_reset_reason() == ESP_RST_DEEPSLEEP) {
        ESP_LOGI(TAG, "deep sleep uyanisi — checklist atlandi");
        return ESP_OK;
    }
    BaseType_t r = xTaskCreatePinnedToCore(selftest_task, "selftest", 8192, NULL,
                                           2, NULL, SENSOR_TASK_CORE);
    if (r != pdPASS) {
        ESP_LOGE(TAG, "task spawn failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}
