/**
 * @file mic_service.cpp
 * @brief ES7210 → I2S ham mikrofon yakalama (bkz. header). Kurulum dizisi selftest_ext ile aynı.
 */
#ifdef TRACKER_FEATURES
#include "mic_service.h"
#include "mic_sd.h"
#include "command_service.h"
#include <time.h>
#include "../hal/i2c_bus.h"
#include "../core/os_config.h"
#include "../drivers/imu/icm42670_driver.h"
#include "nabiz_alg.h"

#include <driver/i2s_tdm.h>
#include <driver/i2s_std.h>
#include <driver/gpio.h>
#include <driver/pulse_cnt.h>
#include <driver/rtc_io.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <rom/ets_sys.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/usb_serial_jtag.h>
#include <esp_heap_caps.h>
#include <stdio.h>
#include <math.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static const char* TAG = "MicSvc";

#define MIC_FS_HW     16000
#define MIC_NCH       4
#define MIC_CHUNK     160          /* 10 ms */
#define MIC_PIN_MCLK  16
#define MIC_PIN_BCLK  41
#define MIC_PIN_WS    45
#define MIC_PIN_DIN   42
#define A_ES7210      0x40

static int s_variant = 0;
void mic_service_set_variant(int v) { s_variant = v; }

static bool es7210_setup(void)
{
    if (s_variant == 1 || s_variant == 2) {
        /* esp-bsp components/es7210/es7210.c es7210_config_codec sırası (16 kHz, 16 bit, TDM, 30 dB). */
        const uint8_t pw = s_variant == 1 ? 0x0F : 0x00;
        const uint8_t seq[][2] = {
            {0x00, 0xFF}, {0x00, 0x32}, {0x09, 0x30}, {0x0A, 0x30},
            {0x23, 0x2A}, {0x22, 0x0A}, {0x21, 0x2A}, {0x20, 0x0A},
            {0x11, 0x60}, {0x12, 0x02},
            {0x40, 0xC3}, {0x41, 0x70}, {0x42, 0x70},
            {0x43, 0x1A}, {0x44, 0x1A}, {0x45, 0x1A}, {0x46, 0x1A},
            {0x47, 0x08}, {0x48, 0x08}, {0x49, 0x08}, {0x4A, 0x08},
            {0x07, 0x20}, {0x02, 0xC1}, {0x04, 0x01}, {0x05, 0x00}, {0x06, 0x04},
            {0x4B, pw}, {0x4C, pw}, {0x00, 0x71}, {0x00, 0x41},
        };
        bool ok = true;
        for (const auto& r : seq) ok &= (i2c_bus_write_reg(A_ES7210, r[0], r[1]) == ESP_OK);
        return ok;
    }
    if (s_variant == 3) {
        /* 2026-10-30 steteskop denemesi: HPF register'ları (0x20-0x23) sıfır, MIC1/MIC2 PGA
         * en yüksek (0x1E). Kalp sesleri 20-150 Hz; varsayılan HPF/kazanç bu bandı kısabilir.
         * Yalnız deneme; sonuç iyi olursa ayrı bir 'mic lowfreq' komutu yapılır. */
        static const uint8_t seq[][2] = {
            {0x00, 0xFF}, {0x00, 0x32}, {0x01, 0x00}, {0x09, 0x30}, {0x0A, 0x30},
            {0x23, 0x00}, {0x22, 0x00}, {0x21, 0x00}, {0x20, 0x00},
            {0x07, 0x20}, {0x02, 0xC1}, {0x04, 0x01}, {0x05, 0x00},
            {0x11, 0x60}, {0x12, 0x02},
            {0x40, 0xC3}, {0x41, 0x70}, {0x42, 0x70},
            {0x43, 0x1E}, {0x44, 0x1E}, {0x45, 0x1E}, {0x46, 0x1E},
            {0x47, 0x08}, {0x48, 0x08}, {0x49, 0x08}, {0x4A, 0x08},
            {0x06, 0x00}, {0x4B, 0x00}, {0x4C, 0x00}, {0x00, 0x71}, {0x00, 0x41},
        };
        bool ok = true;
        for (const auto& s : seq) ok &= (i2c_bus_write_reg(A_ES7210, s[0], s[1]) == ESP_OK);
        return ok;
    }
    /* Espressif esp-bsp es7210.c dizisi: 16 kHz, MCLK 4.096 MHz, 16 bit, TDM 4 kanal.
     * Kazanç: MIC1/MIC2 30 dB (0x0A), MIC3 geri besleme 0 dB, MIC4 0 dB. */
    static const uint8_t seq[][2] = {
        {0x00, 0xFF}, {0x00, 0x32}, {0x01, 0x00}, {0x09, 0x30}, {0x0A, 0x30},
        {0x23, 0x2A}, {0x22, 0x0A}, {0x21, 0x2A}, {0x20, 0x0A},
        {0x07, 0x20}, {0x02, 0xC1}, {0x04, 0x01}, {0x05, 0x00},
        {0x11, 0x60}, {0x12, 0x02},
        /* 2026-10-30 (Emin ölçümü): 0x40=0x43 ile MICBIAS12 (TP12) 0 V kalıyor,
         * 0xC3 ile 2,8 V. Bit7 geri okumada 0 görünür (yazılabilir, okunmaz). */
        {0x40, 0xC3}, {0x41, 0x70}, {0x42, 0x70},
        /* 2026-11-02: PGA 0x1A (30 dB) → 0x1E (37,5 dB, üst sınır). Mikrofon düşük seviyeli,
         * sessiz odada ham tepe ~100/32767; analog kazanç SNR'yi iyileştirir, kırpılma riski yok. */
        {0x43, 0x1E}, {0x44, 0x1E}, {0x45, 0x1E}, {0x46, 0x1E},
        {0x47, 0x08}, {0x48, 0x08}, {0x49, 0x08}, {0x4A, 0x08},
        {0x06, 0x00}, {0x4B, 0x00}, {0x4C, 0x00}, {0x00, 0x71}, {0x00, 0x41},
    };
    bool ok = true;
    for (const auto& s : seq) ok &= (i2c_bus_write_reg(A_ES7210, s[0], s[1]) == ESP_OK);
    return ok;
}

