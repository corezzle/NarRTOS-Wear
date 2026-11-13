/**
 * @file gps_service.cpp — UART NMEA okuyucu (RMC + GGA), minimal ayrıştırıcı.
 * Tamamı BOARD_VARIANT_LCD128 guard'ında (AMOLED build'ı değişmez kuralı).
 */
#ifdef TRACKER_FEATURES

#include "gps_service.h"
#include <driver/uart.h>
#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static const char* TAG = "GpsSvc";

#ifndef GPS_UART_NUM
#define GPS_UART_NUM   UART_NUM_1
#endif
#ifndef GPS_UART_RX
#define GPS_UART_RX    17
#endif
#ifndef GPS_UART_TX
#define GPS_UART_TX    18
#endif
#ifndef GPS_UART_BAUD
#define GPS_UART_BAUD  9600
#endif
#ifndef GPS_EN_PIN
#define GPS_EN_PIN     -1       /* modülün EN/VCC anahtar pini; yoksa -1 */
#endif
#ifndef GPS_EN_ACTIVE_HIGH
#define GPS_EN_ACTIVE_HIGH 1
#endif
#define GPS_FIX_MAX_AGE_S   120     /* bundan eski fix "yok" sayılır */
#define GPS_LINE_MAX        128

static SemaphoreHandle_t s_lock;
static gps_fix_t  s_fix;            /* son bilinen fix */
static int64_t    s_fix_time_us;
static uint32_t   s_sentences;
static bool       s_sim_on;
static bool       s_enabled = true;
static gps_fix_t  s_sim;

/* "ddmm.mmmm" + N/S  →  derece */
static double nmea_to_deg(const char* v, char hemi)
{
    if (!v || !*v) return NAN;
    double raw = atof(v);
    int deg = (int)(raw / 100.0);
    double min = raw - deg * 100.0;
    double d = deg + min / 60.0;
    if (hemi == 'S' || hemi == 'W') d = -d;
    return d;
}

static bool checksum_ok(const char* line)
{
    if (line[0] != '$') return false;
    const char* star = strchr(line, '*');
    if (!star || strlen(star) < 3) return false;
    uint8_t cs = 0;
    for (const char* p = line + 1; p < star; p++) cs ^= (uint8_t)*p;
    return cs == (uint8_t)strtol(star + 1, NULL, 16);
}

/* Virgülle ayrılmış alanları işaretle (boş alanlar korunur). */
static int split(char* s, char** f, int max)
{
    int n = 0; f[n++] = s;
    for (char* p = s; *p && n < max; p++)
        if (*p == ',' || *p == '*') { *p = '\0'; f[n++] = p + 1; }
    return n;
}

static void handle_line(char* line)
{
    if (!checksum_ok(line)) return;
    s_sentences++;
    char* f[20]; int n = split(line, f, 20);
    const char* id = f[0] + 3;               /* $GPRMC / $GNRMC → "RMC" */

    if (strncmp(id, "RMC", 3) == 0 && n >= 8) {
        /* $xxRMC,time,status(A/V),lat,N,lon,E,speed(kn),... */
        bool ok = f[2][0] == 'A';
        double lat = nmea_to_deg(f[3], f[4][0]), lng = nmea_to_deg(f[5], f[6][0]);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (ok && !isnan(lat) && !isnan(lng)) {
            s_fix.valid = true; s_fix.lat = lat; s_fix.lng = lng;
            s_fix.speed_kmh = (float)(atof(f[7]) * 1.852);
            s_fix_time_us = esp_timer_get_time();
        } else {
            s_fix.valid = false;
        }
        xSemaphoreGive(s_lock);
    } else if (strncmp(id, "GGA", 3) == 0 && n >= 9) {
        /* $xxGGA,time,lat,N,lon,E,quality,sats,hdop,... */
        int quality = atoi(f[6]);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_fix.sats = (uint8_t)atoi(f[7]);
        s_fix.hdop = (float)atof(f[8]);
        if (quality > 0) {
            double lat = nmea_to_deg(f[2], f[3][0]), lng = nmea_to_deg(f[4], f[5][0]);
            if (!isnan(lat) && !isnan(lng)) {
                s_fix.valid = true; s_fix.lat = lat; s_fix.lng = lng;
                s_fix_time_us = esp_timer_get_time();
            }
        }
        xSemaphoreGive(s_lock);
    }
}

