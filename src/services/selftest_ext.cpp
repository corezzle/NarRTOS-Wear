/**
 * selftest_ext.cpp — ek donanım testleri. Bkz. selftest_ext.h.
 *
 * GÜVENLİK: PMIC rayları değiştirilmez (ALDO4 kapatılırsa I2C kilitlenir — Session 46l),
 * Wi-Fi yalnız PASİF taranır (verici kapalı), IMU'ya yalnız PWR_MGMT0/ACCEL_CONFIG0
 * yazılır (0x01 DEVICE_CONFIG'e ASLA — Session 46n).
 */
/* OS kopyası: narrtos-board-selftest/src/selftest_ext.cpp ile AYNI tutulur.
 * Yalnız NAR-ESP32S3 derlemesinde etkin (diğer kartlarda pinler farklı/denenmedi). */
#ifdef BOARD_VARIANT_NAR
#include "selftest_ext.h"
#include "../hal/i2c_bus.h"

#include <driver/gpio.h>
#include <driver/i2c.h>
#include <driver/i2s_tdm.h>
#include <driver/uart.h>
#include <esp_private/esp_gpio_reserve.h>   /* esp_gpio_revoke: 43/44 konsol UART0 rezervasyonunu birak */
#include <esp_rom_gpio.h>                    /* esp_rom_gpio_connect_out_signal */
#include <soc/gpio_sig_map.h>                /* SIG_GPIO_OUT_IDX */
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_chip_info.h>
#include <esp_efuse.h>
#include <esp_efuse_table.h>
#include <esp_event.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_psram.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs_flash.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "sdkconfig.h"

#if CONFIG_BT_NIMBLE_ENABLED
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#endif

/* ─── Pinler (NAR SiP = AMOLED 2.06 referans kartıyla aynı ses pinleri) ─────────── */
#define STX_PIN_SDA        15
#define STX_PIN_SCL        14
#define STX_PIN_I2S_MCLK   16
#define STX_PIN_I2S_BCLK   41
#define STX_PIN_I2S_WS     45
#define STX_PIN_I2S_DOUT   40   /* ESP → ES8311 DSDIN */
#define STX_PIN_I2S_DIN    42   /* ES7210 SDOUT → ESP */
#define STX_PIN_PA_EN      46   /* NS4150B CTRL */
#define STX_PIN_MOTOR      18
#define STX_PIN_TE         13
#define STX_PIN_GSM_PWR    47   /* SiP pin 13 -> TPS22965 yuk anahtari ON: HIGH = nRF9151'e 3V3 verilir */
#define STX_PIN_LTE_EN     48   /* SiP pin 42 -> nRF9151 ENABLE (SiP icinde 10K ile VDD'ye cekili) */
#define STX_PIN_U0TXD      43
#define STX_PIN_U0RXD      44

#define A_AXP     0x34
static uint8_t s_es8311_addr = 0x18;      /* CE pini HIGH ise 0x19 */
#define A_ES8311  s_es8311_addr
#define A_ES7210  0x40
#define A_ICM     0x68

volatile bool g_stx_uart_busy = false;

static stx_log_fn s_log = NULL;
void stx_set_logger(stx_log_fn fn) { s_log = fn; }
#define LOGD(...) do { if (s_log) s_log(__VA_ARGS__); } while (0)

static bool rd(uint8_t a, uint8_t r, uint8_t* o, size_t n = 1) { return i2c_bus_read_reg(a, r, o, n) == ESP_OK; }
static bool wr(uint8_t a, uint8_t r, uint8_t v) { return i2c_bus_write_reg(a, r, v) == ESP_OK; }

bool stx_is_nar(void) { return esp_psram_get_size() >= 16u * 1024u * 1024u; }

/* ─── I2C hat seviyesi ─────────────────────────────────────────────────────── */

stx_res_t stx_i2c_level(char* d, size_t n)
{
    adc_oneshot_unit_handle_t adc = NULL;
    adc_oneshot_unit_init_cfg_t ucfg = {};
    ucfg.unit_id = ADC_UNIT_2;
    if (adc_oneshot_new_unit(&ucfg, &adc) != ESP_OK) { snprintf(d, n, "ADC2 acilamadi"); return STX_SKIP; }

    adc_oneshot_chan_cfg_t ccfg = {};
    ccfg.atten = ADC_ATTEN_DB_12;
    ccfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    adc_oneshot_config_channel(adc, ADC_CHANNEL_4, &ccfg);   /* GPIO15 SDA */
    adc_oneshot_config_channel(adc, ADC_CHANNEL_3, &ccfg);   /* GPIO14 SCL */

    adc_cali_handle_t cal = NULL;
    adc_cali_curve_fitting_config_t cc = {};
    cc.unit_id = ADC_UNIT_2;
    cc.atten = ADC_ATTEN_DB_12;
    cc.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_cali_create_scheme_curve_fitting(&cc, &cal) != ESP_OK) cal = NULL;

    int sum[2] = {0, 0}, cnt = 0, rawmax = 0;
    for (int i = 0; i < 16; i++) {
        int raw[2] = {0, 0};
        if (adc_oneshot_read(adc, ADC_CHANNEL_4, &raw[0]) != ESP_OK) continue;
        if (adc_oneshot_read(adc, ADC_CHANNEL_3, &raw[1]) != ESP_OK) continue;
        for (int k = 0; k < 2; k++) {
            if (raw[k] > rawmax) rawmax = raw[k];
            int mv = raw[k] * 3100 / 4095;
            if (cal) adc_cali_raw_to_voltage(cal, raw[k], &mv);
            sum[k] += mv;
        }
        cnt++;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    if (cal) adc_cali_delete_scheme_curve_fitting(cal);
    adc_oneshot_del_unit(adc);
    /* ADC pinleri analoga aldı → I2C sürücüsünün pin ayarını geri kur */
    i2c_set_pin(I2C_NUM_0, STX_PIN_SDA, STX_PIN_SCL, true, true, I2C_MODE_MASTER);

    if (cnt == 0) { snprintf(d, n, "ADC okunamadi"); return STX_SKIP; }
    int sda = sum[0] / cnt, scl = sum[1] / cnt;
    int lo = sda < scl ? sda : scl;
    /* ADC ~3.1 V'da doyar: 2.9 V üstü = hat tam seviyede */
    /* Ham deger tavandaysa kalibrasyon egrisi anlamsiz buyuk mV uretebiliyor (kart 3: "4983 mV") */
    if (rawmax >= 4080) { snprintf(d, n, "SDA/SCL 3.1 V ustu (ADC tavanda) - bosta, saglikli"); return STX_PASS; }
    if (lo >= 2800) { snprintf(d, n, "SDA %d mV, SCL %d mV (bosta, saglikli)", sda, scl); return STX_PASS; }
    if (lo >= 2400) { snprintf(d, n, "SDA %d mV, SCL %d mV - dusuk (1.8V'luk cip hatti kirpiyor?)", sda, scl); return STX_WARN; }
    snprintf(d, n, "SDA %d mV, SCL %d mV - HAT KIRPILIYOR (beslemesiz cip? ALDO4?)", sda, scl);
    return STX_FAIL;
}

