/**
 * @file nabiz_alg.c
 * @brief Nabız + solunum + mırlama/soluma kestirimi — alg A3. Saf C; bkz. nabiz_alg.h ve tools/nabiz_ref.py.
 *
 * Bu dosyadaki her işlev nabiz_ref.py'deki aynı adlı işlevin karşılığıdır; sabitler de aynıdır.
 * Fark: burada süzgeçler ve iç çarpımlar float32 (kartın FPU'su), toplamlar double. Sonuçlar son hanede oynayabilir;
 * tools/nabiz_test.py iki uygulamayı toleransla karşılaştırır.
 */
#include "nabiz_alg.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ── sabitler (nabiz_ref.py ile aynı) ─────────────────────────────────────── */
#define R_ORTA          0.5
#define DOYGUN_ORAN     0.001
#define SINYAL_YOK_DB   (-90.0f)
#define TEPE_ORAN       0.80
#define V_ESIK          0.25
#define V_MIN_ARA       8
#define V_ALMASIK       0.30
#define V_YAYILIM       0.50
#define V_MIN_VURUS     6
#define V_BELIRGIN      4.5
#define KAT_ORAN        0.08
#define M_N             1024
#define M_ADIM          512
#define M_F0_MIN        18.0
#define M_F0_MAX        40.0
#define M_PAY           0.45
#define M_CERCEVE_ORAN  0.5
#define M_MIN_CERCEVE   4
#define S_LAG_MIN       20
#define S_LAG_MAX       150
#define S_MIN_N20       400
#define S_MIN_CEVRIM    2.5
#define S_DUZ           45
#define P_LAG_MIN       13
#define P_LAG_MAX       30
#define P_TREND         51
#define P_R             0.5
#define SIZINTI         0.10
#define PEN_MIN         3       /* pencere kuralı: en az net pencere (ayrıca pencerelerin en az 2/5'i) */
#define PEN_YAKIN       0.12    /* pencere kuralı: net pencereler ortancaya bu kadar yakın olmalı */
#define T_R             0.3     /* tutarlılık kuralı: bütün kayıt r alt sınırı */
#define T_MIN_PEN       4       /* tutarlılık kuralı: en az pencere */
#define T_YAKIN         0.06    /* tutarlılık kuralı: pencerelerin en az 4/5'i adaya bu kadar yakın olmalı */
#define T_R2T           0.15    /* tutarlılık kuralı: iki periyot sonraki r alt sınırı (üst sınır: r) */
#define T_BANT          0.06    /* tutarlılık kuralı: iki periyot çevresinde arama bandı */

static const nabiz_profil_t k_profil[] = {
    { "insan", 20.0f, 150.0f, 30, 171, 15, false, false },   /* 35-200/dk */
    { "kopek", 20.0f, 150.0f, 24, 150, 30, false, true  },   /* 40-250/dk */
    { "kedi",  30.0f, 300.0f, 20,  60, 30, true,  false },   /* 100-300/dk */
};
#define NPROFIL ((int)(sizeof k_profil / sizeof k_profil[0]))

int nabiz_profil_sayisi(void) { return NPROFIL; }
const nabiz_profil_t* nabiz_profil(int i) { return &k_profil[(i >= 0 && i < NPROFIL) ? i : 0]; }
int nabiz_profil_bul(const char* ad)
{
    if (!ad) return -1;
    for (int i = 0; i < NPROFIL; i++) if (strcasecmp(ad, k_profil[i].ad) == 0) return i;
    return -1;
}

/* ── süzgeç ───────────────────────────────────────────────────────────────── */
static void bq_tasarla(nabiz_bq_t* f, bool lp, double fc, double fs, double q)
{
    double w = 2.0 * M_PI * fc / fs, c = cos(w), sn = sin(w), al = sn / (2.0 * q), a0 = 1.0 + al;
    double b0 = lp ? (1.0 - c) / 2.0 / a0 : (1.0 + c) / 2.0 / a0;
    double b1 = lp ? (1.0 - c) / a0 : -(1.0 + c) / a0;
    f->b0 = (float)b0; f->b1 = (float)b1; f->b2 = (float)b0;
    f->a1 = (float)(-2.0 * c / a0); f->a2 = (float)((1.0 - al) / a0); f->z1 = 0; f->z2 = 0;
}
static inline float bq_adim(nabiz_bq_t* f, float x)   /* transposed direct form II */
{
    float y = f->b0 * x + f->z1; f->z1 = f->b1 * x - f->a1 * y + f->z2; f->z2 = f->b2 * x - f->a2 * y; return y;
}