static void gps_task(void* arg)
{
    (void)arg;
    static char line[GPS_LINE_MAX]; int len = 0;
    uint8_t buf[64];
    for (;;) {
        if (!s_enabled) { vTaskDelay(pdMS_TO_TICKS(500)); continue; }
        int r = uart_read_bytes(GPS_UART_NUM, buf, sizeof(buf), pdMS_TO_TICKS(500));
        for (int i = 0; i < r; i++) {
            char c = (char)buf[i];
            if (c == '\n' || c == '\r') {
                if (len > 6) { line[len] = '\0'; handle_line(line); }
                len = 0;
            } else if (len < GPS_LINE_MAX - 1) {
                line[len++] = c;
            } else len = 0;   /* taşma: satırı at */
        }
    }
}

static void drive_en(bool on)
{
#if GPS_EN_PIN >= 0
    gpio_set_level((gpio_num_t)GPS_EN_PIN, (on ? 1 : 0) ^ (GPS_EN_ACTIVE_HIGH ? 0 : 1));
#else
    (void)on;
#endif
}

void gps_service_set_enabled(bool on)
{
    if (s_enabled == on) return;
    s_enabled = on;
    drive_en(on);
    if (!on) uart_flush_input(GPS_UART_NUM);
    ESP_LOGI(TAG, "%s (EN pin %d)", on ? "ENABLED" : "DISABLED (power save)", GPS_EN_PIN);
}
bool gps_service_is_enabled(void) { return s_enabled; }

#ifndef GPS_ENABLED
#define GPS_ENABLED 1
#endif

esp_err_t gps_service_init(void)
{
    s_lock = xSemaphoreCreateMutex();
#if !GPS_ENABLED
    ESP_LOGI(TAG, "GPS devre disi (GPS_ENABLED=0) — modul takilinca bayragi ac");
    return ESP_OK;
#endif
#if GPS_EN_PIN >= 0
    gpio_config_t io = {}; io.pin_bit_mask = 1ULL << GPS_EN_PIN; io.mode = GPIO_MODE_OUTPUT;
    gpio_config(&io); drive_en(true);
#endif
    uart_config_t cfg = {};
    cfg.baud_rate = GPS_UART_BAUD;
    cfg.data_bits = UART_DATA_8_BITS;
    cfg.parity    = UART_PARITY_DISABLE;
    cfg.stop_bits = UART_STOP_BITS_1;
    cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    cfg.source_clk = UART_SCLK_DEFAULT;
    esp_err_t err = uart_driver_install(GPS_UART_NUM, 1024, 0, 0, NULL, 0);
    if (err != ESP_OK) { ESP_LOGE(TAG, "uart install: %s", esp_err_to_name(err)); return err; }
    uart_param_config(GPS_UART_NUM, &cfg);
    uart_set_pin(GPS_UART_NUM, GPS_UART_TX, GPS_UART_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (xTaskCreatePinnedToCore(gps_task, "gps", 3072, NULL, 3, NULL, 1) != pdPASS) return ESP_FAIL;
    ESP_LOGI(TAG, "init OK (UART%d rx=%d tx=%d %d baud)", GPS_UART_NUM, GPS_UART_RX, GPS_UART_TX, GPS_UART_BAUD);
    return ESP_OK;
}

bool gps_service_get_fix(gps_fix_t* out)
{
    if (s_sim_on) { *out = s_sim; out->valid = true; out->simulated = true; out->age_s = 0; return true; }
    if (!s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    gps_fix_t f = s_fix;
    int64_t age = (esp_timer_get_time() - s_fix_time_us) / 1000000;
    xSemaphoreGive(s_lock);
    f.age_s = (age < 0 || age > UINT32_MAX) ? UINT32_MAX : (uint32_t)age;
    f.simulated = false;
    if (!f.valid || f.age_s > GPS_FIX_MAX_AGE_S) { if (out) { *out = f; out->valid = false; } return false; }
    if (out) *out = f;
    return true;
}

bool     gps_service_is_present(void) { return s_sentences > 0; }
uint32_t gps_service_sentences(void)  { return s_sentences; }

void gps_service_simulate(double lat, double lng, float hdop)
{
    s_sim = (gps_fix_t){ .valid = true, .lat = lat, .lng = lng,
                         .hdop = hdop > 0 ? hdop : 1.0f, .sats = 8, .speed_kmh = 0,
                         .age_s = 0, .simulated = true };
    s_sim_on = true;
    ESP_LOGW(TAG, "SIMULATED fix on: %.6f,%.6f hdop=%.1f", lat, lng, s_sim.hdop);
}
void gps_service_simulate_off(void) { s_sim_on = false; ESP_LOGW(TAG, "simulated fix off"); }

#endif /* TRACKER_FEATURES */