/* ─── Ses: ES8311 DAC → NS4150B → ES7210 (MIC3 geri besleme + MIC1/2 akustik) ── */

#define FS        16000
#define TONE_HZ   1000
#define NCH       4
#define CHUNK     160            /* 10 ms = tam 10 periyot → sürekli faz */

static i2s_chan_handle_t s_tx = NULL, s_rx = NULL;
static volatile bool s_tone_on = false, s_writer_run = false, s_writer_done = true;

/* Istege bagli test klibi (src/test_clip_data.c). Dosya projede YOKSA semboller NULL kalir ve
 * klip adimi atlanir (OS kopyasi ve ble/motor/lte projeleri boyle). */
extern "C" const int16_t* stx_clip_data(uint32_t* len) __attribute__((weak));
static const int16_t* s_clip = NULL;
static uint32_t       s_clip_len = 0;
#define STX_TONE_AMP       3000     /* olcum tonu: kisik (~ -20 dBFS) */
#define STX_CLIP_GAIN_PCT  100      /* klip seviyesi: EN YUKSEK. WAV zaten tam olcege (0.97) ve yumusak sinirlayiciyla hazirlandi;
                                     * daha fazlasi dijital kirpma (cizirti) yapar. Daha yuksek ses = amfi kazanci (donanim). */
static volatile bool     s_clip_on  = false;
static volatile uint32_t s_clip_pos = 0;
static bool clip_available(void) { if (!stx_clip_data) return false; s_clip = stx_clip_data(&s_clip_len); return s_clip && s_clip_len > 0; }

static void tone_writer(void* arg)
{
    (void)arg;
    static int16_t tone[CHUNK * NCH], zero[CHUNK * NCH];
    for (int i = 0; i < CHUNK; i++) {
        int16_t v = (int16_t)((float)STX_TONE_AMP * sinf(2.0f * (float)M_PI * TONE_HZ * i / FS));
        for (int c = 0; c < NCH; c++) { tone[i * NCH + c] = v; zero[i * NCH + c] = 0; }
    }
    size_t w = 0;
    static int16_t clipbuf[CHUNK * NCH];
    while (s_writer_run) {
        const int16_t* src = s_tone_on ? tone : zero;
        if (s_clip_on) {
            uint32_t pos = s_clip_pos, len = s_clip_len;
            for (int i = 0; i < CHUNK; i++) {
                int32_t v = (pos + i < len) ? ((int32_t)s_clip[pos + i] * STX_CLIP_GAIN_PCT) / 100 : 0;
                for (int c = 0; c < NCH; c++) clipbuf[i * NCH + c] = (int16_t)v;
            }
            s_clip_pos = pos + CHUNK;
            if (s_clip_pos >= len) s_clip_on = false;
            src = clipbuf;
        }
        i2s_channel_write(s_tx, src, sizeof(tone), &w, pdMS_TO_TICKS(200));
    }
    s_writer_done = true;
    vTaskDelete(NULL);
}

/* 1 kHz bileşeninin genliği (Goertzel), kanal başına */
static void tone_level(const int16_t* buf, int frames, float out[NCH])
{
    const float w = 2.0f * (float)M_PI * TONE_HZ / FS, cw = 2.0f * cosf(w);
    for (int c = 0; c < NCH; c++) {
        float s1 = 0, s2 = 0;
        for (int i = 0; i < frames; i++) { float s0 = buf[i * NCH + c] + cw * s1 - s2; s2 = s1; s1 = s0; }
        float p = s1 * s1 + s2 * s2 - cw * s1 * s2;
        out[c] = sqrtf(p < 0 ? 0 : p) * 2.0f / frames;
    }
}