/* ── akış ─────────────────────────────────────────────────────────────────── */
bool nabiz_akis_baslat(nabiz_akis_t* a, int saniye, int bytes, const nabiz_profil_t* p, const nabiz_ortam_t* o)
{
    memset(a, 0, sizeof *a);
    if (!o || !o->al || !o->birak || saniye < 1) return false;
    a->p = p ? p : &k_profil[0]; a->o = *o; a->bytes = bytes == 2 ? 2 : 3;
    a->esik = a->bytes == 2 ? 32734 : 8380218;
    a->cap1k = (uint32_t)saniye * 1000u + 16u; a->capb = (uint32_t)saniye * 100u + 16u;
    a->x1k  = (float*)o->al(a->cap1k * sizeof(float));
    a->envb = (float*)o->al(a->capb * sizeof(float));
    if (!a->x1k || !a->envb) { nabiz_akis_bitir(a); return false; }
    bq_tasarla(&a->hpb1, false, 200.0, 4000.0, 0.54119610);
    bq_tasarla(&a->hpb2, false, 200.0, 4000.0, 1.30656296);
    bq_tasarla(&a->lpb,  true, 1500.0, 4000.0, 0.7071);
    return true;
}

void nabiz_akis_bitir(nabiz_akis_t* a)
{
    if (!a || !a->o.birak) return;
    if (a->x1k)  a->o.birak(a->x1k);
    if (a->envb) a->o.birak(a->envb);
    a->x1k = NULL; a->envb = NULL;
}

void nabiz_akis_besle(nabiz_akis_t* a, const uint8_t* d, uint32_t len)
{
    const int bytes = a->bytes;
    const double tam = bytes == 2 ? 32768.0 : 8388608.0;
    for (uint32_t i = 0; i + (uint32_t)bytes <= len; i += (uint32_t)bytes) {
        int32_t v = bytes == 2 ? (int32_t)(int16_t)(d[i] | d[i + 1] << 8)
                               : (int32_t)((uint32_t)d[i] | (uint32_t)d[i + 1] << 8 | (uint32_t)d[i + 2] << 16);
        if (bytes == 3 && (v & 0x800000)) v -= 0x1000000;
        if (++a->n_ornek > (uint32_t)NABIZ_FS) {            /* kalite: ilk 1 sn hariç */
            int32_t av = v < 0 ? -v : v;
            if (av > a->tepe) a->tepe = av;
            if (av >= a->esik) a->n_doygun++;
            a->n_kalite++;
        }
        a->acc48 += v;
        if (++a->n48 == 48) {
            if (a->n1k < a->cap1k) a->x1k[a->n1k++] = (float)(a->acc48 / 48.0 / tam);
            a->acc48 = 0; a->n48 = 0;
        }
        a->acc12 += v;
        if (++a->n12 == 12) {
            float x = (float)(a->acc12 / 12.0 / tam); a->acc12 = 0; a->n12 = 0;
            x = bq_adim(&a->lpb, bq_adim(&a->hpb2, bq_adim(&a->hpb1, x)));
            float ax = fabsf(x);
            if (++a->n4k > 4000u) { a->sb_kare += (double)ax * ax; a->sb_n++; }
            a->bacc += ax;
            if (++a->bn == 40) {
                if (a->nb < a->capb) a->envb[a->nb++] = a->bacc / 40.0f;
                a->bacc = 0; a->bn = 0;
            }
        }
    }
}

/* ── temel yardımcılar ────────────────────────────────────────────────────── */
static inline void nefes(const nabiz_ortam_t* o) { if (o->nefes) o->nefes(); }

/* w (tek) genişlikli ortalama; kenarlarda eldeki komşuların ortalaması. Önek toplamı (double) ile. */
static bool duzlestir(const nabiz_ortam_t* o, const float* e, uint32_t n, int w, float* out)
{
    double* p = (double*)o->al(((size_t)n + 1) * sizeof(double));
    if (!p) return false;
    p[0] = 0;
    for (uint32_t i = 0; i < n; i++) p[i + 1] = p[i] + (double)e[i];
    const int y = w / 2;
    for (uint32_t i = 0; i < n; i++) {
        int lo = (int)i - y, hi = (int)i + y;
        if (lo < 0) lo = 0;
        if (hi > (int)n - 1) hi = (int)n - 1;
        out[i] = (float)((p[hi + 1] - p[lo]) / (double)(hi - lo + 1));
    }
    o->birak(p);
    return true;
}

static void ort_cikar(float* s, uint32_t n)
{
    double m = 0;
    for (uint32_t i = 0; i < n; i++) m += s[i];
    m /= (double)n;
    for (uint32_t i = 0; i < n; i++) s[i] = (float)((double)s[i] - m);
}

static int fcmp(const void* a, const void* b)
{
    float x = *(const float*)a, y = *(const float*)b;
    return (x > y) - (x < y);
}
/* Sıralı dizide ortanca ve yüzdelik (nabiz_ref.ortanca / yuzdelik ile aynı tanım) */
static inline float sirali_ortanca(const float* b, uint32_t n) { return 0.5f * (b[(n - 1) / 2] + b[n / 2]); }
static inline float sirali_yuzdelik(const float* b, uint32_t n, double q) { return b[(uint32_t)(q * (double)(n - 1))]; }

/* float iç çarpım; 256'lık parçalar double'da toplanır (hız + doğruluk) */
static double ic_carpim(const float* a, const float* b, uint32_t n)
{
    double t = 0; uint32_t i = 0;
    while (i < n) {
        uint32_t m = n - i; if (m > 256) m = 256;
        float s = 0;
        for (uint32_t k = 0; k < m; k++) s += a[i + k] * b[i + k];
        t += s; i += m;
    }
    return t;
}