esp_err_t mic_service_capture(int seconds, int fs, int channel, void (*emit)(const char*))
{
    if (!emit) return ESP_ERR_INVALID_ARG;
    if (seconds < 1) seconds = 1;
    if (seconds > 60) seconds = 60;
    if (fs < 100) fs = 100;
    if (fs > MIC_FS_HW) fs = MIC_FS_HW;
    int div = MIC_FS_HW / fs;                            /* 1 → 16 kHz (ortalama yok), 4 → 4 kHz, 16 → 1 kHz */
    fs = MIC_FS_HW / div;
    if (channel < 1 || channel > MIC_NCH) channel = 1;
    const int ch = channel - 1;

    uint8_t id = 0;
    /* Kodek cevap vermese de devam: saatler (MCLK/BCLK/WS) yine üretilir, kullanıcı pinlerde ölçebilir. */
    bool codec_ok = (i2c_bus_read_reg(A_ES7210, 0x3D, &id, 1) == ESP_OK);
    if (!codec_ok) ESP_LOGW(TAG, "ES7210 (0x40) cevap vermiyor — yalniz saatler uretilecek (veri 0 beklenir)");

    /* Teşhis: SDOUT hattı (DIN) sürülüyor mu, boşta mı? I2S kurulmadan önce pini girişe alıp
     * pull-up ve pull-down ile oku: pu=1 pd=0 → hat boşta (bağlı değil / üç durumlu);
     * ikisi de 0 → ES7210 hattı LOW tutuyor; ikisi de 1 → HIGH tutuyor. */
    {
        gpio_reset_pin((gpio_num_t)MIC_PIN_DIN);
        gpio_set_direction((gpio_num_t)MIC_PIN_DIN, GPIO_MODE_INPUT);
        gpio_set_pull_mode((gpio_num_t)MIC_PIN_DIN, GPIO_PULLUP_ONLY);   vTaskDelay(pdMS_TO_TICKS(5));
        int pu = gpio_get_level((gpio_num_t)MIC_PIN_DIN);
        gpio_set_pull_mode((gpio_num_t)MIC_PIN_DIN, GPIO_PULLDOWN_ONLY); vTaskDelay(pdMS_TO_TICKS(5));
        int pd = gpio_get_level((gpio_num_t)MIC_PIN_DIN);
        gpio_set_pull_mode((gpio_num_t)MIC_PIN_DIN, GPIO_FLOATING);
        ESP_LOGI(TAG, "DIN GPIO%d: pull-up=%d pull-down=%d (%s)", MIC_PIN_DIN, pu, pd,
                 (pu && !pd) ? "hat BOSTA - surulmuyor" : (!pu && !pd) ? "LOW tutuluyor" : (pu && pd) ? "HIGH tutuluyor" : "?");
    }

    i2s_chan_handle_t rx = NULL;
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.auto_clear = true;
    esp_err_t e = i2s_new_channel(&cc, NULL, &rx);
    if (e != ESP_OK) { ESP_LOGE(TAG, "i2s_new_channel: %s", esp_err_to_name(e)); return e; }

    i2s_tdm_config_t tc = {};
    tc.clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(MIC_FS_HW);
    tc.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    tc.slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO,
                   (i2s_tdm_slot_mask_t)(I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3));
    tc.gpio_cfg.mclk = (gpio_num_t)MIC_PIN_MCLK;
    tc.gpio_cfg.bclk = (gpio_num_t)MIC_PIN_BCLK;
    tc.gpio_cfg.ws   = (gpio_num_t)MIC_PIN_WS;
    tc.gpio_cfg.dout = I2S_GPIO_UNUSED;
    tc.gpio_cfg.din  = (gpio_num_t)MIC_PIN_DIN;
    e = i2s_channel_init_tdm_mode(rx, &tc);
    if (e == ESP_OK) e = i2s_channel_enable(rx);
    if (e != ESP_OK) { ESP_LOGE(TAG, "I2S kurulamadi: %s", esp_err_to_name(e)); i2s_del_channel(rx); return e; }

    bool cfg = es7210_setup();
    ESP_LOGI(TAG, "capture %d sn, kanal %d, %d Hz (/%d), ES7210 %s, varyant %d", seconds, channel, fs, div, cfg ? "ok" : "AYAR HATASI", s_variant);
    {
        static const uint8_t regs[] = {0x00, 0x01, 0x02, 0x04, 0x05, 0x06, 0x07, 0x08, 0x11, 0x12, 0x40, 0x41, 0x43, 0x47, 0x4B, 0x4C};
        char rb[160]; int rp = 0;
        for (size_t i = 0; i < sizeof regs; i++) { uint8_t v = 0xEE; i2c_bus_read_reg(A_ES7210, regs[i], &v, 1);
            rp += snprintf(rb + rp, sizeof rb - rp, " %02X=%02X", regs[i], v); }
        ESP_LOGI(TAG, "ES7210 geri okuma:%s", rb);
    }

    int16_t* buf = (int16_t*)malloc(MIC_CHUNK * MIC_NCH * sizeof(int16_t));
    if (!buf) { i2s_channel_disable(rx); i2s_del_channel(rx); return ESP_ERR_NO_MEM; }

    /* ilk 200 ms'yi at: kodek/filtre oturma */
    size_t got = 0;
    for (int i = 0; i < 20; i++) i2s_channel_read(rx, buf, MIC_CHUNK * MIC_NCH * 2, &got, pdMS_TO_TICKS(200));

    /* Teşhis: dört slotun HAM min/max'ı (DC çıkarılmadan). Hepsi aynı sabitse (0 ya da -1)
     * SDOUT hattı sallanmıyor demektir; sabit ama sıfır dışı DC varsa ADC çalışıyor, AC yok. */
    {
        int32_t mn[MIC_NCH], mx[MIC_NCH];
        for (int c = 0; c < MIC_NCH; c++) { mn[c] = 32767; mx[c] = -32768; }
        if (i2s_channel_read(rx, buf, MIC_CHUNK * MIC_NCH * 2, &got, pdMS_TO_TICKS(500)) == ESP_OK)
            for (int i = 0; i < MIC_CHUNK; i++) for (int c = 0; c < MIC_NCH; c++) {
                int32_t v = buf[i * MIC_NCH + c]; if (v < mn[c]) mn[c] = v; if (v > mx[c]) mx[c] = v; }
        ESP_LOGI(TAG, "ham slot min..max: k1 %ld..%ld  k2 %ld..%ld  k3 %ld..%ld  k4 %ld..%ld",
                 (long)mn[0], (long)mx[0], (long)mn[1], (long)mx[1], (long)mn[2], (long)mx[2], (long)mn[3], (long)mx[3]);
    }

    char line[256]; int pos = 0, n = 0;
    int64_t t0 = esp_timer_get_time();
    const int total_out = seconds * fs;
    int out_cnt = 0; int32_t acc = 0; int acc_n = 0;
    float dc = 0.0f; bool dc_init = false;
    int chunks = seconds * (MIC_FS_HW / MIC_CHUNK);
    for (int c = 0; c < chunks && out_cnt < total_out; c++) {
        if (i2s_channel_read(rx, buf, MIC_CHUNK * MIC_NCH * 2, &got, pdMS_TO_TICKS(500)) != ESP_OK) {
            ESP_LOGW(TAG, "i2s_channel_read zaman asimi"); break;
        }
        for (int i = 0; i < MIC_CHUNK; i++) {
            int32_t v = buf[i * MIC_NCH + ch];
            if (!dc_init) { dc = (float)v; dc_init = true; }
            dc += ((float)v - dc) * 0.0005f;            /* ~0.1 s'lik DC izleyici (yavaş) */
            acc += (int32_t)((float)v - dc); acc_n++;
            if (acc_n == div) {
                int32_t o = acc / div; acc = 0; acc_n = 0;
                if (pos == 0) pos = snprintf(line, sizeof line, "[mic] %lld", (long long)((esp_timer_get_time() - t0) / 1000));
                pos += snprintf(line + pos, sizeof line - pos, " %ld", (long)o); n++; out_cnt++;
                if (n == 20 || out_cnt >= total_out) { emit(line); n = 0; pos = 0; }
                if (out_cnt >= total_out) break;
            }
        }
    }
    if (n) emit(line);
    free(buf);
    i2s_channel_disable(rx);
    i2s_del_channel(rx);
    return ESP_OK;
}

/* ── 48 kHz ikili yakalama (2026-11-06) ─────────────────────────────────────
 * Metin akışı 16 kHz'de bile satır kaçırıyordu. I2S okuyucu görev PSRAM halkasına yazar, komut görevi
 * çerçeveleri USB'ye basar: "NARA" + seq(u32) + len(u16) + tasma(u16) + toplam(u32) + veri.
 * ES7210 bölenleri 16 kHz/4,096 MHz ile aynı oran: MCLK 256×fs = 12,288 MHz. */
#define RAW_FS      48000
#define RAW_FRAMES  120      /* 8 tanım × 120 kare: 24 bitte 15 kB DMA, 20 ms; 240 kare dahili belleğe sığmadı */
#define RAW_PAYLOAD 960      /* 2 ve 3 bayta tam bölünür */
#define RAW_RING    (1024 * 1024)

typedef struct {
    i2s_chan_handle_t rx;
    uint8_t* ring;
    volatile uint32_t head, tail, over;
    volatile bool done;
    uint32_t frames;
    int ch, bytes;
    TaskHandle_t waiter;
    uint32_t reader_stack_left;   /* okuyucu görevin bitişte kalan yığını (bayt) */
} raw_ctx_t;

static void raw_reader_task(void* arg)
{
    raw_ctx_t* c = (raw_ctx_t*)arg;
    const int slot = c->bytes == 2 ? 2 : 4;
    const size_t sz = RAW_FRAMES * MIC_NCH * slot;
    uint8_t* b = (uint8_t*)heap_caps_malloc(sz, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t got = 0;
    if (b) {
        for (int i = 0; i < 80; i++) i2s_channel_read(c->rx, b, sz, &got, pdMS_TO_TICKS(200));   /* 200 ms oturma */
        uint32_t left = c->frames;
        while (left > 0 && i2s_channel_read(c->rx, b, sz, &got, pdMS_TO_TICKS(500)) == ESP_OK) {
            uint32_t n = got / (MIC_NCH * slot);
            if (n > left) n = left;
            for (uint32_t i = 0; i < n; i++) {
                uint8_t s[3]; int w = c->bytes;
                if (slot == 2) { memcpy(s, b + (i * MIC_NCH + c->ch) * 2, 2); }
                else           { memcpy(s, b + (i * MIC_NCH + c->ch) * 4 + 1, 3); }   /* 32 bitin üst 24'ü */
                if (c->head - c->tail + w > RAW_RING) { c->over = c->over + 1; continue; }
                for (int k = 0; k < w; k++) c->ring[(c->head + k) % RAW_RING] = s[k];
                c->head = c->head + w;
            }
            left -= n;
        }
        free(b);
    }
    c->reader_stack_left = (uint32_t)uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t);
    c->done = true;
    xTaskNotifyGive(c->waiter);
    for (;;) vTaskSuspend(NULL);   /* statik görev: raw_run bizi siler, sonra yığını serbest bırakır */
}

/* ── Kartta nabız + solunum kestirimi — alg A2 (2026-11-10) ───────────────────────────────────────
 * Algoritmanın kendisi nabiz_alg.c'de (saf C): tür profili, otokorelasyon + vuruş sayımı, kat hatası düzeltmesi,
 * mırlama (kedi) ve soluma (köpek) tespiti, solunum sayısı. Aynı kaynak bilgisayarda derlenip bağımsız Python
 * referansıyla (tools/nabiz_ref.py) karşılaştırılır: tools/nabiz_test.py. Burada yalnız akışa bağlama, ivmeölçerle
 * hareket ölçümü ve sonuç satırları var. Eşikler başlangıç değerleridir; etiketli hayvan kayıtlarıyla kesinleşir. */
#define NABIZ_HAREKET_MG 30.0f      /* üstü uyarı (kart hayvanın üzerindeyken anlamlı; masadayken ~0) */

int         mic_service_profil_bul(const char* ad) { return nabiz_profil_bul(ad); }
int         mic_service_profil_sure(int p)         { return nabiz_profil(p)->sn; }
const char* mic_service_profil_ad(int p)           { return nabiz_profil(p)->ad; }

/* Kayıt etiketi (tür/hayvan/yer/durum; serbest metin): LISTE.TXT'ye ve sonuç satırına yazılır. `mic etiket <metin>`. */
static char s_etiket[64] = "";
void mic_service_set_label(const char* t)
{
    size_t k = 0;
    if (t) { while (*t == ' ' || *t == '\t') t++; for (; *t && *t != '\r' && *t != '\n' && k < sizeof s_etiket - 1; t++) s_etiket[k++] = *t; }
    while (k > 0 && s_etiket[k - 1] == ' ') k--;
    s_etiket[k] = '\0';
}
const char* mic_service_get_label(void) { return s_etiket; }