static int s_raw_peak = 0;
static bool rx_measure(int16_t* buf, int frames, float out[NCH])
{
    size_t got = 0;
    /* ilk 100 ms'yi at (oturma), sonra ölç */
    i2s_channel_read(s_rx, buf, frames * NCH * 2, &got, pdMS_TO_TICKS(500));
    if (i2s_channel_read(s_rx, buf, frames * NCH * 2, &got, pdMS_TO_TICKS(500)) != ESP_OK || got < (size_t)frames * NCH * 2) return false;
    int pk[NCH] = {0};
    for (int i = 0; i < frames * NCH; i++) { int v = buf[i] < 0 ? -buf[i] : buf[i]; if (v > pk[i % NCH]) pk[i % NCH] = v; }
    LOGD("   ses: ham tepe K1 %d  K2 %d  K3 %d  K4 %d\n", pk[0], pk[1], pk[2], pk[3]);
    s_raw_peak = 0; for (int c = 0; c < NCH; c++) if (pk[c] > s_raw_peak) s_raw_peak = pk[c];
    tone_level(buf, frames, out);
    return true;
}

static bool es8311_setup(void)
{
    bool ok = wr(A_ES8311, 0x00, 0x1F);
    vTaskDelay(pdMS_TO_TICKS(20));
    ok &= wr(A_ES8311, 0x00, 0x00) && wr(A_ES8311, 0x00, 0x80);
    /* MCLK pinden 4.096 MHz, 16 kHz, 16 bit I2S slave (coeff: pre_div 1, bclk_div 4) */
    static const uint8_t seq[][2] = {
        {0x01, 0x3F}, {0x02, 0x00}, {0x03, 0x10}, {0x04, 0x10}, {0x05, 0x00}, {0x06, 0x03},
        {0x07, 0x00}, {0x08, 0xFF}, {0x09, 0x0C}, {0x0A, 0x0C}, {0x0D, 0x01}, {0x0E, 0x02},
        {0x12, 0x00}, {0x13, 0x10}, {0x1C, 0x6A}, {0x37, 0x08}, {0x32, 0xBF}, {0x31, 0x00},
    };
    for (const auto& s : seq) ok &= wr(A_ES8311, s[0], s[1]);
    return ok;
}

static bool es7210_setup(void)
{
    /* Espressif esp-bsp es7210.c dizisi: 16 kHz, MCLK 4.096 MHz, 16 bit, TDM (4 kanal tek hatta).
     * Kazanç: MIC1/MIC2 30 dB (0x0A), MIC3 geri besleme 0 dB, MIC4 0 dB. */
    static const uint8_t seq[][2] = {
        {0x00, 0xFF}, {0x00, 0x32}, {0x01, 0x00}, {0x09, 0x30}, {0x0A, 0x30},
        {0x23, 0x2A}, {0x22, 0x0A}, {0x21, 0x2A}, {0x20, 0x0A},
        {0x07, 0x20}, {0x02, 0xC1}, {0x04, 0x01}, {0x05, 0x00},
        {0x11, 0x60}, {0x12, 0x02},
        {0x40, 0x43}, {0x41, 0x70}, {0x42, 0x70},
        {0x43, 0x1A}, {0x44, 0x1A}, {0x45, 0x1A}, {0x46, 0x1A},
        {0x47, 0x08}, {0x48, 0x08}, {0x49, 0x08}, {0x4A, 0x08},
        {0x06, 0x00}, {0x4B, 0x00}, {0x4C, 0x00}, {0x00, 0x71}, {0x00, 0x41},   /* 4B/4C=00: MIC bias+ADC+PGA acik */
    };
    bool ok = true;
    for (const auto& s : seq) ok &= wr(A_ES7210, s[0], s[1]);
    return ok;
}