typedef struct { int lo, hi; double* v; } ac_t;
#define AC(a, l) ((a)->v[(l) - (a)->lo])
static inline bool ac_var(const ac_t* a, int l) { return a->v && l >= a->lo && l <= a->hi; }

/* ac[lag] = Σ s[i]·s[i-lag] / Σ s², lag = lo..hi (s: ortalaması çıkarılmış). ac0 ≤ 0 ya da bellek yoksa false. */
static bool otokor(const nabiz_ortam_t* o, const float* s, uint32_t n, int lo, int hi, ac_t* ac)
{
    ac->v = NULL; ac->lo = 0; ac->hi = -1;
    if (n < 2) return false;
    double ac0 = ic_carpim(s, s, n);
    if (ac0 <= 0) return false;
    if (lo < 1) lo = 1;
    if (hi > (int)n - 1) hi = (int)n - 1;
    if (hi < lo) return false;
    ac->v = (double*)o->al((size_t)(hi - lo + 1) * sizeof(double));
    if (!ac->v) return false;
    ac->lo = lo; ac->hi = hi;
    for (int lag = lo; lag <= hi; lag++) {
        AC(ac, lag) = ic_carpim(s + lag, s, n - (uint32_t)lag) / ac0;
        if ((lag & 15) == 0) nefes(o);
    }
    return true;
}

/* En yüksek değerin TEPE_ORAN katına ulaşan en kısa gecikmeli yerel tepe + parabol ara değeri. Dönüş: tam gecikme (0 = yok). */
static int tepe_sec(const ac_t* ac, int lag_min, int lag_max, bool yalniz_ic, double* kes, double* r)
{
    *kes = 0; *r = 0;
    if (!ac->v) return 0;
    int a0 = lag_min > ac->lo ? lag_min : ac->lo, a1 = lag_max < ac->hi ? lag_max : ac->hi;
    if (a1 < a0) return 0;
    int gl = a0;
    for (int l = a0; l <= a1; l++) if (AC(ac, l) > AC(ac, gl)) gl = l;
    const double rmax = AC(ac, gl);
    int sec = 0;
    if (rmax > 0) {
        for (int l = a0; l <= a1; l++) {
            if (ac_var(ac, l - 1) && ac_var(ac, l + 1) && AC(ac, l) > AC(ac, l - 1) && AC(ac, l) >= AC(ac, l + 1)
                && AC(ac, l) >= TEPE_ORAN * rmax) { sec = l; break; }
        }
    }
    if (sec == 0) {
        if (yalniz_ic) return 0;
        sec = gl;
    }
    double k = (double)sec;
    if (ac_var(ac, sec - 1) && ac_var(ac, sec + 1)) {
        const double ym = AC(ac, sec - 1), y0 = AC(ac, sec), yp = AC(ac, sec + 1), payda = ym - 2.0 * y0 + yp;
        if (payda < 0) {
            const double d = 0.5 * (ym - yp) / payda;
            if (d > -1.0 && d < 1.0) k = (double)sec + d;
        }
    }
    *kes = k; *r = AC(ac, sec);
    return sec;
}

/* ── nabız ────────────────────────────────────────────────────────────────── */
/* Yöntem 1: otokorelasyon. e: kalp zarfı (100 Hz, ilk 1 sn atılmış).
 * ic (NULL olabilir): 1 = seçilen gecikme gerçek yerel tepe; 0 = yerel tepe yoktu, aralığın en yüksek değeri alındı
 * (çoğunlukla aralığın kenarı: yavaş dalgalanan gürültü). kes_c (NULL olabilir): kesirli gecikme (10 ms). */
static float nabiz_ac(const nabiz_ortam_t* o, const float* e, uint32_t n, int lag_min, int lag_max, float* r, float* r2,
                      int* ic, double* kes_c)
{
    *r = 0; *r2 = 0;
    if (ic) *ic = 0;
    if (kes_c) *kes_c = 0;
    if (n < 400) return 0;
    float* s = (float*)o->al(n * sizeof(float));
    if (!s) return 0;
    float bpm = 0; ac_t ac = { 0, -1, NULL };
    if (duzlestir(o, e, n, 5, s)) {
        ort_cikar(s, n);
        if (otokor(o, s, n, 1, lag_max + 1, &ac)) {
            double kes, rr; int sec = tepe_sec(&ac, lag_min, lag_max, false, &kes, &rr);
            if (sec > 0 && kes > 0) {
                bpm = (float)(6000.0 / kes); *r = (float)rr;
                const int h = sec / 2;
                *r2 = (h >= 1 && ac_var(&ac, h)) ? (float)AC(&ac, h) : 0.0f;
                if (ic) *ic = (ac_var(&ac, sec - 1) && ac_var(&ac, sec + 1) && AC(&ac, sec) > AC(&ac, sec - 1)
                               && AC(&ac, sec) >= AC(&ac, sec + 1)) ? 1 : 0;
                if (kes_c) *kes_c = kes;
            }
        }
    }
    if (ac.v) o->birak(ac.v);
    o->birak(s);
    return bpm;
}

