#ifdef TRACKER_FEATURES
#include "vibration_service.h"
#include "../drivers/display/co5300_driver.h"
#include "ui_service.h"
#include <driver/gpio.h>
#include <esp_adc/adc_oneshot.h>
#include "../drivers/imu/icm42670_driver.h"
#include <stdio.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

static const char* TAG = "VibSvc";
#ifndef PIEZO_PIN
#define PIEZO_PIN -1
#endif
#define VIB_DEBOUNCE_US 30000     /* 30 ms: tek darbe tek sayılsın */
/* Ekransız tasma (NAR): dışarı verilen tek boş pinler ekranın QSPI veri
 * hatları (GPIO4-7). Ekran sürücüsü açılışta bu pinleri alır; piezo bu
 * pinlerden birindeyse GPIO kurulumu ekran init'i BİTTİKTEN sonra yapılır,
 * yoksa ekran init'i pini geri alır ve darbe hiç okunmaz. */
#if PIEZO_PIN >= 4 && PIEZO_PIN <= 7
#define PIEZO_SHARES_DISPLAY_PIN 1
#else
#define PIEZO_SHARES_DISPLAY_PIN 0
#endif
#define WINDOW_S 60

static volatile uint32_t s_total;
static volatile int64_t  s_last_us;
static uint32_t s_ring[WINDOW_S]; static uint8_t s_ri; static uint32_t s_last_total;
static uint16_t s_tpm;
static volatile bool s_attached;   /* pin dijital giriş olarak kuruldu mu (ekran init sonrası) */

#if PIEZO_PIN >= 0
static void IRAM_ATTR isr(void* arg)
{
    (void)arg;
    int64_t now = esp_timer_get_time();
    if (now - s_last_us < VIB_DEBOUNCE_US) return;
    s_last_us = now; s_total++;
}
#endif

#if PIEZO_PIN >= 0
static void attach_pin(void)
{
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << PIEZO_PIN; io.mode = GPIO_MODE_INPUT;
    io.pull_down_en = GPIO_PULLDOWN_ENABLE; io.intr_type = GPIO_INTR_POSEDGE;
    gpio_config(&io);
    gpio_isr_handler_add((gpio_num_t)PIEZO_PIN, isr, NULL);
    /* Pin devralinirken olusan tek kenar darbe sayilmasin */
    vTaskDelay(pdMS_TO_TICKS(50));
    s_total = 0; s_last_total = 0; s_last_us = 0;
    s_attached = true;
    ESP_LOGI(TAG, "piezo GPIO%d giris olarak kuruldu%s", PIEZO_PIN,
             PIEZO_SHARES_DISPLAY_PIN ? " (ekran D-hatti devralindi, ekransiz tasma)" : "");
}
#endif