stx_res_t stx_audio(char* d_spk, size_t n_spk, stx_res_t* mic_res, char* d_mic, size_t n_mic)
{
    *mic_res = STX_SKIP;
    snprintf(d_mic, n_mic, "ses yolu kurulamadi");
    uint8_t v;
    s_es8311_addr = (!rd(0x18, 0xFD, &v) && rd(0x19, 0xFD, &v)) ? 0x19 : 0x18;
    if (!rd(A_ES8311, 0xFD, &v) || !rd(A_ES7210, 0x3D, &v)) {
        snprintf(d_spk, n_spk, "kodek/ADC cevap vermiyor (0x18|0x19 / 0x40)");
        return STX_SKIP;
    }

    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.auto_clear = true;
    if (i2s_new_channel(&cc, &s_tx, &s_rx) != ESP_OK) { snprintf(d_spk, n_spk, "I2S kanali acilamadi"); return STX_SKIP; }

    i2s_tdm_config_t tc = {};
    tc.clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(FS);
    tc.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    tc.slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO,
                   (i2s_tdm_slot_mask_t)(I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3));
    tc.gpio_cfg.mclk = (gpio_num_t)STX_PIN_I2S_MCLK;
    tc.gpio_cfg.bclk = (gpio_num_t)STX_PIN_I2S_BCLK;
    tc.gpio_cfg.ws   = (gpio_num_t)STX_PIN_I2S_WS;
    tc.gpio_cfg.dout = (gpio_num_t)STX_PIN_I2S_DOUT;
    tc.gpio_cfg.din  = (gpio_num_t)STX_PIN_I2S_DIN;

    stx_res_t res = STX_FAIL;
    const int FR = 1600;   /* 100 ms */
    int16_t* buf = (int16_t*)malloc(FR * NCH * 2);
    bool up = buf && i2s_channel_init_tdm_mode(s_tx, &tc) == ESP_OK && i2s_channel_init_tdm_mode(s_rx, &tc) == ESP_OK
              && i2s_channel_enable(s_tx) == ESP_OK && i2s_channel_enable(s_rx) == ESP_OK;
    bool writer = false;
    if (!up) {
        snprintf(d_spk, n_spk, "I2S baslatilamadi");
        res = STX_SKIP;
    } else {
        gpio_reset_pin((gpio_num_t)STX_PIN_PA_EN);
        gpio_set_direction((gpio_num_t)STX_PIN_PA_EN, GPIO_MODE_OUTPUT);
        gpio_set_level((gpio_num_t)STX_PIN_PA_EN, 0);

        bool c1 = es8311_setup(), c2 = es7210_setup();
        s_tone_on = false; s_writer_run = true; s_writer_done = false;
        writer = xTaskCreate(tone_writer, "stx_tone", 4096, NULL, 6, NULL) == pdPASS;
        if (!writer) s_writer_done = true;
        vTaskDelay(pdMS_TO_TICKS(150));

        float base[NCH] = {0}, on[NCH] = {0};
        bool m1 = writer && rx_measure(buf, FR, base);          /* sessizlik */
        gpio_set_level((gpio_num_t)STX_PIN_PA_EN, 1);
        vTaskDelay(pdMS_TO_TICKS(60));
        s_tone_on = true;
        vTaskDelay(pdMS_TO_TICKS(120));
        bool m2 = m1 && rx_measure(buf, FR, on);                 /* 1 kHz çalarken */
        s_tone_on = false;

        /* Olcumden sonra ozgun test klibi (varsa): kulakla kontrol icin ~10 sn */
        if (m2 && clip_available()) {
            LOGD("   ses: test klibi caliniyor (%.1f sn, seviye %%%d)...\n", s_clip_len / (float)FS, STX_CLIP_GAIN_PCT);
            vTaskDelay(pdMS_TO_TICKS(250));
            s_clip_pos = 0; s_clip_on = true;
            for (int i = 0; i < 1300 && s_clip_on; i++) vTaskDelay(pdMS_TO_TICKS(10));
            s_clip_on = false;
            vTaskDelay(pdMS_TO_TICKS(150));
        }
        gpio_set_level((gpio_num_t)STX_PIN_PA_EN, 0);

        { uint8_t r[8] = {0}; rd(A_ES7210, 0x00, &r[0]); rd(A_ES7210, 0x02, &r[1]); rd(A_ES7210, 0x06, &r[2]); rd(A_ES7210, 0x11, &r[3]);
          rd(A_ES7210, 0x12, &r[4]); rd(A_ES7210, 0x4B, &r[5]); rd(A_ES8311, 0x00, &r[6]); rd(A_ES8311, 0x0C, &r[7]);
          LOGD("   ses: ES7210 00=%02X 02=%02X 06=%02X 11=%02X 12=%02X 4B=%02X   ES8311 00=%02X 0C=%02X\n", r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]); }
        LOGD("   ses: kodek ayar %s/%s  1kHz genlik (sessiz -> ton)  K1 %.0f->%.0f  K2 %.0f->%.0f  K3 %.0f->%.0f  K4 %.0f->%.0f\n",
             c1 ? "ok" : "HATA", c2 ? "ok" : "HATA", base[0], on[0], base[1], on[1], base[2], on[2], base[3], on[3]);

        if (!m2) {
            snprintf(d_spk, n_spk, "I2S'ten veri gelmedi (SDOUT/GPIO%d?)", STX_PIN_I2S_DIN);
        } else {
            /* Ton "var": sessizliğe göre en az 8 kat ve mutlak eşik üstü */
            bool hit[NCH];
            for (int c = 0; c < NCH; c++) hit[c] = on[c] > 20.0f && on[c] > 8.0f * (base[c] + 1.5f);   /* kisik olcum tonuna gore */
            /* K3 = MIC3 = amfi çıkışının elektriksel geri beslemesi (hoparlör takılı olmasa da çalışır) */
            if (hit[2]) { snprintf(d_spk, n_spk, "DAC+amfi+ADC ok (geri besleme K3 %.0f)", on[2]); res = STX_PASS; }
            else if (hit[0] || hit[1]) { snprintf(d_spk, n_spk, "ses cikiyor (mikrofon duydu) ama K3 geri besleme yok (%.0f)", on[2]); res = STX_WARN; }
            else if (s_raw_peak < 12) snprintf(d_spk, n_spk, "ADC 4 kanalda da sessiz (tepe %d) - analog besleme A3V3 (SiP pin 38/53)?", s_raw_peak);
            else snprintf(d_spk, n_spk, "ton geri gelmedi (K3 %.0f) - ES8311 cikisi / geri besleme direncleri?", on[2]);

            int mics = (hit[0] ? 1 : 0) + (hit[1] ? 1 : 0);
            if (mics == 2)      { snprintf(d_mic, n_mic, "MIC1 %.0f, MIC2 %.0f - ikisi de tonu duydu", on[0], on[1]); *mic_res = STX_PASS; }
            else if (mics == 1) { snprintf(d_mic, n_mic, "yalniz MIC%d duydu (MIC1 %.0f, MIC2 %.0f)", hit[0] ? 1 : 2, on[0], on[1]); *mic_res = STX_WARN; }
            else if (res == STX_PASS) { snprintf(d_mic, n_mic, "ton duyulmadi (MIC1 %.0f, MIC2 %.0f) - hoparlor takili mi?", on[0], on[1]); *mic_res = STX_WARN; }
            else                { snprintf(d_mic, n_mic, "olculemedi (ses cikisi yok)"); *mic_res = STX_SKIP; }
        }
    }

    s_writer_run = false;
    for (int i = 0; i < 60 && !s_writer_done; i++) vTaskDelay(pdMS_TO_TICKS(10));
    if (up) { i2s_channel_disable(s_tx); i2s_channel_disable(s_rx); }
    i2s_del_channel(s_tx); i2s_del_channel(s_rx);
    s_tx = s_rx = NULL;
    free(buf);
    wr(A_ES8311, 0x31, 0x60);            /* DAC sessiz */
    wr(A_ES8311, 0x00, 0x1F);            /* kodek reset durumunda kalsın */
    wr(A_ES7210, 0x06, 0xFF);            /* ADC güç kapalı */
    return res;
}