/* Tutarlılık kuralı: zarf otokorelasyonunun iki periyot (2×kes, ±T_BANT) çevresindeki en yüksek değeri. Yoksa 0. */
static float r_iki_kat(const nabiz_ortam_t* o, const float* e, uint32_t n, double kes)
{
    if (n < 400 || kes <= 0) return 0;
    const int lo = (int)floor(2.0 * kes * (1.0 - T_BANT)), hi = (int)ceil(2.0 * kes * (1.0 + T_BANT));
    if (lo < 1 || hi > (int)(n / 2u)) return 0;
    float* s = (float*)o->al(n * sizeof(float));
    if (!s) return 0;
    float en = 0; ac_t ac = { 0, -1, NULL };
    if (duzlestir(o, e, n, 5, s)) {
        ort_cikar(s, n);
        if (otokor(o, s, n, lo, hi, &ac)) {
            double m = AC(&ac, ac.lo);
            for (int l = ac.lo + 1; l <= ac.hi; l++) if (AC(&ac, l) > m) m = AC(&ac, l);
            en = (float)m;
        }
    }
    if (ac.v) o->birak(ac.v);
    o->birak(s);
    return en;
}

typedef struct { float nabiz; int n; float yayilim; int iki_tepe; float belirgin; int guvenilir; } vurus_t;

/* Yöntem 2: vuruş tepeleri ve aralıkları. */
static vurus_t nabiz_vurus(const nabiz_ortam_t* o, const float* e, uint32_t n, int lag_min, int lag_max)
{
    vurus_t v = { 0, 0, 0, 0, 0, 0 };
    if (n < 400) return v;
    float* s  = (float*)o->al(n * sizeof(float));      /* düzleştirilmiş zarf */
    float* t  = (float*)o->al(n * sizeof(float));      /* sıralama / geçici */
    int* tepe = (int*)o->al(n * sizeof(int));
    if (!s || !t || !tepe || !duzlestir(o, e, n, 5, s)) goto cik;

    memcpy(t, s, n * sizeof(float)); qsort(t, n, sizeof(float), fcmp);
    const float med = sirali_ortanca(t, n), p98 = sirali_yuzdelik(t, n, 0.98);
    if (p98 <= med || med <= 0) goto cik;
    const float esik = med + (float)V_ESIK * (p98 - med);
    int m = 0;
    for (uint32_t i = 1; i + 1 < n; i++) {
        if (s[i] > esik && s[i] >= s[i - 1] && s[i] > s[i + 1]) {
            if (m > 0 && (int)i - tepe[m - 1] < V_MIN_ARA) { if (s[i] > s[tepe[m - 1]]) tepe[m - 1] = (int)i; }
            else tepe[m++] = (int)i;
        }
    }
    if (m < 5) goto cik;

    for (int k = 0; k < m; k++) t[k] = s[tepe[k]];
    qsort(t, (size_t)m, sizeof(float), fcmp);
    v.belirgin = sirali_ortanca(t, (uint32_t)m) / med;

    for (int k = 0; k < m - 1; k++) t[k] = (float)(tepe[k + 1] - tepe[k]);             /* i1 */
    qsort(t, (size_t)(m - 1), sizeof(float), fcmp);
    const float med1 = sirali_ortanca(t, (uint32_t)(m - 1));
    for (int k = 0; k < m - 2; k++) {
        int d = (tepe[k + 2] - tepe[k + 1]) - (tepe[k + 1] - tepe[k]);
        t[k] = (float)(d < 0 ? -d : d);
    }
    qsort(t, (size_t)(m - 2), sizeof(float), fcmp);
    const float alm = med1 > 0 ? sirali_ortanca(t, (uint32_t)(m - 2)) / med1 : 0.0f;
    v.iki_tepe = (alm > (float)V_ALMASIK && m >= 6) ? 1 : 0;

    int nc;
    if (v.iki_tepe) { nc = m - 2; for (int k = 0; k < nc; k++) t[k] = (float)(tepe[k + 2] - tepe[k]); }
    else            { nc = m - 1; for (int k = 0; k < nc; k++) t[k] = (float)(tepe[k + 1] - tepe[k]); }
    qsort(t, (size_t)nc, sizeof(float), fcmp);
    const float medc = sirali_ortanca(t, (uint32_t)nc);
    if (medc <= 0) { v.belirgin = 0; v.iki_tepe = 0; goto cik; }
    v.yayilim = (sirali_yuzdelik(t, (uint32_t)nc, 0.75) - sirali_yuzdelik(t, (uint32_t)nc, 0.25)) / medc;
    v.n = v.iki_tepe ? m / 2 : m;
    v.nabiz = 6000.0f / medc;
    {
        const double oran = (double)v.n / ((double)n / (double)medc);
        v.guvenilir = (v.n >= V_MIN_VURUS && (double)v.belirgin >= V_BELIRGIN && (double)v.yayilim <= V_YAYILIM
                       && oran >= 0.75 && oran <= 1.25 && medc >= (float)lag_min && medc <= (float)lag_max) ? 1 : 0;
    }
cik:
    if (s) o->birak(s);
    if (t) o->birak(t);
    if (tepe) o->birak(tepe);
    return v;
}