/* nabiz_alg ortamı: büyük diziler PSRAM'den; uzun hesapta en çok 200 ms'de bir 1 tik bırak (görev bekçisi için) */
static void* nabiz_al(size_t n)  { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
static void  nabiz_birak(void* p) { free(p); }
static void  nabiz_nefes(void)
{
    static int64_t son = 0;
    int64_t t = esp_timer_get_time();
    if (t - son > 200000) { vTaskDelay(1); son = esp_timer_get_time(); }
}

typedef struct {
    nabiz_akis_t akis;
    double hs[3], hss[3]; uint32_t h_n; int64_t h_son;    /* hareket: ivme (g) toplam / kare toplam, 10 Hz */
    bool raporlandi;
} bpm_t;
static bool bpm_init(bpm_t* a, int seconds, int bytes, const nabiz_profil_t* p) {
    memset(a, 0, sizeof *a);
    const nabiz_ortam_t o = { nabiz_al, nabiz_birak, nabiz_nefes };
    return nabiz_akis_baslat(&a->akis, seconds, bytes, p, &o);
}
static void bpm_bitir(bpm_t* a) { nabiz_akis_bitir(&a->akis); }
static void bpm_feed(bpm_t* a, const uint8_t* d, uint32_t len, int bytes) { (void)bytes; nabiz_akis_besle(&a->akis, d, len); }
/* İvmeölçerden 10 Hz örnek (I2C kilitli tek okuma). IMU yoksa sessizce atlanır. */
static void bpm_hareket(bpm_t* a) {
    int64_t t = esp_timer_get_time();
    if (t - a->h_son < 100000) return;
    a->h_son = t;
    icm42670_data_t d;
    if (icm42670_read(&d) != ESP_OK) return;
    const float v[3] = { d.accel_x, d.accel_y, d.accel_z };
    for (int k = 0; k < 3; k++) { a->hs[k] += v[k]; a->hss[k] += (double)v[k] * v[k]; }
    a->h_n++;
}
static float s_last_bpm = 0, s_last_r = 0;
float mic_service_last_bpm(float* r) { if (r) *r = s_last_r; return s_last_bpm; }

/* Analiz + sonuç satırları + BLE "son sonuç" + LISTE.TXT notu. sdname: yazılan WAV adı ya da "yok". */
static void bpm_report(bpm_t* a, void (*emit)(const char*), const char* sdname, char* note, size_t note_n) {
    char l[512];   /* A3: sonuc satırı ~45 karakter uzadı (pen_n/pen_iyi/pen_uyan/r2t) */
    nabiz_sonuc_t* s = (nabiz_sonuc_t*)nabiz_al(sizeof *s);
    if (!s || !nabiz_analiz(&a->akis, s)) {
        emit("[mic] nabiz: hesaplanamadi (analiz icin bellek yok)");
        command_service_set_result("nabiz hesaplanamadi (bellek)");
        free(s); return;
    }
    const nabiz_profil_t* p = s->p;
    float hareket = 0;
    if (a->h_n >= 2) {
        double var = 0;
        for (int k = 0; k < 3; k++) { double mu = a->hs[k] / a->h_n; double vk = a->hss[k] / a->h_n - mu * mu; if (vk > 0) var += vk; }
        hareket = 1000.0f * sqrtf((float)var);
    }
    const float doygun_yuzde = s->n_kalite ? 100.0f * (float)s->doygun / (float)s->n_kalite : 0.0f;
    const bool ok = strcmp(s->durum, "OK") == 0, vurus = strcmp(s->yontem, "vurus") == 0;
    /* A3: "pencere" ve "tutarli" yollarından gelen sonuç DÜŞÜK GÜVENLİDİR (bkz. nabiz_ref.py başındaki açıklama) */
    const bool pencere = strcmp(s->yontem, "pencere") == 0, tutarli = strcmp(s->yontem, "tutarli") == 0;
    const char* guven = (pencere || tutarli) ? "dusuk" : (vurus ? "orta" : (s->r >= 0.7f ? "yuksek" : "orta"));
    s_last_bpm = ok ? s->nabiz : 0; s_last_r = s->r;

    /* 1) nabız */
    if (ok && vurus)
        snprintf(l, sizeof l, "[mic] nabiz: %.0f/dk  ritim DUZENSIZ, vurus sayimiyla (%d vurus, yayilim %.2f, belirginlik %.1f; otokorelasyon r=%.2f)  sure %.1f sn  profil %s",
                 s->nabiz, s->nvurus, s->yayilim, s->belirgin, s->r, s->sure, p->ad);
    else if (ok && pencere)
        snprintf(l, sizeof l, "[mic] nabiz: %.0f/dk  DUSUK GUVEN, pencere kuraliyla (%d/%d pencere net ve birbirine yakin; butun kayit r=%.2f, aday %.0f/dk)  sure %.1f sn  profil %s",
                 s->nabiz, s->pen_iyi, s->pencere_n, s->r, s->nabiz_ac, s->sure, p->ad);
    else if (ok && tutarli)
        snprintf(l, sizeof l, "[mic] nabiz: %.0f/dk  DUSUK GUVEN, tutarlilik kuraliyla (%d/%d pencere ayni ritimde; r=%.2f, iki periyot sonra r=%.2f)  sure %.1f sn  profil %s",
                 s->nabiz, s->pen_uyan, s->pencere_n, s->r, s->r2t, s->sure, p->ad);
    else if (ok)
        snprintf(l, sizeof l, "[mic] nabiz: %.0f/dk  guven r=%.2f (%s)%s  sure %.1f sn  profil %s", s->nabiz, s->r, guven,
                 s->r2 > 0.5f ? "  [yari periyot da guclu: ikilenme suphesi]" : "", s->sure, p->ad);
    else if (strcmp(s->durum, "BELIRSIZ") == 0)
        snprintf(l, sizeof l, "[mic] nabiz: BELIRSIZ (ritim bulunamadi: r=%.2f, aday %.0f/dk; vurus belirginligi %.1f)  sure %.1f sn  profil %s  - mikrofon tam temas etmeli, hareket/konusma olmamali",
                 s->r, s->nabiz_ac, s->belirgin, s->sure, p->ad);
    else if (strcmp(s->durum, "KAT_SUPHESI") == 0)
        snprintf(l, sizeof l, "[mic] nabiz: BELIRSIZ - kat suphesi: otokorelasyon %.0f/dk (r=%.2f), vurus sayimi %.0f/dk; tekrar olcun  profil %s", s->nabiz_ac, s->r, s->nabiz_vurus, p->ad);
    else if (strcmp(s->durum, "MIRLAMA") == 0)
        snprintf(l, sizeof l, "[mic] nabiz: GECERSIZ - mirlama algilandi (%.0f Hz, cercevelerin %%%.0f'i); mirlama bitince tekrar olcun", s->mirlama_hz, 100.0f * s->mirlama_oran);
    else if (strcmp(s->durum, "DOYGUN") == 0)
        snprintf(l, sizeof l, "[mic] nabiz: GECERSIZ - kayit doygun (%lu ornek, %%%.2f): mikrofona darbe/baski ya da ses cok yuksek; aday %.0f/dk r=%.2f  profil %s", (unsigned long)s->doygun, doygun_yuzde, s->nabiz_ac, s->r, p->ad);
    else if (strcmp(s->durum, "SINYAL_YOK") == 0)
        snprintf(l, sizeof l, "[mic] nabiz: GECERSIZ - sinyal yok (bant seviyesi %.1f dBFS): mikrofon/kanal/baglanti kontrol edilmeli", s->seviye_db);
    else
        snprintf(l, sizeof l, "[mic] nabiz: hesaplanamadi (kayit cok kisa: en az 5 sn gerekir)");
    emit(l);

    /* 2) 8 sn pencereler, 4 sn adım */
    if (s->pencere_n > 0) {
        int off = snprintf(l, sizeof l, "[mic] pencereler (8 sn):");
        for (int k = 0; k < s->pencere_n && off < (int)sizeof l - 16; k++) off += snprintf(l + off, sizeof l - off, " %.0f(%.2f)", s->pencere_bpm[k], s->pencere_r[k]);
        emit(l);
    }

    /* 3) solunum */
    if (strcmp(s->sdurum, "OK") == 0)             snprintf(l, sizeof l, "[mic] solunum: %.0f/dk  guven r=%.2f", s->solunum, s->rs);
    else if (strcmp(s->sdurum, "SOLUMA") == 0)    snprintf(l, sizeof l, "[mic] solunum: SOLUMA (hizli soluk alip verme %.0f/dk, r=%.2f); dinlenme solunumu olculemez", s->soluma_hiz, s->soluma_r);
    else if (strcmp(s->sdurum, "MIRLAMA") == 0)   snprintf(l, sizeof l, "[mic] solunum: olculemedi (mirlama)");
    else if (strcmp(s->sdurum, "KALP_SIZINTISI") == 0) snprintf(l, sizeof l, "[mic] solunum: BELIRSIZ (bulunan %.0f/dk nabizla ayni: kalp sesi sizintisi)", s->solunum);
    else if (strcmp(s->sdurum, "KISA") == 0)      snprintf(l, sizeof l, "[mic] solunum: olculemedi (kayit kisa: en az 21 sn ve 2,5 solunum dongusu gerekir)");
    else                                          snprintf(l, sizeof l, "[mic] solunum: BELIRSIZ (r=%.2f, aday %.0f/dk)", s->rs, s->solunum);
    emit(l);

    /* 4) kalite */
    snprintf(l, sizeof l, "[mic] kalite: tepe %.1f dBFS, doygun %lu ornek (%%%.3f), kalp bandi %.1f dBFS, solunum bandi %.1f dBFS, hareket %.0f mg (%lu olcum)%s",
             s->tepe_db, (unsigned long)s->doygun, doygun_yuzde, s->seviye_db, s->sseviye_db, hareket, (unsigned long)a->h_n, hareket > NABIZ_HAREKET_MG ? " - HAREKET YUKSEK" : "");
    emit(l);

    /* 5) makine okur satır (tools/nar_olcum.py, ileride mobil uygulama): nabiz_ref.satir ile aynı + hareket/sd/etiket */
    {
        int off = snprintf(l, sizeof l, "[mic] sonuc: ");
        off += nabiz_satir(s, l + off, sizeof l - off);
        if (off < (int)sizeof l) snprintf(l + off, sizeof l - off, " hareket=%.0f sd=%s etiket=%s", hareket, sdname ? sdname : "yok", s_etiket);
        emit(l);
    }

    /* 6) BLE "son sonuç" (…0004): saat varsa saatle */
    {
        char ts[24] = "saat yok"; time_t now = time(NULL); struct tm tmv; localtime_r(&now, &tmv);
        if (tmv.tm_year + 1900 >= 2024) strftime(ts, sizeof ts, "%Y-%m-%d %H:%M", &tmv);
        char res[160]; int off;
        if (ok && vurus) off = snprintf(res, sizeof res, "nabiz %.0f/dk (duzensiz ritim)", s->nabiz);
        else if (ok && (pencere || tutarli)) off = snprintf(res, sizeof res, "nabiz %.0f/dk (dusuk guven)", s->nabiz);
        else if (ok)     off = snprintf(res, sizeof res, "nabiz %.0f/dk guven %.2f (%s)", s->nabiz, s->r, guven);
        else if (strcmp(s->durum, "BELIRSIZ") == 0 || strcmp(s->durum, "KAT_SUPHESI") == 0) off = snprintf(res, sizeof res, "nabiz BELIRSIZ");
        else             off = snprintf(res, sizeof res, "nabiz GECERSIZ (%s)", s->durum);
        if (off < (int)sizeof res) {
            if (strcmp(s->sdurum, "OK") == 0)          off += snprintf(res + off, sizeof res - off, ", solunum %.0f/dk", s->solunum);
            else if (strcmp(s->sdurum, "SOLUMA") == 0) off += snprintf(res + off, sizeof res - off, ", soluyor");
        }
        if (off < (int)sizeof res) snprintf(res + off, sizeof res - off, " | %s %.0f sn %s", p->ad, s->sure, ts);
        command_service_set_result(res);
    }
    if (note && note_n) snprintf(note, note_n, "%s | %s | durum=%s nabiz=%.0f r=%.2f solunum=%.0f(%s) hareket=%.0f", p->ad, s_etiket, s->durum, s->nabiz, s->r, s->solunum, s->sdurum, hareket);
    a->raporlandi = true;
    free(s);
}

/* I2S TDM RX kanalını açar ve etkinleştirir; hata durumunda *rx = NULL. */
static esp_err_t raw_i2s_open(int bytes, i2s_chan_handle_t* rx)
{
    *rx = NULL;
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.auto_clear = true; cc.dma_desc_num = 6; cc.dma_frame_num = RAW_FRAMES;   /* 6×1920 B dahili DMA (8'di; dahili RAM dar) */
    esp_err_t e = i2s_new_channel(&cc, NULL, rx);
    if (e != ESP_OK) { *rx = NULL; return e; }
    i2s_tdm_config_t tc = {};
    tc.clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(RAW_FS);
    tc.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    tc.slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(bytes == 2 ? I2S_DATA_BIT_WIDTH_16BIT : I2S_DATA_BIT_WIDTH_32BIT,
                   I2S_SLOT_MODE_STEREO, (i2s_tdm_slot_mask_t)(I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3));
    tc.gpio_cfg.mclk = (gpio_num_t)MIC_PIN_MCLK;
    tc.gpio_cfg.bclk = (gpio_num_t)MIC_PIN_BCLK;
    tc.gpio_cfg.ws   = (gpio_num_t)MIC_PIN_WS;
    tc.gpio_cfg.dout = I2S_GPIO_UNUSED;
    tc.gpio_cfg.din  = (gpio_num_t)MIC_PIN_DIN;
    e = i2s_channel_init_tdm_mode(*rx, &tc);
    if (e == ESP_OK) e = i2s_channel_enable(*rx);
    if (e != ESP_OK) { i2s_del_channel(*rx); *rx = NULL; }
    return e;
}

/* Ortak yakalama döngüsü (2026-11-09): okuyucu görev PSRAM halkasına yazar; bu döngü halkayı boşaltıp
 * (a) USB'ye çerçeve basar (to_usb) ve/veya (b) SD karttaki WAV dosyasına yazar (to_sd).
 * SD: kart bağlanamazsa to_usb varsa USB-only sürer (uyarı satırı), yoksa hata döner.
 * 48 kHz × 3 bayt = 144 KB/s; 1 MB halka ~7 sn tampon, SD yazma duraklamaları bunu aşmaz. */
#define SD_CHUNK 8192        /* dahili DMA belleği parçalı: 8 → 4 → 2 KB denenir (2 KB = 4 sektör, yeterli) */

static esp_err_t raw_run(int seconds, int channel, int bits, int variant, void (*emit)(const char*), bool to_usb, bool to_sd, bpm_t* bpm)
{
    if (!emit) return ESP_ERR_INVALID_ARG;
    if (seconds < 1) seconds = 1;
    if (seconds > 300) seconds = 300;
    if (channel < 1 || channel > MIC_NCH) channel = 3;
    const int bytes = bits == 24 ? 3 : 2;
    char l[160];

    /* Teşhis (2026-11-09): dahili heap durumu + bütünlük. Bozuksa I2S DMA ayırımı rastgele adres döndürüp çöküyor. */
    {
        bool ok = heap_caps_check_integrity_all(true);
        snprintf(l, sizeof l, "[mic] bellek: dahili serbest %u B, en buyuk blok %u B, DMA serbest %u B, butunluk %s, bu gorevin yigini %u B kaldi",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL), ok ? "OK" : "BOZUK",
                 (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
        emit(l);
        if (!ok) return ESP_ERR_INVALID_STATE;
    }

    /* SD: önce bağla ki kart yoksa I2S açılmadan bilinsin */
    FILE* wf = NULL; char wname[16] = {0}; char sdmsg[80] = "kapali"; bool sd_ok = false; uint32_t sd_bytes = 0;
    if (to_sd) {
        if (mic_sd_mount(sdmsg, sizeof sdmsg)) {
            wf = mic_sd_wav_open(RAW_FS, bytes * 8, wname, sizeof wname);
            if (wf) sd_ok = true;
            else { snprintf(sdmsg, sizeof sdmsg, "dosya acilamadi (salt okunur kart?)"); mic_sd_unmount(); }
        }
        if (!sd_ok) {
            snprintf(l, sizeof l, "[mic] SD: %s", sdmsg); emit(l);
            if (!to_usb && !bpm) return ESP_ERR_NOT_FOUND;   /* analiz varsa SD'siz sürer */
        }
    }

    raw_ctx_t c = {};
    c.ch = channel - 1; c.bytes = bytes; c.frames = (uint32_t)seconds * RAW_FS; c.waiter = xTaskGetCurrentTaskHandle();
    c.ring = (uint8_t*)heap_caps_malloc(RAW_RING, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t* pkt = (uint8_t*)heap_caps_malloc(16 + RAW_PAYLOAD, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    /* SD tamponu DAHİLİ + DMA bellek: SDMMC sürücüsü PSRAM'den DMA yapamıyor (ilk kartta fwrite anında hata verdi).
     * 16 KB tek parça bulunamadı (NO_MEM); küçülterek dene. */
    uint8_t* sdbuf = NULL; uint32_t sd_chunk = SD_CHUNK;
    if (sd_ok) {
        for (; sd_chunk >= 2048 && !sdbuf; sd_chunk /= 2)
            sdbuf = (uint8_t*)heap_caps_malloc(sd_chunk, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        if (sdbuf) sd_chunk *= 2;                                   /* döngü bir kez fazla böldü */
    }
    if (sd_ok && !sdbuf && (to_usb || bpm)) {
        /* SD tamponu için dahili DMA bellek yok: SD kaydından vazgeç, akış/ölçüm sürsün (2026-11-10) */
        emit("[mic] SD: tampon icin dahili bellek yok, bu kayit SD'ye yazilmayacak");
        mic_sd_wav_discard(wf, wname); mic_sd_unmount(); wf = NULL; sd_ok = false;
    }
    if (!c.ring || !pkt || (sd_ok && !sdbuf)) {
        snprintf(l, sizeof l, "[mic] bellek yok: halka=%d pkt=%d sdbuf=%d (dahili serbest %u, en buyuk %u)", c.ring ? 1 : 0, pkt ? 1 : 0, sdbuf ? 1 : 0,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
        emit(l);
        free(c.ring); free(pkt); free(sdbuf);
        if (wf) { mic_sd_wav_discard(wf, wname); mic_sd_unmount(); }
        return ESP_ERR_NO_MEM;
    }

    esp_err_t e = raw_i2s_open(bytes, &c.rx);
    if (e != ESP_OK && sd_ok && (to_usb || bpm)) {
        /* Dahili RAM dar olabilir (SD + I2S DMA birlikte): SD'den vazgeçip bir kez daha dene; ölçüm kayıttan önce gelir. */
        snprintf(l, sizeof l, "[mic] I2S acilamadi (%s); SD kapatilip yeniden deneniyor", esp_err_to_name(e)); emit(l);
        mic_sd_wav_discard(wf, wname); mic_sd_unmount(); wf = NULL; sd_ok = false; free(sdbuf); sdbuf = NULL;
        e = raw_i2s_open(bytes, &c.rx);
    }
    if (e != ESP_OK) {
        snprintf(l, sizeof l, "[mic] I2S acilamadi: %s (dahili serbest %u, en buyuk DMA blok %u)", esp_err_to_name(e),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
        emit(l);
        free(c.ring); free(pkt); free(sdbuf);
        if (wf) { mic_sd_wav_discard(wf, wname); mic_sd_unmount(); }
        return e;
    }
    s_variant = variant;   /* 0 = normal, 3 = HPF kapalı (kalp) */
    bool cfg = es7210_setup();
    if (bytes == 3) cfg &= (i2c_bus_write_reg(A_ES7210, 0x11, 0x80) == ESP_OK);   /* kelime 32 bit */
    snprintf(l, sizeof l, "[mic] raw %d sn, %d Hz, %d bit, kanal %d, varyant %d, ES7210 %s, SD %s%s%s", seconds, RAW_FS, bytes * 8, channel, variant,
             cfg ? "ok" : "AYAR HATASI", sd_ok ? "KAYIT/" : "", sd_ok ? wname : (to_sd ? "yok" : "kapali"), to_usb ? "" : " (USB akisi yok)");
    emit(l);
    if (to_usb) esp_log_level_set("*", ESP_LOG_NONE);   /* çerçevelerin arasına log girmesin */
    vTaskDelay(pdMS_TO_TICKS(50));

    /* 2026-11-09: 4 KB yığın BLE bağlıyken taştı (kesme çerçeveleri görev yığınına iner) → I2S/GDMA yapıları bozuldu,
     * i2s_del_channel içinde InstrFetchProhibited. 8 KB; kalan pay bitiş satırında raporlanır. */
    /* Okuyucu görev: yığın PSRAM'de (CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y; görev flaşa dokunmaz), TCB dahili.
     * Dahili RAM'e 0 yük; 8 KB rahat. Görev kendini silmez, raw_run siler ve tamponları bırakır. */
    const uint32_t rstack = 8192;
    StackType_t*  rbuf = (StackType_t*)heap_caps_malloc(rstack, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    StaticTask_t* rtcb = (StaticTask_t*)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    TaskHandle_t  rtask = (rbuf && rtcb) ? xTaskCreateStaticPinnedToCore(raw_reader_task, "mic_raw", rstack / sizeof(StackType_t), &c, 12, rbuf, rtcb, 1) : NULL;
    BaseType_t tr = rtask ? pdPASS : pdFAIL;
    if (tr != pdPASS) {
        free(rbuf); free(rtcb);
        esp_log_level_set("*", ESP_LOG_INFO);
        snprintf(l, sizeof l, "[mic] okuyucu gorev acilamadi: dahili serbest %u B, en buyuk blok %u B", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        emit(l);
        i2s_channel_disable(c.rx); i2s_del_channel(c.rx); free(c.ring); free(pkt); free(sdbuf);
        if (wf) { mic_sd_wav_discard(wf, wname); mic_sd_unmount(); }
        return ESP_ERR_NO_MEM;
    }

    uint32_t seq = 0, sent = 0, sdfill = 0; bool usb_ok = to_usb; int sd_errno = 0;
    const uint32_t pull = to_usb ? RAW_PAYLOAD : (sdbuf ? sd_chunk : RAW_PAYLOAD);   /* USB çerçevesi 960 B; yalnız SD'de sd_chunk; yalnız analizde 960 */
    while ((usb_ok || sd_ok || bpm) && !(c.done && c.head == c.tail)) {
        uint32_t have = c.head - c.tail;
        if (bpm) bpm_hareket(bpm);                              /* 10 Hz ivmeölçer (kendi içinde zaman kapılı) */
        if (have < pull && !c.done) { vTaskDelay(1); continue; }
        uint32_t len = have < pull ? have : pull;
        if (len == 0) break;
        uint32_t sum = 0;
        uint8_t* dst = to_usb ? pkt + 16 : (sdbuf ? sdbuf : pkt + 16);   /* yalnız SD modunda doğrudan SD tamponuna */
        for (uint32_t i = 0; i < len; i++) { uint8_t v = c.ring[(c.tail + i) % RAW_RING]; dst[i] = v; sum += v; }
        c.tail = c.tail + len;
        if (bpm) bpm_feed(bpm, dst, len, bytes);

        if (usb_ok) {
            uint16_t len16 = (uint16_t)len, ov = c.over > 65535 ? 65535 : (uint16_t)c.over;
            memcpy(pkt, "NARA", 4); memcpy(pkt + 4, &seq, 4); memcpy(pkt + 8, &len16, 2); memcpy(pkt + 10, &ov, 2); memcpy(pkt + 12, &sum, 4);
            size_t off = 0, tot = 16u + len; int stall = 0;
            while (off < tot) {
                int w = usb_serial_jtag_write_bytes(pkt + off, tot - off, pdMS_TO_TICKS(500));
                if (w > 0) { off += w; stall = 0; }
                else if (++stall > 20) { usb_ok = false; break; }   /* 10 sn USB alıcısı yok; SD varsa kayıt sürer */
            }
            seq++; sent += len;
        }
        if (sd_ok) {
            if (to_usb) {                                          /* USB çerçevesini SD tamponuna biriktir */
                memcpy(sdbuf + sdfill, pkt + 16, len); sdfill += len;
                if (sdfill + RAW_PAYLOAD > sd_chunk || c.done) {
                    if (fwrite(sdbuf, 1, sdfill, wf) != sdfill) { sd_ok = false; sd_errno = errno; } else sd_bytes += sdfill;
                    sdfill = 0;
                }
            } else if (sdbuf) {
                if (fwrite(sdbuf, 1, len, wf) != len) { sd_ok = false; sd_errno = errno; } else sd_bytes += len;
            }
        }
    }
    if (sd_ok && sdfill) { if (fwrite(sdbuf, 1, sdfill, wf) == sdfill) sd_bytes += sdfill; else { sd_ok = false; sd_errno = errno; } }
    if (!c.done) ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(seconds * 1000 + 3000));
    if (!c.done) { emit("[mic] UYARI: okuyucu gorev zamaninda bitmedi"); }
    vTaskDelay(pdMS_TO_TICKS(20));            /* okuyucu vTaskSuspend'e girsin */
    vTaskDelete(rtask); free(rbuf); free(rtcb);
    i2s_channel_disable(c.rx);
    i2s_del_channel(c.rx);
    esp_log_level_set("*", ESP_LOG_INFO);
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Analiz raporu SD kapanmadan önce: sonuç LISTE.TXT satırına da girsin (2026-11-10) */
    char bnote[190] = "";
    if (bpm) bpm_report(bpm, emit, (wf && sd_ok && sd_bytes) ? wname : "yok", bnote, sizeof bnote);

    bool sd_closed = false;
    if (wf) {
        if (!sd_ok) { snprintf(l, sizeof l, "[mic] SD yazma hatasi errno=%d (%s) yazilan=%lu B", sd_errno, strerror(sd_errno), (unsigned long)sd_bytes); emit(l); }
        if (!sd_ok && sd_bytes == 0) {
            mic_sd_wav_discard(wf, wname);                     /* bos dosya birakma */
        } else {
            char note[260]; snprintf(note, sizeof note, "kanal %d varyant %d%s%s%s", channel, variant, sd_ok ? "" : " YAZMA HATASI", bnote[0] ? " | " : "", bnote);
            sd_closed = mic_sd_wav_close(wf, wname, sd_bytes, RAW_FS, bytes * 8, note);
        }
        mic_sd_unmount();
    }
    snprintf(l, sizeof l, "[mic] raw bitti cerceve=%lu bayt=%lu tasma=%lu usb=%d sd=%s yigin: okuyucu %lu/%lu B, bu gorev %lu B kaldi", (unsigned long)seq, (unsigned long)sent, (unsigned long)c.over,
             usb_ok ? 1 : 0, wf ? (sd_ok && sd_closed ? wname : "YAZMA HATASI") : (to_sd ? "yok" : "kapali"),
             (unsigned long)c.reader_stack_left, (unsigned long)rstack, (unsigned long)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)));
    emit(l);
    if (wf && sd_ok && sd_closed) {
        snprintf(l, sizeof l, "[mic] SD kayit: KAYIT/%s %lu KB (%lu sn, %d bit)", wname, (unsigned long)(sd_bytes / 1024),
                 (unsigned long)(sd_bytes / (uint32_t)(bytes * RAW_FS)), bytes * 8);
        emit(l);
    }
    free(c.ring); free(pkt); free(sdbuf);
    if (to_usb && !usb_ok) return ESP_ERR_TIMEOUT;
    if (to_sd && !to_usb && !bpm && !(sd_ok && sd_closed)) return ESP_FAIL;
    return ESP_OK;
}

/* USB akışı + (kart takılıysa) SD kopyası. Kart yoksa eskisi gibi yalnız USB. */
esp_err_t mic_service_raw(int seconds, int channel, int bits, int variant, void (*emit)(const char*))
{
    return raw_run(seconds, channel, bits, variant, emit, true, true, NULL);
}

/* Yalnız SD'ye kayıt (USB'ye çerçeve basılmaz; loglar açık kalır). Kart yoksa ESP_ERR_NOT_FOUND. */
esp_err_t mic_service_sdrec(int seconds, int channel, int bits, int variant, void (*emit)(const char*))
{
    return raw_run(seconds, channel, bits, variant, emit, false, true, NULL);
}

/* Kartta nabız: kaydı analiz eder, sonucu emit ile yazar. profil: mic_service_profil_bul() dizini (0 = insan).
 * seconds <= 0 → profilin varsayılan süresi (insan 15, kedi/köpek 30). to_sd: ham WAV karta (kart yoksa ölçüm sürer).
 * to_usb: ham akış bilgisayara da gider (tools/nar_olcum.py, nar_nabiz.sh). USB alıcısı yoksa akış düşer, analiz sürer. */
esp_err_t mic_service_nabiz(int seconds, int channel, int bits, int variant, bool to_sd, bool to_usb, int profil, void (*emit)(const char*))
{
    if (profil < 0 || profil >= nabiz_profil_sayisi()) profil = 0;
    if (seconds <= 0) seconds = nabiz_profil(profil)->sn;
    if (seconds < 5) seconds = 5;
    if (seconds > 300) seconds = 300;
    bpm_t a;
    if (!bpm_init(&a, seconds, bits == 24 ? 3 : 2, nabiz_profil(profil))) { bpm_bitir(&a); return ESP_ERR_NO_MEM; }
    esp_err_t e = raw_run(seconds, channel, bits, variant, emit, to_usb, to_sd, &a);
    if (!a.raporlandi) { s_last_bpm = 0; s_last_r = 0; if (e != ESP_OK) command_service_set_result("nabiz olculemedi (kayit baslamadi)"); }
    bpm_bitir(&a);
    if (a.raporlandi) return ESP_OK;    /* rapor verildiyse SD/USB yan hataları ölçümü başarısız saymaz */
    return e == ESP_OK ? ESP_ERR_NO_MEM : e;
}

/* SD'deki kayıtları listeler; kartı bağlar ve ayırır. */
esp_err_t mic_service_sd_list(void (*emit)(const char*))
{
    char d[80], l[120];
    if (!mic_sd_mount(d, sizeof d)) { snprintf(l, sizeof l, "[mic] SD: %s", d); emit(l); return ESP_ERR_NOT_FOUND; }
    snprintf(l, sizeof l, "[mic] SD %s, klasor /sdcard/KAYIT", d); emit(l);
    int n = mic_sd_list(emit);
    if (n < 0) emit("[mic]   (KAYIT klasoru yok; ilk kayitta olusur)");
    else { snprintf(l, sizeof l, "[mic]   %d dosya", n); emit(l); }
    mic_sd_unmount();
    return ESP_OK;
}

/* ── Saat teşhisi ─────────────────────────────────────────────────────────── */
static uint32_t count_hz(int pin, int ms)
{
    pcnt_unit_config_t uc = {}; uc.low_limit = -1; uc.high_limit = 32767; uc.flags.accum_count = true;
    pcnt_unit_handle_t u = NULL;
    if (pcnt_new_unit(&uc, &u) != ESP_OK) return 0xFFFFFFFFu;
    pcnt_chan_config_t cc = {}; cc.edge_gpio_num = pin; cc.level_gpio_num = -1;
    pcnt_channel_handle_t ch = NULL;
    if (pcnt_new_channel(u, &cc, &ch) != ESP_OK) { pcnt_del_unit(u); return 0xFFFFFFFFu; }
    pcnt_channel_set_edge_action(ch, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_HOLD);
    pcnt_unit_add_watch_point(u, 32767);
    pcnt_unit_enable(u); pcnt_unit_clear_count(u); pcnt_unit_start(u);
    int64_t t0 = esp_timer_get_time(); ets_delay_us(ms * 1000); int64_t dt = esp_timer_get_time() - t0;
    int c = 0; pcnt_unit_get_count(u, &c);
    pcnt_unit_stop(u); pcnt_unit_disable(u); pcnt_del_channel(ch); pcnt_del_unit(u);
    return (uint32_t)((int64_t)c * 1000000 / (dt > 0 ? dt : 1));
}

static esp_err_t open_rx(i2s_role_t role, i2s_chan_handle_t* out)
{
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, role);
    i2s_chan_handle_t rx = NULL;
    esp_err_t e = i2s_new_channel(&cc, NULL, &rx);
    if (e != ESP_OK) return e;
    i2s_tdm_config_t tc = {};
    tc.clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(MIC_FS_HW);
    tc.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    tc.slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO,
                   (i2s_tdm_slot_mask_t)(I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3));
    tc.gpio_cfg.mclk = (gpio_num_t)MIC_PIN_MCLK; tc.gpio_cfg.bclk = (gpio_num_t)MIC_PIN_BCLK;
    tc.gpio_cfg.ws = (gpio_num_t)MIC_PIN_WS; tc.gpio_cfg.dout = I2S_GPIO_UNUSED; tc.gpio_cfg.din = (gpio_num_t)MIC_PIN_DIN;
    e = i2s_channel_init_tdm_mode(rx, &tc);
    if (e == ESP_OK) e = i2s_channel_enable(rx);
    if (e != ESP_OK) { i2s_del_channel(rx); return e; }
    *out = rx; return ESP_OK;
}

/* Volt testi: ADC'li pedlerde (GPIO1-20) pedin GERÇEK gerilimini oku. RTC IO üzerinden sürülür
 * (ADC pad'i analog moda aldığı için dijital GPIO çıkışı pede ulaşmaz).
 *   pu : yalnız dahili pull-up ile → serbest hat ≈3300 mV, kelepçe ≈600-2000 mV, sert kısa ≈0
 *   d3 : en güçlü sürücüyle HIGH → serbest ≈3300, beslemesiz çip ESD diyotu ≈600-2000, kısa ≈0
 * Dönüş: false = bu pin ADC'li değil / ADC açılamadı. */
static bool pad_volt(int pin, int* pu_mv, int* d3_mv)
{
    if (pin < 1 || pin > 20) return false;
    adc_unit_t unit = (pin <= 10) ? ADC_UNIT_1 : ADC_UNIT_2;
    adc_channel_t ch = (adc_channel_t)((pin <= 10) ? pin - 1 : pin - 11);
    adc_oneshot_unit_handle_t adc = NULL;
    adc_oneshot_unit_init_cfg_t ucfg = {}; ucfg.unit_id = unit;
    if (adc_oneshot_new_unit(&ucfg, &adc) != ESP_OK) return false;
    adc_oneshot_chan_cfg_t ccfg = {}; ccfg.atten = ADC_ATTEN_DB_12; ccfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    adc_oneshot_config_channel(adc, ch, &ccfg);
    adc_cali_handle_t cal = NULL;
    adc_cali_curve_fitting_config_t cc = {}; cc.unit_id = unit; cc.atten = ADC_ATTEN_DB_12; cc.bitwidth = ADC_BITWIDTH_DEFAULT;
    if (adc_cali_create_scheme_curve_fitting(&cc, &cal) != ESP_OK) cal = NULL;
    auto read_mv = [&]() -> int {
        int sum = 0, cnt = 0;
        for (int i = 0; i < 8; i++) {
            int raw = 0;
            if (adc_oneshot_read(adc, ch, &raw) != ESP_OK) continue;
            int mv = raw * 3100 / 4095;
            if (cal) adc_cali_raw_to_voltage(cal, raw, &mv);
            sum += mv; cnt++;
        }
        return cnt ? sum / cnt : -1;
    };
    gpio_num_t g = (gpio_num_t)pin;
    rtc_gpio_init(g);
    rtc_gpio_set_direction(g, RTC_GPIO_MODE_INPUT_ONLY);
    rtc_gpio_pulldown_dis(g); rtc_gpio_pullup_en(g); ets_delay_us(300);
    *pu_mv = read_mv();
    rtc_gpio_pullup_dis(g);
    rtc_gpio_set_drive_capability(g, GPIO_DRIVE_CAP_3);
    rtc_gpio_set_direction(g, RTC_GPIO_MODE_INPUT_OUTPUT);
    rtc_gpio_set_level(g, 1); ets_delay_us(300);
    *d3_mv = read_mv();
    rtc_gpio_set_level(g, 0);
    rtc_gpio_set_direction(g, RTC_GPIO_MODE_DISABLED);
    rtc_gpio_deinit(g);
    if (cal) adc_cali_delete_scheme_curve_fitting(cal);
    adc_oneshot_del_unit(adc);
    gpio_reset_pin(g);
    return true;
}

/* Standart I2S (Philips) 16 bit stereo, 16 kHz — ESP-BOX'un ES7210 ile kanıtlanmış yolu (MIC1/MIC2,
 * TDM yok, REG12=0x00). TDM çerçeve biçimi uyuşmazlığı şüphesini elemek için. */
static esp_err_t open_rx_std(i2s_role_t role, i2s_chan_handle_t* out)
{
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, role);
    i2s_chan_handle_t rx = NULL;
    esp_err_t e = i2s_new_channel(&cc, NULL, &rx);
    if (e != ESP_OK) return e;
    i2s_std_config_t sc = {};
    sc.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_FS_HW);
    sc.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    sc.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    sc.gpio_cfg.mclk = (gpio_num_t)MIC_PIN_MCLK; sc.gpio_cfg.bclk = (gpio_num_t)MIC_PIN_BCLK;
    sc.gpio_cfg.ws = (gpio_num_t)MIC_PIN_WS; sc.gpio_cfg.dout = I2S_GPIO_UNUSED; sc.gpio_cfg.din = (gpio_num_t)MIC_PIN_DIN;
    e = i2s_channel_init_std_mode(rx, &sc);
    if (e == ESP_OK) e = i2s_channel_enable(rx);
    if (e != ESP_OK) { i2s_del_channel(rx); return e; }
    *out = rx; return ESP_OK;
}

static bool es7210_setup_stereo(void)
{
    /* esp_codec_dev es7210.c: open + mic_select(MIC1|MIC2) + start; REG01 off_reg = 0x3F & ~0x0B = 0x34 */
    static const uint8_t seq[][2] = {
        {0x00, 0xFF}, {0x00, 0x41}, {0x01, 0x3F}, {0x09, 0x30}, {0x0A, 0x30},
        {0x23, 0x2A}, {0x22, 0x0A}, {0x20, 0x0A}, {0x21, 0x2A},
        {0x08, 0x10},                                   /* slave */
        {0x40, 0x43}, {0x41, 0x70}, {0x42, 0x70}, {0x07, 0x20}, {0x02, 0xC1},
        {0x43, 0x00}, {0x44, 0x00}, {0x45, 0x00}, {0x46, 0x00}, {0x4B, 0xFF}, {0x4C, 0xFF},
        {0x01, 0x34}, {0x4B, 0x00}, {0x43, 0x1A}, {0x44, 0x1A},   /* MIC1+MIC2 on, 30 dB */
        {0x12, 0x00},                                   /* TDM YOK: standart I2S, SDOUT1 = MIC1(L)/MIC2(R) */
        {0x11, 0x60},                                   /* 16 bit I2S */
        /* start */
        {0x01, 0x34}, {0x06, 0x00}, {0x40, 0x43}, {0x47, 0x08}, {0x48, 0x08}, {0x49, 0x08}, {0x4A, 0x08},
        {0x40, 0x43}, {0x00, 0x71}, {0x00, 0x41},
    };
    bool ok = true;
    for (const auto& r : seq) ok &= (i2c_bus_write_reg(A_ES7210, r[0], r[1]) == ESP_OK);
    return ok;
}

/* Ped sürme + ayrım testi — `mic clocks` adım 0 ve `pad test <gpio…>` ortak çekirdeği.
 * Pini düz GPIO çıkışı yapıp HIGH/LOW sür, geri oku. HIGH sürülüp LOW okunuyorsa ped
 * dışarıdan toprağa çekiliyor (kısa devre / hasar). En fazla 8 pin (satır tamponu). */
static void pad_test(const int* pins, int n, void (*emit)(const char*))
    {
        char l[320];
        int pos = snprintf(l, sizeof l, "[mic] ped surme testi (sur->oku):");
        for (int i = 0; i < n; i++) {
            gpio_reset_pin((gpio_num_t)pins[i]);
            gpio_set_direction((gpio_num_t)pins[i], GPIO_MODE_INPUT_OUTPUT);
            gpio_set_level((gpio_num_t)pins[i], 1); ets_delay_us(50); int h = gpio_get_level((gpio_num_t)pins[i]);
            gpio_set_level((gpio_num_t)pins[i], 0); ets_delay_us(50); int lo = gpio_get_level((gpio_num_t)pins[i]);
            gpio_reset_pin((gpio_num_t)pins[i]);
            pos += snprintf(l + pos, sizeof l - pos, "  GPIO%d H->%d L->%d%s", pins[i], h, lo,
                            (h == 1 && lo == 0) ? " ok" : (h == 0) ? " TOPRAGA KISA?" : " ?");
        }
        emit(l);

        /* 0b) Ayrım testi — kısa devre mi, ESP ped sürücüsü mü?
         *    pu : yalnız dahili pull-up (~45 kΩ), çıkış kapalı → net serbestse 1 okur.
         *    pd : yalnız dahili pull-down → her durumda 0 beklenir (kontrol).
         *    d0 : en zayıf sürücü (~5 mA) HIGH;  d3 : en güçlü sürücü (~40 mA) HIGH.
         *    pu=1 & d3=0 → net serbest ama ped çıkışı ölü (ESP hasarı).
         *    pu=0 & d3=0 → net dışarıdan toprağa çekili (kısa devre / lehim köprüsü).
         *    pu=0 & d3=1 → yüksek dirençli kısa (d0 0, d3 1 ise ~birkaç yüz Ω). */
        pos = snprintf(l, sizeof l, "[mic] ayrim testi (pu/pd/d0/d3):");
        for (int i = 0; i < n; i++) {
            gpio_num_t g = (gpio_num_t)pins[i];
            gpio_reset_pin(g);
            gpio_set_direction(g, GPIO_MODE_INPUT);
            gpio_set_pull_mode(g, GPIO_PULLUP_ONLY);   ets_delay_us(200); int pu = gpio_get_level(g);
            gpio_set_pull_mode(g, GPIO_PULLDOWN_ONLY); ets_delay_us(200); int pd = gpio_get_level(g);
            gpio_set_pull_mode(g, GPIO_FLOATING);
            gpio_set_direction(g, GPIO_MODE_INPUT_OUTPUT);
            gpio_set_drive_capability(g, GPIO_DRIVE_CAP_0); gpio_set_level(g, 1); ets_delay_us(50); int d0 = gpio_get_level(g);
            gpio_set_drive_capability(g, GPIO_DRIVE_CAP_3); gpio_set_level(g, 1); ets_delay_us(50); int d3 = gpio_get_level(g);
            gpio_set_level(g, 0);
            gpio_reset_pin(g);
            const char* v = (pu == 1 && d3 == 1) ? "ok" : (pu == 1 && d3 == 0) ? "PED CIKISI OLU?" :
                            (pu == 0 && d3 == 1) ? "dirençli kisa" : "TOPRAGA KISA";
            pos += snprintf(l + pos, sizeof l - pos, "  GPIO%d %d/%d/%d/%d %s", pins[i], pu, pd, d0, d3, v);
        }
        emit(l);

        /* Volt testi (yalnız ADC'li pedler GPIO1-20): pull-up ile ve 40 mA HIGH sürerken ped mV. */
        pos = snprintf(l, sizeof l, "[mic] volt testi (pu mV / d3 mV):");
        bool any = false;
        for (int i = 0; i < n; i++) {
            int pu_mv = -1, d3_mv = -1;
            if (!pad_volt(pins[i], &pu_mv, &d3_mv)) continue;
            any = true;
            const char* v = (d3_mv > 2800) ? "serbest" : (d3_mv > 300) ? "KELEPCE (beslemesiz cip/diyot?)" : "SERT KISA";
            pos += snprintf(l + pos, sizeof l - pos, "  GPIO%d %d/%d %s", pins[i], pu_mv, d3_mv, v);
        }
        if (any) emit(l);
    }

esp_err_t mic_service_pad_test(const int* pins, int n, void (*emit)(const char*))
{
    char l[160];
    bool i2c = false;
    for (int i = 0; i < n; i++) {
        int p = pins[i];
        /* Güvenli olmayan pedler: USB (19/20), flash/PSRAM (26-37), nRF9151 UART (43/44), strap 0/3/45/46. */
        bool bad = (p < 0 || p > 48 || p == 19 || p == 20 || (p >= 26 && p <= 37) ||
                    p == 43 || p == 44 || p == 0 || p == 3 || p == 46);
        if (bad) { snprintf(l, sizeof l, "[pad] GPIO%d bu testte SURULMEZ (USB/flash/GSM UART/strap)", p); emit(l); return ESP_ERR_INVALID_ARG; }
        if (p == PIN_I2C_SDA || p == PIN_I2C_SCL) i2c = true;
    }
    if (i2c) {
        emit("[pad] I2C SDA/SCL dahil: hat kilitlendi, test sonrasi I2C'ye geri baglanir (harici pull-up var: pu/pd=1 normal)");
        i2c_bus_lock();
    }
    pad_test(pins, n, emit);
    if (i2c) {
        esp_err_t e = i2c_bus_reattach_pins();
        i2c_bus_unlock();
        uint8_t id = 0;
        esp_err_t r = i2c_bus_read_reg(0x34, 0x03, &id, 1);   /* AXP2101 chip id — hat sağ mı? */
        snprintf(l, sizeof l, "[pad] I2C geri baglandi: %s, PMIC okuma %s (id 0x%02X)",
                 esp_err_to_name(e), r == ESP_OK ? "OK" : esp_err_to_name(r), id);
        emit(l);
    }
    return ESP_OK;
}

esp_err_t mic_service_clock_test(void (*emit)(const char*))
{
    char l[200];
    uint8_t id = 0;
    if (i2c_bus_read_reg(A_ES7210, 0x3D, &id, 1) != ESP_OK) { emit("[mic] ES7210 I2C cevap yok"); return ESP_ERR_NOT_FOUND; }

    /* 0) Ped sürme + ayrım testi (pad_test): MCLK, komşu GPIO40, BCLK, WS. */
    {
        static const int pins[] = { MIC_PIN_MCLK, 40, MIC_PIN_BCLK, MIC_PIN_WS };
        pad_test(pins, 4, emit);
    }

    /* 1) ESP master: saatler pedlerde var mı? */
    i2s_chan_handle_t rx = NULL;
    esp_err_t e = open_rx(I2S_ROLE_MASTER, &rx);
    if (e != ESP_OK) { snprintf(l, sizeof l, "[mic] I2S master acilamadi: %s", esp_err_to_name(e)); emit(l); return e; }
    s_variant = 0; es7210_setup();
    vTaskDelay(pdMS_TO_TICKS(100));
    uint32_t f_mclk = count_hz(MIC_PIN_MCLK, 4), f_bclk = count_hz(MIC_PIN_BCLK, 4), f_ws = count_hz(MIC_PIN_WS, 20), f_sd = count_hz(MIC_PIN_DIN, 20);
    snprintf(l, sizeof l, "[mic] ESP master: MCLK(16)=%lu Hz  BCLK(41)=%lu Hz  WS(45)=%lu Hz  SDOUT(42)=%lu kenar/s  (beklenen 4096000 / 1024000 / 16000 / 0)",
             (unsigned long)f_mclk, (unsigned long)f_bclk, (unsigned long)f_ws, (unsigned long)f_sd);
    emit(l);
    /* ADC çıkışı bu sırada ne? (ham, DC'siz) */
    {
        int16_t* buf = (int16_t*)malloc(MIC_CHUNK * MIC_NCH * 2); size_t got = 0;
        if (buf) {
            for (int i = 0; i < 10; i++) i2s_channel_read(rx, buf, MIC_CHUNK * MIC_NCH * 2, &got, pdMS_TO_TICKS(200));
            int32_t mn = 32767, mx = -32768;
            for (int i = 0; i < MIC_CHUNK * MIC_NCH; i++) { if (buf[i] < mn) mn = buf[i]; if (buf[i] > mx) mx = buf[i]; }
            snprintf(l, sizeof l, "[mic] ESP master iken ADC ham (4 slot): %ld..%ld", (long)mn, (long)mx); emit(l); free(buf);
        }
    }
    /* SDOUT canlı mı? Saatler çalışırken DIN'e yalnız pull-up / yalnız pull-down tak ve 2000 örnek say.
     *   pu≈2000 & pd≈0  → hat Hi-Z: kodek SDOUT'u SÜRMÜYOR (watch/besleme/bağlantı sorunu)
     *   pu≈0   & pd≈0  → kodek sürekli 0 sürüyor (veri yolu çalışıyor, ADC sessiz)
     *   karışık         → gerçek veri akıyor */
    {
        gpio_num_t d = (gpio_num_t)MIC_PIN_DIN;
        int pu = 0, pd = 0;
        gpio_set_pull_mode(d, GPIO_PULLUP_ONLY);   ets_delay_us(200);
        for (int i = 0; i < 2000; i++) { pu += gpio_get_level(d); ets_delay_us(3); }
        gpio_set_pull_mode(d, GPIO_PULLDOWN_ONLY); ets_delay_us(200);
        for (int i = 0; i < 2000; i++) { pd += gpio_get_level(d); ets_delay_us(3); }
        gpio_set_pull_mode(d, GPIO_FLOATING);
        const char* v = (pu > 1900 && pd < 100) ? "Hi-Z: KODEK SDOUT'U SURMUYOR" :
                        (pu < 100 && pd < 100) ? "surekli 0 suruluyor (veri yolu var, ADC sessiz)" : "veri akiyor";
        snprintf(l, sizeof l, "[mic] SDOUT canlilik (saatler acikken): pull-up'ta 1=%d/2000, pull-down'da 1=%d/2000 -> %s", pu, pd, v);
        emit(l);
    }
    i2s_channel_disable(rx); i2s_del_channel(rx); rx = NULL;

    /* 1b) Standart I2S stereo (TDM yok) — ESP-BOX yolu. SDOUT canlı mı, L/R ham veri var mı? */
    e = open_rx_std(I2S_ROLE_MASTER, &rx);
    if (e == ESP_OK) {
        bool cfg = es7210_setup_stereo();
        vTaskDelay(pdMS_TO_TICKS(100));
        uint32_t sb = count_hz(MIC_PIN_BCLK, 4), ssd = count_hz(MIC_PIN_DIN, 20);
        int16_t* buf = (int16_t*)malloc(MIC_CHUNK * 2 * 2); size_t got = 0;
        int32_t lmn = 32767, lmx = -32768, rmn = 32767, rmx = -32768;
        if (buf) {
            for (int i = 0; i < 10; i++) i2s_channel_read(rx, buf, MIC_CHUNK * 2 * 2, &got, pdMS_TO_TICKS(200));
            for (int i = 0; i < MIC_CHUNK * 2; i += 2) {
                if (buf[i] < lmn) { lmn = buf[i]; }
                if (buf[i] > lmx) { lmx = buf[i]; }
                if (buf[i+1] < rmn) { rmn = buf[i+1]; }
                if (buf[i+1] > rmx) { rmx = buf[i+1]; }
            }
            free(buf);
        }
        gpio_num_t d = (gpio_num_t)MIC_PIN_DIN; int pu = 0;
        gpio_set_pull_mode(d, GPIO_PULLUP_ONLY); ets_delay_us(200);
        for (int i = 0; i < 2000; i++) { pu += gpio_get_level(d); ets_delay_us(3); }
        gpio_set_pull_mode(d, GPIO_FLOATING);
        snprintf(l, sizeof l, "[mic] STEREO I2S (TDM yok, kodek ayar %s): BCLK=%lu Hz SDOUT kenar/s=%lu, pull-up'ta 1=%d/2000 -> %s; ham L %ld..%ld  R %ld..%ld",
                 cfg ? "ok" : "HATA", (unsigned long)sb, (unsigned long)ssd, pu, pu > 1900 ? "Hi-Z" : "SURULUYOR",
                 (long)lmn, (long)lmx, (long)rmn, (long)rmx);
        emit(l);
        i2s_channel_disable(rx); i2s_del_channel(rx); rx = NULL;
        s_variant = 0; es7210_setup();   /* TDM ayarına geri dön (master testi için) */
    } else { snprintf(l, sizeof l, "[mic] I2S std acilamadi: %s", esp_err_to_name(e)); emit(l); }

    /* 2) ES7210 master: ESP slave (MCLK hala ESP'den), kodek BCLK/WS uretiyor mu? */
    e = open_rx(I2S_ROLE_SLAVE, &rx);
    if (e != ESP_OK) { snprintf(l, sizeof l, "[mic] I2S slave acilamadi: %s", esp_err_to_name(e)); emit(l); return e; }
    uint8_t r08 = 0; i2c_bus_read_reg(A_ES7210, 0x08, &r08, 1);
    uint32_t f_mclk2 = count_hz(MIC_PIN_MCLK, 4);
    uint32_t idle_b = count_hz(MIC_PIN_BCLK, 20), idle_w = count_hz(MIC_PIN_WS, 20);
    /* Referans (esp_codec_dev es7210.c, master): REG08 bit0=1, REG03 bit7 = MCLK kaynağı
     * (0 = MCLK pedi, 1 = dahili clock doubler), REG02=C1 / REG04=01 / REG05=00 / REG07=20 (zaten yazılı),
     * ardından REG00 71→41 (durum makinesini yeniden başlat). İki kaynağı da dene. */
    uint8_t r03 = 0; i2c_bus_read_reg(A_ES7210, 0x03, &r03, 1);
    uint32_t m_b[2] = {0, 0}, m_w[2] = {0, 0}, m_sd[2] = {0, 0};
    for (int src = 0; src < 2; src++) {
        i2c_bus_write_reg(A_ES7210, 0x03, (uint8_t)((r03 & 0x7F) | (src ? 0x80 : 0x00)));
        i2c_bus_write_reg(A_ES7210, 0x08, (uint8_t)(r08 | 0x01));        /* bit0 = master */
        i2c_bus_write_reg(A_ES7210, 0x00, 0x71);
        i2c_bus_write_reg(A_ES7210, 0x00, 0x41);
        vTaskDelay(pdMS_TO_TICKS(150));
        m_b[src] = count_hz(MIC_PIN_BCLK, 20); m_w[src] = count_hz(MIC_PIN_WS, 20); m_sd[src] = count_hz(MIC_PIN_DIN, 20);
    }
    i2c_bus_write_reg(A_ES7210, 0x08, r08);                               /* geri al: slave */
    i2c_bus_write_reg(A_ES7210, 0x03, r03);
    i2c_bus_write_reg(A_ES7210, 0x00, 0x71); i2c_bus_write_reg(A_ES7210, 0x00, 0x41);
    snprintf(l, sizeof l, "[mic] ESP slave: MCLK=%lu Hz; ES7210 master oncesi BCLK=%lu WS=%lu; REG03=%02X REG08 %02X->%02X",
             (unsigned long)f_mclk2, (unsigned long)idle_b, (unsigned long)idle_w, r03, r08, r08 | 0x01);
    emit(l);
    snprintf(l, sizeof l, "[mic]   kaynak=MCLK pedi:  BCLK=%lu Hz WS=%lu Hz SDOUT=%lu   |   kaynak=doubler: BCLK=%lu Hz WS=%lu Hz SDOUT=%lu",
             (unsigned long)m_b[0], (unsigned long)m_w[0], (unsigned long)m_sd[0], (unsigned long)m_b[1], (unsigned long)m_w[1], (unsigned long)m_sd[1]);
    emit(l);
    emit((m_b[0] > 1000 || m_b[1] > 1000) ? "[mic] SONUC: ES7210 kendi saatini uretti -> MCLK kodege ULASIYOR, saat cekirdegi canli"
                                           : "[mic] SONUC: ES7210 master iken de BCLK yok -> MCLK kodek pinine ulasmiyor ya da cip olu");
    i2s_channel_disable(rx); i2s_del_channel(rx);
    return ESP_OK;
}
#endif /* TRACKER_FEATURES */