/* ─── Wi-Fi pasif tarama ───────────────────────────────────────────────────── */

stx_res_t stx_wifi(char* d, size_t n, bool wifi_busy)
{
    if (wifi_busy) { snprintf(d, n, "Wi-Fi OS tarafindan kullaniliyor"); return STX_SKIP; }

    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) { nvs_flash_erase(); nvs_flash_init(); }
    esp_netif_init();
    e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) { snprintf(d, n, "event loop: %s", esp_err_to_name(e)); return STX_SKIP; }
    static esp_netif_t* s_sta = NULL;
    if (!s_sta) s_sta = esp_netif_create_default_wifi_sta();

    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    e = esp_wifi_init(&ic);
    if (e != ESP_OK) { snprintf(d, n, "esp_wifi_init: %s", esp_err_to_name(e)); return STX_FAIL; }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);
    stx_res_t res = STX_FAIL;
    if ((e = esp_wifi_start()) != ESP_OK) {
        snprintf(d, n, "esp_wifi_start: %s", esp_err_to_name(e));
    } else {
        wifi_scan_config_t sc = {};
        sc.show_hidden = true;
        sc.scan_type = WIFI_SCAN_TYPE_PASSIVE;      /* verici KAPALI — yalnız dinler */
        sc.scan_time.passive = 120;
        e = esp_wifi_scan_start(&sc, true);
        uint16_t cnt = 0;
        esp_wifi_scan_get_ap_num(&cnt);
        static wifi_ap_record_t rec[16];
        uint16_t m = 16;
        int best = -127;
        if (e == ESP_OK && esp_wifi_scan_get_ap_records(&m, rec) == ESP_OK)
            for (int i = 0; i < m; i++) if (rec[i].rssi > best) best = rec[i].rssi;
        esp_wifi_clear_ap_list();
        if (e != ESP_OK)      snprintf(d, n, "tarama: %s", esp_err_to_name(e));
        else if (cnt == 0)  { snprintf(d, n, "hic ag bulunamadi - anten takili mi?"); res = STX_WARN; }
        else if (best < -85){ snprintf(d, n, "%u ag, en iyi %d dBm - sinyal cok zayif (anten?)", cnt, best); res = STX_WARN; }
        else                { snprintf(d, n, "%u ag, en iyi %d dBm (pasif tarama)", cnt, best); res = STX_PASS; }
        esp_wifi_stop();
    }
    esp_wifi_deinit();
    return res;
}

/* ─── BLE pasif tarama ─────────────────────────────────────────────────────── */

#if CONFIG_BT_NIMBLE_ENABLED
static uint8_t  s_ble_seen[32][6];
static int      s_ble_n = 0, s_ble_best = -127;
static volatile bool s_ble_synced = false;

static int ble_gap_cb(struct ble_gap_event* ev, void* arg)
{
    (void)arg;
    if (ev->type != BLE_GAP_EVENT_DISC) return 0;
    if (ev->disc.rssi > s_ble_best) s_ble_best = ev->disc.rssi;
    for (int i = 0; i < s_ble_n; i++) if (!memcmp(s_ble_seen[i], ev->disc.addr.val, 6)) return 0;
    if (s_ble_n < 32) memcpy(s_ble_seen[s_ble_n++], ev->disc.addr.val, 6);
    return 0;
}
static void ble_on_sync(void) { s_ble_synced = true; }
static void ble_host_task(void* arg) { (void)arg; nimble_port_run(); nimble_port_freertos_deinit(); }
#endif

stx_res_t stx_ble(char* d, size_t n, bool ble_busy)
{
    if (ble_busy) { snprintf(d, n, "BLE OS tarafindan kullaniliyor"); return STX_SKIP; }
#if CONFIG_BT_NIMBLE_ENABLED
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) { nvs_flash_erase(); nvs_flash_init(); }
    s_ble_n = 0; s_ble_best = -127; s_ble_synced = false;
    e = nimble_port_init();
    if (e != ESP_OK) { snprintf(d, n, "BLE denetleyici acilamadi: %s", esp_err_to_name(e)); return STX_FAIL; }
    ble_hs_cfg.sync_cb = ble_on_sync;
    nimble_port_freertos_init(ble_host_task);
    for (int i = 0; i < 100 && !s_ble_synced; i++) vTaskDelay(pdMS_TO_TICKS(20));

    stx_res_t res = STX_FAIL;
    if (!s_ble_synced) {
        snprintf(d, n, "BLE yigini senkron olmadi");
    } else {
        uint8_t own = 0;
        ble_hs_id_infer_auto(0, &own);
        struct ble_gap_disc_params p = {};
        p.passive = 1;
        p.filter_duplicates = 0;
        int rc = ble_gap_disc(own, 3000, &p, ble_gap_cb, NULL);
        if (rc != 0) snprintf(d, n, "tarama baslatilamadi (rc=%d)", rc);
        else {
            vTaskDelay(pdMS_TO_TICKS(3200));
            ble_gap_disc_cancel();
            if (s_ble_n == 0) { snprintf(d, n, "3 sn'de hic BLE cihazi duyulmadi (ortam/anten?)"); res = STX_WARN; }
            else { snprintf(d, n, "%d%s cihaz, en iyi %d dBm (pasif tarama)", s_ble_n, s_ble_n >= 32 ? "+" : "", s_ble_best); res = STX_PASS; }
        }
    }
    if (nimble_port_stop() == 0) nimble_port_deinit();
    return res;
#else
    snprintf(d, n, "BLE bu derlemede kapali");
    return STX_SKIP;
#endif
}