/* ── mırlama ──────────────────────────────────────────────────────────────── */
static void fft_1024(float* re, float* im)   /* yerinde, radix-2 */
{
    const int n = M_N;
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { float t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (int len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * M_PI / (double)len;
        const float wr = (float)cos(ang), wi = (float)sin(ang);
        for (int i = 0; i < n; i += len) {
            float cr = 1.0f, ci = 0.0f;
            for (int k = 0; k < len / 2; k++) {
                const int u = i + k, w = i + k + len / 2;
                const float xr = re[w] * cr - im[w] * ci, xi = re[w] * ci + im[w] * cr;
                re[w] = re[u] - xr; im[w] = im[u] - xi; re[u] += xr; im[u] += xi;
                const float nr = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = nr;
            }
        }
    }
}

/* xp: 15-150 Hz süzülmüş 1 kHz sinyal (ilk 1 sn atılmış). */
static int mirlama_bul(const nabiz_ortam_t* o, const float* xp, uint32_t n, float* hz, float* oran)
{
    *hz = 0; *oran = 0;
    if (n < (uint32_t)M_N) return 0;
    const double df = 1000.0 / (double)M_N;
    const int k_lo = (int)ceil(15.0 / df), k_hi = (int)floor(150.0 / df);
    const int f_lo = (int)ceil(M_F0_MIN / df), f_hi = (int)floor(M_F0_MAX / df);
    int gen[3];
    for (int h = 1; h <= 3; h++) gen[h - 1] = (int)floor(1.5 * h / df + 0.5);
    const uint32_t ncer = (n - (uint32_t)M_N) / (uint32_t)M_ADIM + 1u;
    float* re  = (float*)o->al(M_N * sizeof(float));
    float* im  = (float*)o->al(M_N * sizeof(float));
    float* pen = (float*)o->al(M_N * sizeof(float));
    float* f0  = (float*)o->al(ncer * sizeof(float));
    int var = 0;
    if (re && im && pen && f0) {
        for (int i = 0; i < M_N; i++) pen[i] = (float)(0.5 - 0.5 * cos(2.0 * M_PI * (double)i / (double)M_N));
        uint32_t top = 0, tonal = 0;
        for (uint32_t b = 0; b + (uint32_t)M_N <= n; b += (uint32_t)M_ADIM) {
            for (int i = 0; i < M_N; i++) { re[i] = xp[b + (uint32_t)i] * pen[i]; im[i] = 0; }
            fft_1024(re, im);
            for (int k = k_lo; k <= k_hi; k++) re[k] = re[k] * re[k] + im[k] * im[k];   /* güç, yalnız gereken bantta */
            double bant = 0;
            for (int k = k_lo; k <= k_hi; k++) bant += re[k];
            top++;
            if (bant > 0) {
                double en = 0; int en_k = 0;
                for (int k0 = f_lo; k0 <= f_hi; k0++) {
                    double parca[3], tarak = 0; int dolu = 0;
                    for (int h = 1; h <= 3; h++) {
                        int a = h * k0 - gen[h - 1], z = h * k0 + gen[h - 1];
                        if (a < k_lo) a = k_lo;
                        if (z > k_hi) z = k_hi;
                        double t = 0;
                        for (int k = a; k <= z; k++) t += re[k];
                        parca[h - 1] = t; tarak += t;
                    }
                    if (tarak <= 0) continue;
                    for (int h = 0; h < 3; h++) if (parca[h] >= 0.15 * tarak) dolu++;
                    if (dolu < 2) continue;                     /* tek çizgi (50 Hz şebeke vb.) mırlama değildir */
                    if (tarak / bant > en) { en = tarak / bant; en_k = k0; }
                }
                if (en >= M_PAY) f0[tonal++] = (float)(en_k * df);
            }
            nefes(o);
        }
        if (top > 0) {
            *oran = (float)tonal / (float)top;
            var = (tonal >= (uint32_t)M_MIN_CERCEVE && (double)*oran >= M_CERCEVE_ORAN) ? 1 : 0;
            if (tonal > 0) { qsort(f0, tonal, sizeof(float), fcmp); *hz = sirali_ortanca(f0, tonal); }
        }
    }
    if (re) o->birak(re);
    if (im) o->birak(im);
    if (pen) o->birak(pen);
    if (f0) o->birak(f0);
    return var;
}

/* ── soluma / solunum ─────────────────────────────────────────────────────── */
/* eb: solunum zarfı (100 Hz, ilk 1 sn atılmış). Dönüş hız (/dk), *r güven; bulunamazsa 0. */
static float soluma_bul(const nabiz_ortam_t* o, const float* eb, uint32_t n, float* r)
{
    *r = 0;
    if (n < 400) return 0;
    float* t = (float*)o->al(n * sizeof(float));
    float* s = (float*)o->al(n * sizeof(float));
    float hiz = 0; ac_t ac = { 0, -1, NULL };
    if (t && s && duzlestir(o, eb, n, P_TREND, t)) {
        for (uint32_t i = 0; i < n; i++) t[i] = eb[i] - t[i];
        if (duzlestir(o, t, n, 3, s)) {
            ort_cikar(s, n);
            if (otokor(o, s, n, P_LAG_MIN - 1, P_LAG_MAX + 1, &ac)) {
                double kes, rr;
                if (tepe_sec(&ac, P_LAG_MIN, P_LAG_MAX, true, &kes, &rr) > 0 && kes > 0) { hiz = (float)(6000.0 / kes); *r = (float)rr; }
            }
        }
    }
    if (ac.v) o->birak(ac.v);
    if (t) o->birak(t);
    if (s) o->birak(s);
    return hiz;
}

/* eb: solunum zarfı (100 Hz). kalp_periyot: 10 ms birim, 0 = bilinmiyor. durum: KISA | BELIRSIZ | OK. */
static float solunum_bul(const nabiz_ortam_t* o, const float* eb, uint32_t n, double kalp_periyot, float* r, char* durum, size_t dn)
{
    *r = 0; snprintf(durum, dn, "KISA");
    if (n / 5u < (uint32_t)S_MIN_N20) return 0;
    int w = S_DUZ;
    if (kalp_periyot > 0) w = (int)floor(ceil((double)S_DUZ / kalp_periyot) * kalp_periyot + 0.5);
    if (w % 2 == 0) w += 1;
    const uint32_t n20 = n / 5u;
    float* d = (float*)o->al(n * sizeof(float));
    float* b = (float*)o->al(n20 * sizeof(float));
    float rr_dk = 0; ac_t ac = { 0, -1, NULL };
    snprintf(durum, dn, "BELIRSIZ");
    if (d && b && duzlestir(o, eb, n, w, d)) {
        for (uint32_t i = 0; i < n20; i++) {
            double t = 0;
            for (int k = 0; k < 5; k++) t += d[i * 5u + (uint32_t)k];
            b[i] = (float)(t / 5.0);
        }
        ort_cikar(b, n20);
        if (otokor(o, b, n20, S_LAG_MIN - 1, S_LAG_MAX + 1, &ac)) {
            double kes, rr; int sec = tepe_sec(&ac, S_LAG_MIN, S_LAG_MAX, true, &kes, &rr);
            if (sec > 0 && kes > 0) {
                rr_dk = (float)(1200.0 / kes); *r = (float)rr;
                if ((double)n20 < S_MIN_CEVRIM * (double)sec) snprintf(durum, dn, "KISA");
                else snprintf(durum, dn, "%s", rr >= R_ORTA ? "OK" : "BELIRSIZ");
            }
        }
    }
    if (ac.v) o->birak(ac.v);
    if (d) o->birak(d);
    if (b) o->birak(b);
    return rr_dk;
}

static inline bool yakin(double a, double b, double oran) { return b > 0 && fabs(a - b) <= oran * b; }
static inline float db20(double a) { return a > 0 ? (float)(20.0 * log10(a)) : -120.0f; }

/* A3 koruması: aday, eşlenmemiş (kalp periyoduna göre düzleştirilmemiş) solunum hızına ya da iki katına yakınsa bulunan
 * ritim büyük olasılıkla nefes sesidir. Solunum bir kez hesaplanır (c: önbellek). */
typedef struct { bool hazir, ok; float rr; } nefes0_t;
static bool nefes_gibi(const nabiz_ortam_t* o, const float* eb, uint32_t neb, nefes0_t* c, double aday)
{
    if (!c->hazir) {
        float rs; char sd[16];
        c->rr = solunum_bul(o, eb, neb, 0.0, &rs, sd, sizeof sd);
        c->ok = strcmp(sd, "OK") == 0; c->hazir = true;
    }
    return c->ok && (yakin(aday, (double)c->rr, SIZINTI) || yakin(aday, 2.0 * (double)c->rr, SIZINTI));
}

/* ── tam analiz ───────────────────────────────────────────────────────────── */
bool nabiz_analiz(nabiz_akis_t* a, nabiz_sonuc_t* out)
{
    const nabiz_ortam_t* o = &a->o; const nabiz_profil_t* p = a->p;
    memset(out, 0, sizeof *out);
    out->p = p; snprintf(out->durum, sizeof out->durum, "KISA"); snprintf(out->sdurum, sizeof out->sdurum, "KISA");
    snprintf(out->yontem, sizeof out->yontem, "ac");
    const double tam = a->bytes == 2 ? 32768.0 : 8388608.0;
    out->doygun = a->n_doygun; out->n_kalite = a->n_kalite;
    out->tepe_db = db20((double)a->tepe / tam);
    out->sseviye_db = db20(a->sb_n ? sqrt(a->sb_kare / (double)a->sb_n) : 0.0);

    const uint32_t n1 = a->n1k, nh = n1 / 10u;
    float* y    = (float*)o->al((n1 ? n1 : 1u) * sizeof(float));
    float* envh = (float*)o->al((nh ? nh : 1u) * sizeof(float));
    if (!y || !envh) { if (y) o->birak(y); if (envh) o->birak(envh); return false; }

    /* kalp zarfı */
    {
        nabiz_bq_t hp, lp; bq_tasarla(&hp, false, p->hp, 1000.0, 0.7071); bq_tasarla(&lp, true, p->lp, 1000.0, 0.7071);
        double kare = 0; uint32_t kn = 0;
        for (uint32_t i = 0; i < n1; i++) {
            const float v = fabsf(bq_adim(&lp, bq_adim(&hp, a->x1k[i])));
            y[i] = v;
            if (i >= 1000u) { kare += (double)v * v; kn++; }
            if ((i & 8191u) == 8191u) nefes(o);
        }
        out->seviye_db = db20(kn ? sqrt(kare / (double)kn) : 0.0);
        for (uint32_t i = 0; i < nh; i++) {
            double t = 0;
            for (int k = 0; k < 10; k++) t += y[i * 10u + (uint32_t)k];
            envh[i] = (float)(t / 10.0);
        }
    }
    const float* e = envh + 100; const uint32_t ne = nh > 100u ? nh - 100u : 0u;
    const float* eb = a->envb + 100; const uint32_t neb = a->nb > 100u ? a->nb - 100u : 0u;
    out->sure = (float)ne / 100.0f;

    float r = 0, r2 = 0; int ic1 = 0; double kes1 = 0;
    const float bpm1 = nabiz_ac(o, e, ne, p->lag_min, p->lag_max, &r, &r2, &ic1, &kes1);
    const vurus_t v2 = nabiz_vurus(o, e, ne, p->lag_min, p->lag_max);
    /* A3 geçici dizileri yığında değil ortam belleğinde (kartta görev yığını dar): iyi[] + pen_ic[] */
    float*   iyi    = (float*)o->al(NABIZ_PENCERE_MAX * sizeof(float));
    uint8_t* pen_ic = (uint8_t*)o->al(NABIZ_PENCERE_MAX);
    if (!iyi || !pen_ic) { if (iyi) o->birak(iyi); if (pen_ic) o->birak(pen_ic); o->birak(y); o->birak(envh); return false; }
    for (uint32_t k = 0; k + 800u <= ne && out->pencere_n < NABIZ_PENCERE_MAX; k += 400u) {
        float wr, wr2; int wic;
        out->pencere_bpm[out->pencere_n] = nabiz_ac(o, e + k, 800u, p->lag_min, p->lag_max, &wr, &wr2, &wic, NULL);
        out->pencere_r[out->pencere_n] = wr; pen_ic[out->pencere_n] = (uint8_t)wic; out->pencere_n++;
    }

    /* A3: pencere kuralı (net pencerelerin ortancası) ve tutarlılık kuralı (pencereler adayı doğruluyor mu) */
    const int npen = out->pencere_n;
    const float r2t = bpm1 > 0 ? r_iki_kat(o, e, ne, kes1) : 0.0f;
    float pen_nabiz = 0; int pen_iyi = 0, pen_uyan = 0;
    {
        for (int k = 0; k < npen; k++)
            if (out->pencere_bpm[k] > 0 && pen_ic[k] && (double)out->pencere_r[k] >= R_ORTA) iyi[pen_iyi++] = out->pencere_bpm[k];
        int gerek = (2 * npen + 4) / 5;
        if (gerek < PEN_MIN) gerek = PEN_MIN;
        if (pen_iyi >= gerek) {
            qsort(iyi, (size_t)pen_iyi, sizeof(float), fcmp);
            const double med = (double)sirali_ortanca(iyi, (uint32_t)pen_iyi);
            bool hepsi = true;
            for (int k = 0; k < pen_iyi; k++) if (fabs((double)iyi[k] - med) > PEN_YAKIN * med) { hepsi = false; break; }
            if (hepsi) pen_nabiz = (float)med;
        }
        for (int k = 0; k < npen; k++)
            if (out->pencere_bpm[k] > 0 && bpm1 > 0 && fabs((double)out->pencere_bpm[k] - (double)bpm1) <= T_YAKIN * (double)bpm1) pen_uyan++;
    }
    const bool tutarli = ic1 && (double)r >= T_R && npen >= T_MIN_PEN && pen_uyan >= (4 * npen + 4) / 5
                         && (double)r2t >= T_R2T && r2t <= r;
    nefes0_t n0 = { false, false, 0 };
    o->birak(iyi); o->birak(pen_ic);

    /* mırlama (kedi): x1k → 15-150 Hz (y tamponu yeniden kullanılır) */
    int mir = 0;
    if (p->mirlama && n1 > 1000u) {
        nabiz_bq_t hp, lp; bq_tasarla(&hp, false, 15.0, 1000.0, 0.7071); bq_tasarla(&lp, true, 150.0, 1000.0, 0.7071);
        for (uint32_t i = 0; i < n1; i++) y[i] = bq_adim(&lp, bq_adim(&hp, a->x1k[i]));
        mir = mirlama_bul(o, y + 1000, n1 - 1000u, &out->mirlama_hz, &out->mirlama_oran);
    }
    float sol_r = 0; const float sol_hiz = p->soluma ? soluma_bul(o, eb, neb, &sol_r) : 0.0f;

    /* nabız kararı */
    float nabiz = bpm1; const char* durum; const char* yontem = "ac";
    if (ne < 400u) durum = "KISA";
    else if (a->tepe == 0 || out->seviye_db < SINYAL_YOK_DB) durum = "SINYAL_YOK";
    else if (a->n_kalite && (double)a->n_doygun > DOYGUN_ORAN * (double)a->n_kalite) durum = "DOYGUN";
    else if (bpm1 <= 0) durum = "BELIRSIZ";
    else if (mir) durum = "MIRLAMA";
    else if ((double)r >= R_ORTA) {
        const double oran = v2.guvenilir ? (double)v2.nabiz / (double)bpm1 : 0.0;
        const bool kat = fabs(oran - 2.0) <= 2.0 * KAT_ORAN || fabs(oran - 0.5) <= 0.5 * KAT_ORAN;
        durum = kat ? "KAT_SUPHESI" : "OK";
    }
    else if (v2.guvenilir) { durum = "OK"; nabiz = v2.nabiz; yontem = "vurus"; }
    else if (pen_nabiz > 0 && !nefes_gibi(o, eb, neb, &n0, (double)pen_nabiz)) { durum = "OK"; nabiz = pen_nabiz; yontem = "pencere"; }
    else if (tutarli && !nefes_gibi(o, eb, neb, &n0, (double)bpm1)) { durum = "OK"; yontem = "tutarli"; }
    else durum = "BELIRSIZ";
    const bool ok = strcmp(durum, "OK") == 0;

    /* soluma (köpek) */
    int soluma = 0;
    if (p->soluma && (double)sol_r >= P_R)
        soluma = (ok && (yakin(sol_hiz, nabiz, SIZINTI) || yakin(sol_hiz, 2.0 * nabiz, SIZINTI))) ? 0 : 1;

    /* solunum */
    const double kp = (ok && nabiz > 0) ? 6000.0 / (double)nabiz : ((bpm1 > 0 && r >= 0.3f) ? 6000.0 / (double)bpm1 : 0.0);
    float rs = 0; char sd[16];
    const float rr = solunum_bul(o, eb, neb, kp, &rs, sd, sizeof sd);
    if (strcmp(sd, "OK") == 0) {
        if (mir) snprintf(sd, sizeof sd, "MIRLAMA");
        else if (soluma) snprintf(sd, sizeof sd, "SOLUMA");
        else if (ok && yakin(rr, nabiz, SIZINTI)) snprintf(sd, sizeof sd, "KALP_SIZINTISI");
        else if (!ok && (double)rr >= 6000.0 / (double)p->lag_max) snprintf(sd, sizeof sd, "BELIRSIZ");
    } else if (strcmp(sd, "BELIRSIZ") == 0 && soluma) snprintf(sd, sizeof sd, "SOLUMA");

    snprintf(out->durum, sizeof out->durum, "%s", durum);
    snprintf(out->yontem, sizeof out->yontem, "%s", yontem);
    snprintf(out->sdurum, sizeof out->sdurum, "%s", sd);
    out->nabiz = nabiz; out->r = r; out->r2 = r2; out->nabiz_ac = bpm1; out->nabiz_vurus = v2.nabiz;
    out->nvurus = v2.n; out->yayilim = v2.yayilim; out->belirgin = v2.belirgin; out->iki_tepe = v2.iki_tepe; out->vguven = v2.guvenilir;
    out->solunum = rr; out->rs = rs; out->mirlama = mir; out->soluma = soluma; out->soluma_hiz = sol_hiz; out->soluma_r = sol_r;
    out->pen_iyi = pen_iyi; out->pen_uyan = pen_uyan; out->r2t = r2t;

    o->birak(y); o->birak(envh);
    return true;
}

int nabiz_satir(const nabiz_sonuc_t* s, char* buf, size_t n)
{
    return snprintf(buf, n,
        "alg=%s profil=%s durum=%s nabiz=%.0f r=%.2f r2=%.2f yontem=%s nabiz_ac=%.0f nabiz_vurus=%.0f nvurus=%d "
        "yayilim=%.2f belirgin=%.1f vguven=%d solunum=%.0f rs=%.2f sdurum=%s mirlama=%d soluma=%d doygun=%lu "
        "tepe=%.1f seviye=%.1f sseviye=%.1f sure=%.1f pen_n=%d pen_iyi=%d pen_uyan=%d r2t=%.2f",
        NABIZ_ALG, s->p ? s->p->ad : "?", s->durum, (double)s->nabiz, (double)s->r, (double)s->r2, s->yontem,
        (double)s->nabiz_ac, (double)s->nabiz_vurus, s->nvurus, (double)s->yayilim, (double)s->belirgin, s->vguven,
        (double)s->solunum, (double)s->rs, s->sdurum, s->mirlama, s->soluma, (unsigned long)s->doygun,
        (double)s->tepe_db, (double)s->seviye_db, (double)s->sseviye_db, (double)s->sure,
        s->pencere_n, s->pen_iyi, s->pen_uyan, (double)s->r2t);
}