static void task(void* arg)
{
    (void)arg;
#if PIEZO_PIN >= 0 && PIEZO_SHARES_DISPLAY_PIN
    /* Ekran init'ini bekle (UI task, Core 0). En fazla 20 sn; sonra yine kur. */
    for (int i = 0; i < 200 && !co5300_is_ready(); i++) vTaskDelay(pdMS_TO_TICKS(100));
    if (!co5300_is_ready()) ESP_LOGW(TAG, "ekran init bekleme asildi, pin yine de kuruluyor");
    vTaskDelay(pdMS_TO_TICKS(200));
    attach_pin();
#endif
    for (;;) {
        uint32_t t = s_total, d = t >= s_last_total ? t - s_last_total : 0; s_last_total = t;
        s_ring[s_ri] = d; s_ri = (s_ri + 1) % WINDOW_S;
        uint32_t sum = 0; for (int i = 0; i < WINDOW_S; i++) sum += s_ring[i];
        s_tpm = (uint16_t)(sum > 65535 ? 65535 : sum);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t vibration_service_init(void)
{
#if PIEZO_PIN >= 0
    /* ISR servisini burada (ana görev, geniş yığın) kur; attach_pin küçük görevden çağrılır */
    { esp_err_t e = gpio_install_isr_service(0); (void)e; /* ESP_ERR_INVALID_STATE = zaten kurulu */ }
#if !PIEZO_SHARES_DISPLAY_PIN
    attach_pin();
#endif
    memset(s_ring, 0, sizeof(s_ring));
    xTaskCreatePinnedToCore(task, "vib", 4096, NULL, 2, NULL, 1);   /* 2048 ekran-bekleme + attach ile tasti */
    ESP_LOGI(TAG, "init OK (piezo on GPIO%d, dijital darbe)", PIEZO_PIN);
#else
    ESP_LOGI(TAG, "piezo yok (PIEZO_PIN tanimsiz)");
#endif
    return ESP_OK;
}
bool     vibration_service_present(void)      { return PIEZO_PIN >= 0; }
esp_err_t vibration_service_capture(int seconds, int hz, void (*emit)(const char*), int pull)
{
#if PIEZO_PIN >= 0
    if (!emit) return ESP_ERR_INVALID_ARG;
    if (seconds < 1) seconds = 1;
    if (seconds > 60) seconds = 60;
    if (hz < 20) hz = 20;
    if (hz > 500) hz = 500;
    adc_unit_t unit; adc_channel_t ch;
    esp_err_t e = adc_oneshot_io_to_channel(PIEZO_PIN, &unit, &ch);
    if (e != ESP_OK) return e;
    if (unit != ADC_UNIT_1) return ESP_ERR_NOT_SUPPORTED;   /* ADC2 WiFi ile okunamaz */
    if (!s_attached) return ESP_ERR_INVALID_STATE;          /* ekran init bitmeden pin bizim degil */
    s_attached = false;
    gpio_isr_handler_remove((gpio_num_t)PIEZO_PIN);
    adc_oneshot_unit_handle_t h = NULL;
    adc_oneshot_unit_init_cfg_t uc = {}; uc.unit_id = unit;
    e = adc_oneshot_new_unit(&uc, &h);
    if (e != ESP_OK) { attach_pin(); return e; }
    adc_oneshot_chan_cfg_t cc = {}; cc.atten = ADC_ATTEN_DB_12; cc.bitwidth = ADC_BITWIDTH_DEFAULT;
    adc_oneshot_config_channel(h, ch, &cc);
    /* Deney: modül çıkışı VCC'ye yaslıysa (ADC tavanı 3.1 V) dahili pull-down (~45k)
     * modülün çıkış direnciyle bölücü oluşturup seviyeyi okunur aralığa çekebilir.
     * pull: 0 = yok, -1 = pull-down, +1 = pull-up */
    if (pull < 0) { gpio_pullup_dis((gpio_num_t)PIEZO_PIN); gpio_pulldown_en((gpio_num_t)PIEZO_PIN); }
    if (pull > 0) { gpio_pulldown_dis((gpio_num_t)PIEZO_PIN); gpio_pullup_en((gpio_num_t)PIEZO_PIN); }
    const int     total     = seconds * hz;
    const int64_t period_us = 1000000 / hz;
    /* Satır: "[cap] <t_ms> p1..p20 | ax ay az x5"  — piezo hz'de, ivme 50 Hz'de (her
     * hz/50 örnekte bir, mg cinsinden). İvme, Mac tarafında hareket referansı olarak
     * kullanılır (uyarlamalı hareket iptali + hareket kapısı). IMU yoksa '|' kısmı boş. */
    char line[320]; int n = 0, pos = 0;
    char acc[120]; int apos = 0;
    const int acc_every = hz >= 50 ? hz / 50 : 1;
    /* Ekran QSPI veri hatları (GPIO4/5/7) bu pinin hemen yanında; her kare (~9 Hz)
     * ADC'ye karışıyor. Kayıt boyunca LVGL kilidini tutarak ekran çizimini durdur. */
    ui_service_lock();
    int64_t t0 = esp_timer_get_time(), next = t0;
    for (int i = 0; i < total; i++) {
        while (esp_timer_get_time() < next) vTaskDelay(1);
        next += period_us;
        int raw = 0; adc_oneshot_read(h, ch, &raw);
        if (pos == 0) pos = snprintf(line, sizeof line, "[cap] %lld", (long long)((esp_timer_get_time() - t0) / 1000));
        pos += snprintf(line + pos, sizeof line - pos, " %d", raw); n++;
        if ((i % acc_every) == 0) {
            icm42670_data_t d;
            if (icm42670_read(&d) == ESP_OK && apos < (int)sizeof(acc) - 24)
                apos += snprintf(acc + apos, sizeof acc - apos, " %d %d %d",
                                 (int)(d.accel_x * 1000), (int)(d.accel_y * 1000), (int)(d.accel_z * 1000));
        }
        if (n == 20 || i == total - 1) {
            snprintf(line + pos, sizeof line - pos, " |%s", acc);
            emit(line); n = 0; pos = 0; apos = 0; acc[0] = 0;
        }
    }
    ui_service_unlock();
    adc_oneshot_del_unit(h);
    attach_pin();   /* dijital darbe girişine geri dön (ekran hattı zaten devralınmış) */
    return ESP_OK;
#else
    (void)seconds; (void)hz; (void)emit; (void)pull; return ESP_ERR_NOT_SUPPORTED;
#endif
}

uint32_t vibration_service_taps_total(void)   { return s_total; }
int      vibration_service_level(void)
{
#if PIEZO_PIN >= 0
    return gpio_get_level((gpio_num_t)PIEZO_PIN);
#else
    return -1;
#endif
}
uint16_t vibration_service_taps_per_min(void) { return s_tpm; }
uint32_t vibration_service_since_last_s(void)
{ return s_last_us ? (uint32_t)((esp_timer_get_time() - s_last_us) / 1000000) : UINT32_MAX; }
#endif /* TRACKER_FEATURES */