/* ─── Titreşim motoru + IMU ────────────────────────────────────────────────── */

static bool imu_rms(float* out)
{
    /* 120 örnek, ~2.5 ms aralık; eksen başına ortalama çıkarılıp toplam RMS (mg) */
    float sx = 0, sy = 0, sz = 0, qx = 0, qy = 0, qz = 0;
    int cnt = 0;
    for (int i = 0; i < 120; i++) {
        uint8_t b[6];
        if (rd(A_ICM, 0x0B, b, 6)) {
            float x = (int16_t)((b[0] << 8) | b[1]), y = (int16_t)((b[2] << 8) | b[3]), z = (int16_t)((b[4] << 8) | b[5]);
            sx += x; sy += y; sz += z; qx += x * x; qy += y * y; qz += z * z; cnt++;
        }
        esp_rom_delay_us(1500);
        if ((i & 15) == 15) vTaskDelay(1);
    }
    if (cnt < 60) return false;
    float vx = qx / cnt - (sx / cnt) * (sx / cnt), vy = qy / cnt - (sy / cnt) * (sy / cnt), vz = qz / cnt - (sz / cnt) * (sz / cnt);
    float v = vx + vy + vz;
    *out = sqrtf(v < 0 ? 0 : v) * 1000.0f / 2048.0f;   /* ±16 g → 2048 LSB/g */
    return true;
}

stx_res_t stx_motor(char* d, size_t n)
{
    if (!stx_is_nar()) { snprintf(d, n, "bu kartta yok"); return STX_SKIP; }
    uint8_t who = 0;
    if (!rd(A_ICM, 0x75, &who) || who != 0x67) { snprintf(d, n, "IMU yok - titresim olculemiyor"); return STX_SKIP; }

    uint8_t pwr = 0, acfg = 0;
    rd(A_ICM, 0x1F, &pwr); rd(A_ICM, 0x21, &acfg);
    wr(A_ICM, 0x21, 0x06);          /* ±16 g, 800 Hz */
    wr(A_ICM, 0x1F, (uint8_t)((pwr & 0xFC) | 0x03));   /* accel Low Noise */
    vTaskDelay(pdMS_TO_TICKS(60));

    gpio_reset_pin((gpio_num_t)STX_PIN_MOTOR);
    gpio_set_direction((gpio_num_t)STX_PIN_MOTOR, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)STX_PIN_MOTOR, 0);
    vTaskDelay(pdMS_TO_TICKS(50));

    float off = 0, on = 0;
    bool ok = imu_rms(&off);
    gpio_set_level((gpio_num_t)STX_PIN_MOTOR, 1);
    vTaskDelay(pdMS_TO_TICKS(150));                    /* motor hızlansın */
    ok = ok && imu_rms(&on);
    gpio_set_level((gpio_num_t)STX_PIN_MOTOR, 0);

    wr(A_ICM, 0x21, acfg);
    wr(A_ICM, 0x1F, pwr);

    if (!ok) { snprintf(d, n, "IMU verisi okunamadi"); return STX_SKIP; }
    if (on > 25.0f && on > 3.0f * (off + 2.0f)) { snprintf(d, n, "titresim olculdu (%.0f -> %.0f mg)", off, on); return STX_PASS; }
    snprintf(d, n, "titresim yok (%.0f -> %.0f mg) - motor takili mi?", off, on);
    return STX_WARN;
}

/* ─── GSM modülü (şemada NAR-IOT-LTE) ───────────────────────────────────────────────────────────── */

static int lte_xfer(const char* cmd, char* out, size_t n, int wait_ms)
{
    uart_flush_input(UART_NUM_1);
    if (cmd) uart_write_bytes(UART_NUM_1, cmd, strlen(cmd));
    int len = 0;
    int64_t end = esp_timer_get_time() + (int64_t)wait_ms * 1000;
    while (esp_timer_get_time() < end && len < (int)n - 1) {
        int r = uart_read_bytes(UART_NUM_1, (uint8_t*)out + len, n - 1 - len, pdMS_TO_TICKS(50));
        if (r > 0) { len += r; out[len] = 0; if (strstr(out, "OK") || strstr(out, "ERROR")) break; }
    }
    out[len] = 0;
    return len;
}

static void printable(char* s)
{
    for (char* p = s; *p; p++) if (*p == '\r' || *p == '\n') *p = ' '; else if ((unsigned char)*p < 32 || (unsigned char)*p > 126) *p = '.';
}

/* 2026-11-10: GSM adımı AÇILIŞTA ÇALIŞTIRILMAZ (STX_GSM_ACILISTA 0).
 * Ölçüm (NAR numune kartı): GPIO47 ya da GPIO48 çıkış olarak yüksek sürüldüğü sürece PSRAM bit hatası veriyor
 * (boş PSRAM'de de, PSRAM'den çalışan kodda da; hata sayısı pinin sürüş gücüyle artıyor, pin düşük/boşta iken 0).
 * GPIO47/48 flaş + PSRAM ile aynı 1,8 V hattındadır (VDD_SPI). Bu adım pinleri 12 sn yüksek tuttuğu için her açılışta
 * bellekte rastgele bitler bozuluyordu (9 Kasım heap bozulması, 10 Kasım açılış çökmesi). Donanım tarafı çözülünce
 * 1 yapılır. */
#define STX_GSM_ACILISTA 0

stx_res_t stx_lte(char* d, size_t n)
{
    if (!stx_is_nar()) { snprintf(d, n, "bu kartta yok"); return STX_SKIP; }
    if (!STX_GSM_ACILISTA) { snprintf(d, n, "atlandi: GPIO47/48 surulunce PSRAM bozuluyor (10 Kas)"); return STX_SKIP; }

    /* Aciliş sirasi (NAR9151 ic semasi, Session 47l):
     *   1) GPIO47 -> SiP pin 13 -> TPS22965 yuk anahtari ON  => VCC3V3_nRF geliyor.
     *      BU OLMADAN nRF9151'in hic beslemesi yoktur; UART hatlari olu kalir.
     *   2) GPIO48 -> SiP pin 42 -> ENABLE (iceride 10K pull-up var, yine de surulur).
     *   3) RESET (SiP pin 35) CHIP_PU'ya bagli; ESP calisirken zaten yuksek. */
    /* ALDO1 = 3.3 V + ACIK (kullanici karari: GSM modulu simdilik ALDO1 ile aktif/pasif edilecek).
     * Test sonunda KAPATILMAZ: ALDO1 ses ciplerinin analog beslemesini (A3V3) de tasiyor. */
    { uint8_t en = 0; wr(A_AXP, 0x92, 0x1C); if (rd(A_AXP, 0x90, &en)) wr(A_AXP, 0x90, (uint8_t)(en | 0x01)); vTaskDelay(pdMS_TO_TICKS(20)); }

    gpio_reset_pin((gpio_num_t)STX_PIN_GSM_PWR);
    gpio_set_direction((gpio_num_t)STX_PIN_GSM_PWR, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)STX_PIN_GSM_PWR, 1);
    vTaskDelay(pdMS_TO_TICKS(50));                      /* ray otursun */

    gpio_reset_pin((gpio_num_t)STX_PIN_LTE_EN);
    gpio_set_direction((gpio_num_t)STX_PIN_LTE_EN, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)STX_PIN_LTE_EN, 1);

    /* Hat yönü: UART çıkışı boştayken HIGH sürülür. Pull-down ile bakınca HIGH okunan
     * hat modülün TX'idir. Şemadaki not (P0.11=TX ↔ U0TXD) çapraz bağlantı ihtimali
     * gösteriyor; hangi hat modül TX'iyse onu RX yaparız — iki çıkış asla karşı karşıya gelmez. */
    g_stx_uart_busy = true;
    static const int k_uart_pins[2] = { STX_PIN_U0TXD, STX_PIN_U0RXD };
    /* GPIO43/44 = ESP U0TXD/U0RXD. ROM ikincil cikisi U0TXD'yi GPIO43'e baglar ve push-pull HIGH surer;
     * bu hem 'hat canlandi' tespitini bozar hem nRF sinyalini bastirir. UART0'i pinlerden kopar. */
    for (int pin : k_uart_pins) {
        esp_rom_gpio_connect_out_signal((gpio_num_t)pin, SIG_GPIO_OUT_IDX, false, false);
        gpio_set_direction((gpio_num_t)pin, GPIO_MODE_INPUT);
        gpio_set_pull_mode((gpio_num_t)pin, GPIO_PULLDOWN_ONLY);
    }
    esp_gpio_revoke((1ULL << STX_PIN_U0TXD) | (1ULL << STX_PIN_U0RXD));
    /* nRF9151 acilisi (yuk anahtari + onyukleyici + uygulama) saniyeler surebilir: hat canlanana
     * kadar en fazla 12 sn beklenir. Bos UART cikisi HIGH surulur, o yuzden pull-down ile bakariz. */
    int lv43 = 0, lv44 = 0, wait_ms = 0;
    for (; wait_ms < 12000; wait_ms += 100) {
        lv43 = gpio_get_level((gpio_num_t)STX_PIN_U0TXD);
        lv44 = gpio_get_level((gpio_num_t)STX_PIN_U0RXD);
        if (lv43 || lv44) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    LOGD("   GSM: guc (GPIO%d) + ENABLE (GPIO%d) = 1, %d ms sonra hat seviyeleri: GPIO43=%d GPIO44=%d\n",
         STX_PIN_GSM_PWR, STX_PIN_LTE_EN, wait_ms, lv43, lv44);

    stx_res_t res = STX_WARN;
    if (!lv43 && !lv44) {
        snprintf(d, n, "12 sn'de hic cevap yok (GPIO43/44 LOW) - modul takili mi? yuk anahtari/lehim?");
    } else if (uart_driver_install(UART_NUM_1, 1024, 0, 0, NULL, 0) != ESP_OK) {
        snprintf(d, n, "UART1 acilamadi");
        res = STX_SKIP;
    } else {
        uart_config_t uc = {};
        uc.baud_rate = 115200; uc.data_bits = UART_DATA_8_BITS; uc.parity = UART_PARITY_DISABLE;
        uc.stop_bits = UART_STOP_BITS_1; uc.flow_ctrl = UART_HW_FLOWCTRL_DISABLE; uc.source_clk = UART_SCLK_DEFAULT;
        uart_param_config(UART_NUM_1, &uc);
        /* Calisan UART'ta iki hat da bosta HIGH (nRF P0.11 TX idle + P0.12 RX pull-up), o yuzden
         * seviyeye bakip yon secmek yaniltir. Once BILINEN dogru yonu dene: ESP TX=U0RXD(GPIO44)->nRF
         * P0.12 RX, ESP RX=U0TXD(GPIO43)<-nRF P0.11 TX. Olmazsa capraz yonu dener. */
        static const int dir[2][2] = { { STX_PIN_U0RXD, STX_PIN_U0TXD }, { STX_PIN_U0TXD, STX_PIN_U0RXD } };
        static char buf[160];
        int got = 0; bool ok = false, crossed = false;
        for (int k = 0; k < 2 && !ok; k++) {
            int tx = dir[k][0], rx = dir[k][1];
            esp_gpio_revoke((1ULL << tx) | (1ULL << rx));   /* 43/44 UART0 rezervasyonunu birak */
            uart_set_pin(UART_NUM_1, tx, rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
            buf[0] = 0; got = 0;
            for (int i = 0; i < 4 && !(got > 0 && strstr(buf, "OK")); i++) got = lte_xfer("AT\r\n", buf, sizeof(buf), 700);
            if (got > 0 && strstr(buf, "OK")) { ok = true; crossed = (k == 1); }
        }
        if (ok) {
            char imei[160] = "";
            lte_xfer("AT+CGSN\r\n", imei, sizeof(imei), 1000);
            printable(imei);
            char* p = imei; while (*p && (*p < '0' || *p > '9')) p++;
            char* q = p; while (*q >= '0' && *q <= '9') q++; *q = 0;
            snprintf(d, n, "AT -> OK%s%s%s", *p ? ", IMEI " : "", p, crossed ? "  (capraz yon)" : "");
            res = STX_PASS;
        } else if (got > 0) {
            printable(buf);
            snprintf(d, n, "cevap var ama OK yok: \"%.40s\"", buf);
        } else {
            snprintf(d, n, "hat canli ama AT'ye cevap yok - nRF9151'de SLM var mi?");
        }
        uart_driver_delete(UART_NUM_1);
    }

    /* Pinleri plain GPIO girisi birak (UART0'a GERI BAGLAMA: konsol native USB'de, U0TXD'yi 43'e
     * geri baglarsak nRF hattini yine bastirir). Modulu kapat. */
    esp_rom_gpio_connect_out_signal((gpio_num_t)STX_PIN_U0TXD, SIG_GPIO_OUT_IDX, false, false);
    esp_rom_gpio_connect_out_signal((gpio_num_t)STX_PIN_U0RXD, SIG_GPIO_OUT_IDX, false, false);
    gpio_set_direction((gpio_num_t)STX_PIN_U0TXD, GPIO_MODE_INPUT);
    gpio_set_direction((gpio_num_t)STX_PIN_U0RXD, GPIO_MODE_INPUT);
    gpio_set_level((gpio_num_t)STX_PIN_LTE_EN, 0);
    gpio_set_level((gpio_num_t)STX_PIN_GSM_PWR, 0);     /* yuk anahtarini da kapat: test iz birakmasin */
    vTaskDelay(pdMS_TO_TICKS(50));
    g_stx_uart_busy = false;
    return res;
}

/* ─── Panel TE darbesi ─────────────────────────────────────────────────────── */

int stx_te_pulses(void)
{
    gpio_set_direction((gpio_num_t)STX_PIN_TE, GPIO_MODE_INPUT);
    gpio_set_pull_mode((gpio_num_t)STX_PIN_TE, GPIO_PULLDOWN_ONLY);
    int edges = 0, last = gpio_get_level((gpio_num_t)STX_PIN_TE);
    int64_t end = esp_timer_get_time() + 300000;
    int spin = 0;
    while (esp_timer_get_time() < end) {
        int l = gpio_get_level((gpio_num_t)STX_PIN_TE);
        if (l && !last) edges++;
        last = l;
        esp_rom_delay_us(100);
        if (++spin >= 500) { spin = 0; vTaskDelay(1); }
    }
    gpio_set_pull_mode((gpio_num_t)STX_PIN_TE, GPIO_FLOATING);
    return edges;
}

/* ─── Bilgi satırı ─────────────────────────────────────────────────────────── */

stx_res_t stx_info(char* d, size_t n)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    bool force = esp_efuse_read_field_bit(ESP_EFUSE_VDD_SPI_FORCE);
    bool tieh  = esp_efuse_read_field_bit(ESP_EFUSE_VDD_SPI_TIEH);

    LOGD("   cip: ESP32-S3 rev v%d.%d, %d cekirdek   flash/PSRAM voltaji (eFuse): %s\n",
         ci.revision / 100, ci.revision % 100, ci.cores, !force ? "pin ile (GPIO45)" : tieh ? "3.3 V" : "1.8 V");

    uint8_t on = 0, off = 0;
    if (rd(A_AXP, 0x20, &on) && rd(A_AXP, 0x21, &off)) {
        LOGD("   PMIC acilis nedeni 0x20=%02X:%s%s%s%s%s   son kapanma 0x21=%02X:%s%s%s%s%s%s%s\n", on,
             (on & 0x01) ? " PWRON-tus" : "", (on & 0x02) ? " IRQ" : "", (on & 0x04) ? " VBUS-takildi" : "",
             (on & 0x08) ? " pil-sarj" : "", (on & 0x10) ? " pil-takildi" : "", off,
             (off & 0x01) ? " tus-uzun" : "", (off & 0x02) ? " yazilim" : "", (off & 0x08) ? " VSYS-dusuk" : "",
             (off & 0x10) ? " VBUS-asiri-voltaj" : "", (off & 0x20) ? " DCDC-dusuk-voltaj" : "",
             (off & 0x40) ? " DCDC-asiri-voltaj" : "", (off & 0x80) ? " asiri-sicak" : "");
    }

    /* 12/24V algılama şemada GPIO35'te — AMA ESP32-S3R16V'de GPIO33–37 çipin kendi octal
     * PSRAM hattıdır; pine dokunmak PSRAM'i bozup kartı çökertir (denendi). Okunmaz. */
    const char* vin = "";
    snprintf(d, n, "%s  MAC %02X:%02X:%02X:%02X:%02X:%02X  rev v%d.%d%s", stx_is_nar() ? "NAR-ESP32S3" : "AMOLED 2.06",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], ci.revision / 100, ci.revision % 100, vin);
    return STX_PASS;
}

#endif /* BOARD_VARIANT_NAR */
