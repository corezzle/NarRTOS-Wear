/**
 * @file unistroke.cpp
 * @brief $1 Unistroke Recognizer — minimal, bağımsız, matematiksel template'ler.
 *
 * Bellek: yaklaşık 36 template × 32 nokta × 4 byte (2×int16) ≈ 4.6 KB.
 * Süre: 240 MHz ESP32-S3'te classify yaklaşık 10-30 ms (template sayısına göre).
 *
 * NOT: Algoritma path'in ZAMANSAL sırasına duyarlıdır. "O" harfini saat
 * yönünde çizerseniz ters yönde çizenle eşleşmez; templates burada saat
 * yönünü beklediği için kullanıcı aynı yönde çizmeli.
 */
#include "unistroke.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <esp_log.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char* TAG = "Unistroke";

/* ==== Parametreler ======================================================== */
#define N_POINTS       32      /* her stroke bu kadar noktaya resample edilir */
#define SQ_SIZE        100.0f  /* normalize edilmiş kutu kenarı */
#define MAX_TEMPLATES  80
#define ACCEPT_THRESH  38.0f   /* bu eşiğin ÜSTÜ → belirsiz (reddedilir).
                                * 32 → 38: multi-stroke harflerde kullanıcı çizimi
                                * template'ten biraz daha fazla kayabildiği için
                                * hafifçe gevşetildi; yine de yabancı şekilleri
                                * ayıklamaya yeterli. */

/* $1 "best angle" için altın oran araması */
#define ANGLE_RANGE_DEG    45.0f
#define ANGLE_DELTA_DEG    2.0f

/* ==== Template deposu ===================================================== */
typedef struct {
    char    ch;
    float   pts[N_POINTS * 2];  /* [x0,y0,x1,y1,...], normalize edilmiş */
} template_t;

static template_t s_templates[MAX_TEMPLATES];
static int        s_tmpl_count = 0;
static bool       s_tmpl_ready = false;

/* ==== Geometrik yardımcılar =============================================== */
static float path_length(const unistroke_point_t* pts, size_t n)
{
    float total = 0;
    for (size_t i = 1; i < n; i++) {
        float dx = pts[i].x - pts[i-1].x;
        float dy = pts[i].y - pts[i-1].y;
        total += sqrtf(dx*dx + dy*dy);
    }
    return total;
}

/* Ham stroke'u tam olarak N_POINTS noktaya resample et. */
static void resample(const unistroke_point_t* pts, size_t n, float out[N_POINTS * 2])
{
    if (n < 2) {
        for (int i = 0; i < N_POINTS; i++) {
            out[i*2]   = n ? pts[0].x : 0;
            out[i*2+1] = n ? pts[0].y : 0;
        }
        return;
    }

    float I = path_length(pts, n) / (N_POINTS - 1);
    if (I <= 0.0f) I = 1.0f;

    float D = 0;
    float curx = pts[0].x, cury = pts[0].y;
    out[0] = curx; out[1] = cury;
    int out_idx = 1;

    /* i: kaynak indeksi (0'dan başlar) */
    size_t i = 1;
    /* Her iterasyonda ya yeni nokta üret, ya da sonraki kaynak noktasına geç. */
    while (i < n && out_idx < N_POINTS) {
        float dx = pts[i].x - curx;
        float dy = pts[i].y - cury;
        float d = sqrtf(dx*dx + dy*dy);

        if ((D + d) >= I) {
            float t = (I - D) / d;
            float qx = curx + t * dx;
            float qy = cury + t * dy;
            out[out_idx*2]     = qx;
            out[out_idx*2 + 1] = qy;
            out_idx++;
            curx = qx; cury = qy;
            D = 0;
            /* i ilerletme — mevcut segmentte yeni başlangıçtan devam */
        } else {
            D += d;
            curx = pts[i].x;
            cury = pts[i].y;
            i++;
        }
    }
    /* Yuvarlama nedeniyle son noktayı alamadıysak kopyala */
    while (out_idx < N_POINTS) {
        out[out_idx*2]     = out[(out_idx-1)*2];
        out[out_idx*2 + 1] = out[(out_idx-1)*2 + 1];
        out_idx++;
    }
}

static void centroid(const float* pts, float* cx, float* cy)
{
    float sx = 0, sy = 0;
    for (int i = 0; i < N_POINTS; i++) {
        sx += pts[i*2];
        sy += pts[i*2 + 1];
    }
    *cx = sx / N_POINTS;
    *cy = sy / N_POINTS;
}

static void bounding_box(const float* pts, float* minx, float* miny,
                         float* maxx, float* maxy)
{
    *minx = *maxx = pts[0];
    *miny = *maxy = pts[1];
    for (int i = 1; i < N_POINTS; i++) {
        float x = pts[i*2], y = pts[i*2 + 1];
        if (x < *minx) *minx = x;
        if (x > *maxx) *maxx = x;
        if (y < *miny) *miny = y;
        if (y > *maxy) *maxy = y;
    }
}

/* Stroke'u indicative angle (centroid→ilk nokta) kadar geri döndür. */
static void rotate_to_zero(float* pts)
{
    float cx, cy;
    centroid(pts, &cx, &cy);
    float angle = atan2f(cy - pts[1], cx - pts[0]);
    float c = cosf(-angle), s = sinf(-angle);
    for (int i = 0; i < N_POINTS; i++) {
        float x = pts[i*2] - cx;
        float y = pts[i*2 + 1] - cy;
        pts[i*2]     = x*c - y*s + cx;
        pts[i*2 + 1] = x*s + y*c + cy;
    }
}

static void scale_to_square(float* pts, float size)
{
    float minx, miny, maxx, maxy;
    bounding_box(pts, &minx, &miny, &maxx, &maxy);
    float w = maxx - minx, h = maxy - miny;
    if (w < 1e-3f) w = 1;
    if (h < 1e-3f) h = 1;
    for (int i = 0; i < N_POINTS; i++) {
        pts[i*2]     = pts[i*2]     * size / w;
        pts[i*2 + 1] = pts[i*2 + 1] * size / h;
    }
}

static void translate_to_origin(float* pts)
{
    float cx, cy;
    centroid(pts, &cx, &cy);
    for (int i = 0; i < N_POINTS; i++) {
        pts[i*2]     -= cx;
        pts[i*2 + 1] -= cy;
    }
}

static void rotate_by(const float* in, float* out, float angle)
{
    float cx, cy;
    centroid(in, &cx, &cy);
    float c = cosf(angle), s = sinf(angle);
    for (int i = 0; i < N_POINTS; i++) {
        float x = in[i*2]     - cx;
        float y = in[i*2 + 1] - cy;
        out[i*2]     = x*c - y*s + cx;
        out[i*2 + 1] = x*s + y*c + cy;
    }
}

static float path_distance(const float* a, const float* b)
{
    float sum = 0;
    for (int i = 0; i < N_POINTS; i++) {
        float dx = a[i*2]     - b[i*2];
        float dy = a[i*2 + 1] - b[i*2 + 1];
        sum += sqrtf(dx*dx + dy*dy);
    }
    return sum / N_POINTS;
}

/* Altın oran araması ile ±ANGLE_RANGE_DEG aralığında en iyi dönüş açısı. */
static float distance_at_best_angle(const float* pts, const float* tmpl)
{
    const float theta  = ANGLE_RANGE_DEG * (float)M_PI / 180.0f;
    const float dtheta = ANGLE_DELTA_DEG * (float)M_PI / 180.0f;
    const float phi    = 0.5f * (-1.0f + sqrtf(5.0f));

    float a = -theta, b = theta;
    float x1 = phi * a + (1 - phi) * b;
    float x2 = (1 - phi) * a + phi * b;

    float buf[N_POINTS * 2];

    rotate_by(pts, buf, x1);
    float f1 = path_distance(buf, tmpl);

    rotate_by(pts, buf, x2);
    float f2 = path_distance(buf, tmpl);

    while (fabsf(b - a) > dtheta) {
        if (f1 < f2) {
            b = x2; x2 = x1; f2 = f1;
            x1 = phi * a + (1 - phi) * b;
            rotate_by(pts, buf, x1);
            f1 = path_distance(buf, tmpl);
        } else {
            a = x1; x1 = x2; f1 = f2;
            x2 = (1 - phi) * a + phi * b;
            rotate_by(pts, buf, x2);
            f2 = path_distance(buf, tmpl);
        }
    }
    return f1 < f2 ? f1 : f2;
}

/* ==== Template üretimi ==================================================== */

static void normalize_template(float* pts)
{
    rotate_to_zero(pts);
    scale_to_square(pts, SQ_SIZE);
    translate_to_origin(pts);
}

static void add_template(char ch, const unistroke_point_t* raw, size_t n)
{
    if (s_tmpl_count >= MAX_TEMPLATES) return;
    template_t* t = &s_templates[s_tmpl_count++];
    t->ch = ch;
    resample(raw, n, t->pts);
    normalize_template(t->pts);
}

/* Parametrik eğri helper'ları ============================================== */

/* Düz çizgi: (x1,y1) → (x2,y2), N örnekli */
static void line_stroke(char ch, float x1, float y1, float x2, float y2)
{
    const int N = 16;
    unistroke_point_t p[N];
    for (int i = 0; i < N; i++) {
        float t = (float)i / (N - 1);
        p[i].x = (int16_t)(x1 + (x2 - x1) * t);
        p[i].y = (int16_t)(y1 + (y2 - y1) * t);
    }
    add_template(ch, p, N);
}

/* İki segmentli çizgi: (x1,y1) → (x2,y2) → (x3,y3) */
static void two_seg(char ch, float x1, float y1, float x2, float y2,
                    float x3, float y3)
{
    const int N = 24;
    unistroke_point_t p[N];
    for (int i = 0; i < N; i++) {
        float t = (float)i / (N - 1);
        float x, y;
        if (t < 0.5f) {
            float tt = t * 2;
            x = x1 + (x2 - x1) * tt;
            y = y1 + (y2 - y1) * tt;
        } else {
            float tt = (t - 0.5f) * 2;
            x = x2 + (x3 - x2) * tt;
            y = y2 + (y3 - y2) * tt;
        }
        p[i].x = (int16_t)x;
        p[i].y = (int16_t)y;
    }
    add_template(ch, p, N);
}

/* Üç segmentli çizgi */
static void three_seg(char ch,
                      float x1, float y1, float x2, float y2,
                      float x3, float y3, float x4, float y4)
{
    const int N = 30;
    unistroke_point_t p[N];
    for (int i = 0; i < N; i++) {
        float t = (float)i / (N - 1) * 3;
        float x, y;
        if (t < 1) {
            x = x1 + (x2 - x1) * t;
            y = y1 + (y2 - y1) * t;
        } else if (t < 2) {
            float tt = t - 1;
            x = x2 + (x3 - x2) * tt;
            y = y2 + (y3 - y2) * tt;
        } else {
            float tt = t - 2;
            x = x3 + (x4 - x3) * tt;
            y = y3 + (y4 - y3) * tt;
        }
        p[i].x = (int16_t)x;
        p[i].y = (int16_t)y;
    }
    add_template(ch, p, N);
}

/* Yay: (cx,cy) merkezli, r yarıçaplı, a1→a2 radyan */
static void arc_stroke(char ch, float cx, float cy, float r,
                       float a1, float a2)
{
    const int N = 24;
    unistroke_point_t p[N];
    for (int i = 0; i < N; i++) {
        float t = (float)i / (N - 1);
        float a = a1 + (a2 - a1) * t;
        p[i].x = (int16_t)(cx + r * cosf(a));
        p[i].y = (int16_t)(cy + r * sinf(a));
    }
    add_template(ch, p, N);
}

/* Yol (path) — keyfi köşe dizisinden template üretir.
 * Kullanıcı iki ya da daha fazla stroke'la çizdiğinde, stroke'lar arası
 * "pen-up teleport" bizim classify buffer'ımızda ardışık iki nokta
 * arasında uzun bir segment olarak görünür. O yüzden multi-stroke harfleri
 * (H/T/t/i/x/f...) tanımlarken o teleport'u da düz çizgi olarak ekliyoruz.
 * Her segmenti N_SEG nokta ile örnekler; sonra resample() arc-length'e göre
 * N_POINTS'e indirger.
 *
 * @param ch       template karakteri (tanımanın döndüreceği sonuç)
 * @param verts    (x,y) vertex dizisi, n_vert adet
 * @param n_vert   vertex sayısı (>=2)
 */
static void path_template(char ch, const float* verts, int n_vert)
{
    if (n_vert < 2) return;
    const int PER_SEG = 8;
    const int n_seg   = n_vert - 1;
    const int N       = n_seg * PER_SEG;
    if (N > 96) return;  /* güvenlik */
    unistroke_point_t p[96];
    int idx = 0;
    for (int s = 0; s < n_seg; s++) {
        float x1 = verts[2*s], y1 = verts[2*s + 1];
        float x2 = verts[2*(s+1)], y2 = verts[2*(s+1) + 1];
        for (int i = 0; i < PER_SEG; i++) {
            float t = (float)i / (PER_SEG - 1);
            p[idx].x = (int16_t)(x1 + (x2 - x1) * t);
            p[idx].y = (int16_t)(y1 + (y2 - y1) * t);
            idx++;
        }
    }
    add_template(ch, p, idx);
}

/* ==== Template seti =======================================================
 * Not: Kullanıcı her karakteri BURADA TANIMLANAN yönde çizmeli.
 * (y aşağı pozitif — ekran koordinatı.)
 */
static void init_templates(void)
{
    if (s_tmpl_ready) return;

    /* --- Rakamlar ------------------------------------------------------- */
    /* 0 / o  — saat yönünde oval (üst-orta'dan başla) */
    arc_stroke('0', 50, 50, 40, -(float)M_PI/2, (float)(3*M_PI/2));
    arc_stroke('o', 50, 50, 40, -(float)M_PI/2, (float)(3*M_PI/2));

    /* 1 — yukarıdan aşağı düz çizgi */
    line_stroke('1', 50, 10, 50, 90);

    /* 2 — tepe yayı + çapraz aşağı + alt çizgi */
    {
        const int N = 28;
        unistroke_point_t p[N];
        int idx = 0;
        /* üst yay */
        for (int i = 0; i < 12; i++, idx++) {
            float a = (float)M_PI + (float)M_PI * i / 11;
            p[idx].x = (int16_t)(40 + 25 * cosf(a));
            p[idx].y = (int16_t)(30 + 25 * sinf(a));
        }
        /* çapraz aşağı */
        for (int i = 0; i < 8; i++, idx++) {
            float t = (float)i / 7;
            p[idx].x = (int16_t)(65 - 50 * t);
            p[idx].y = (int16_t)(30 + 60 * t);
        }
        /* alt yatay */
        for (int i = 0; i < 8; i++, idx++) {
            float t = (float)i / 7;
            p[idx].x = (int16_t)(15 + 70 * t);
            p[idx].y = 90;
        }
        add_template('2', p, N);
    }

    /* 3 — iki yarı yay */
    {
        const int N = 28;
        unistroke_point_t p[N];
        for (int i = 0; i < 14; i++) {
            float a = (float)(1.2*M_PI) + (float)(1.6*M_PI) * i / 13;
            p[i].x = (int16_t)(50 + 22 * cosf(a));
            p[i].y = (int16_t)(30 + 22 * sinf(a));
        }
        for (int i = 0; i < 14; i++) {
            float a = (float)(1.2*M_PI) + (float)(1.6*M_PI) * i / 13;
            p[14 + i].x = (int16_t)(50 + 22 * cosf(a));
            p[14 + i].y = (int16_t)(70 + 22 * sinf(a));
        }
        add_template('3', p, N);
    }

    /* 4 — çapraz aşağı, yatay, aşağı (iki segment benzeri, üç segment olarak) */
    three_seg('4', 70, 10, 20, 55, 80, 55, 80, 90);

    /* 5 — üst yatay, sol-aşağı, sağ alt yay */
    {
        const int N = 32;
        unistroke_point_t p[N];
        int idx = 0;
        /* üst yatay */
        for (int i = 0; i < 8; i++, idx++) {
            p[idx].x = (int16_t)(65 - 50 * (float)i/7);
            p[idx].y = 15;
        }
        /* sol aşağı */
        for (int i = 0; i < 8; i++, idx++) {
            p[idx].x = 15;
            p[idx].y = (int16_t)(15 + 35 * (float)i/7);
        }
        /* alt yay */
        for (int i = 0; i < 16; i++, idx++) {
            float a = (float)M_PI + (float)M_PI * i / 15;
            p[idx].x = (int16_t)(40 + 25 * cosf(a));
            p[idx].y = (int16_t)(70 + 25 * sinf(a));
        }
        add_template('5', p, N);
    }

    /* 6 — üstten çapraz aşağı, alttan kapalı daire */
    {
        const int N = 28;
        unistroke_point_t p[N];
        /* çapraz üstten alt loopa */
        for (int i = 0; i < 12; i++) {
            float t = (float)i / 11;
            p[i].x = (int16_t)(70 - 45 * t);
            p[i].y = (int16_t)(10 + 55 * t);
        }
        /* alt daire */
        for (int i = 0; i < 16; i++) {
            float a = (float)M_PI + (float)(2*M_PI) * i / 15;
            p[12 + i].x = (int16_t)(45 + 22 * cosf(a));
            p[12 + i].y = (int16_t)(68 + 22 * sinf(a));
        }
        add_template('6', p, N);
    }

    /* 7 — üst yatay + çapraz aşağı */
    two_seg('7', 15, 15, 85, 15, 30, 90);

    /* 8 — üst daire sonra alt daire (iki halka basitleştirme) */
    {
        const int N = 32;
        unistroke_point_t p[N];
        for (int i = 0; i < 16; i++) {
            float a = -(float)M_PI/2 + (float)(2*M_PI) * i / 15;
            p[i].x = (int16_t)(50 + 20 * cosf(a));
            p[i].y = (int16_t)(30 + 20 * sinf(a));
        }
        for (int i = 0; i < 16; i++) {
            float a = -(float)M_PI/2 + (float)(2*M_PI) * i / 15;
            p[16 + i].x = (int16_t)(50 + 20 * cosf(a));
            p[16 + i].y = (int16_t)(70 + 20 * sinf(a));
        }
        add_template('8', p, N);
    }

    /* 9 (saat yönünde loop) — üst daire CW, sonra sağ-aşağı dik çizgi.
     * Başlangıç tepede, CW daire (sağ→alt→sol→tepe), sonra sağ kenardan
     * aşağı düz. */
    {
        const int N = 28;
        unistroke_point_t p[N];
        for (int i = 0; i < 16; i++) {
            float a = -(float)M_PI/2 + (float)(2*M_PI) * i / 15;
            p[i].x = (int16_t)(50 + 22 * cosf(a));
            p[i].y = (int16_t)(30 + 22 * sinf(a));
        }
        for (int i = 0; i < 12; i++) {
            float t = (float)i / 11;
            p[16 + i].x = 72;
            p[16 + i].y = (int16_t)(30 + 60 * t);
        }
        add_template('9', p, N);
    }

    /* 9 (saat TERSİNE loop) — daha yaygın çizim stili. Sağ-üstten başla,
     * CCW daire (tepe→sol→alt→sağ-üste geri), sonra sağ-aşağı dik.
     * Eklendi çünkü kullanıcıların çoğu 9'u CCW çiziyor ve CW-only template
     * varken 'g'/'a' (ikisi de CCW loop + tail) yanlış eşleşiyordu. */
    {
        const int N = 28;
        unistroke_point_t p[N];
        /* CCW üst daire: başla (72,30) = angle 0 (sağ). Azalan açı ile
         * hareket: 0 → -π/2 (tepe, (50,8)) → -π (sol, (28,30)) → -3π/2 (alt,
         * (50,52)) → -2π (tam tur, (72,30)'a dön). */
        for (int i = 0; i < 16; i++) {
            float a = 0 - (float)(2*M_PI) * i / 15;
            p[i].x = (int16_t)(50 + 22 * cosf(a));
            p[i].y = (int16_t)(30 + 22 * sinf(a));
        }
        /* sağ kenardan düşey düz */
        for (int i = 0; i < 12; i++) {
            float t = (float)i / 11;
            p[16 + i].x = 72;
            p[16 + i].y = (int16_t)(30 + 60 * t);
        }
        add_template('9', p, N);
    }

    /* --- Harfler ---------------------------------------------------------
     * MULTI-STROKE harfler (H/T/t/i/x/f/k...) için template'i, stroke'lar
     * arasındaki "pen-up teleport"u da düz çizgi olarak içeren bir path
     * olarak veriyoruz. Kullanıcı, template'in tariflediği SIRADA çizmeli:
     * her template'in yanında beklenen çizim sırasını yorum olarak yazdım.
     */

    /* l — tek dikey çizgi ('1' ile çakışır; bağlam harf/rakam ayrımı).
     * İki aday yakınsa threshold filtresiyle 'l' döner. */
    line_stroke('l', 50, 10, 50, 90);

    /* L (büyük harf) — dikey aşağı + alt yatay sağa (TEK stroke).
     * Kullanıcının "L yaparken t görünüyor" bug'ının asıl sebebi: önceden
     * L şekli için hiçbir template yoktu, en yakın 't' çıkıyordu. */
    two_seg('L', 15, 10, 15, 90, 85, 90);

    /* G (büyük harf) — C benzeri büyük yay + orta sağda içe doğru yatay bar.
     * Tek stroke olarak tanımlı: sağ-üstten başla, saat yönünün TERSİNE
     * (üst → sol → alt → sağ-alt) yayı çiz, sağ kenar ortasında sola doğru
     * kısa iç bar ile bitir.
     *
     * BUG'ın sebebi: G için hiç template yoktu → $1 en yakın eğriye düşüyor
     * ve 's' (çift yay) veya 'c' (tek yay) matching ediyordu. */
    {
        float v[] = {
            82, 25,    /* sağ-üst (başlangıç) */
            55, 12,    /* tepe */
            25, 22,    /* sol-üst */
            12, 55,    /* sol-orta */
            25, 85,    /* sol-alt */
            55, 90,    /* alt */
            82, 75,    /* sağ-alt */
            82, 55,    /* sağ-orta */
            55, 55     /* iç merkeze */
        };
        path_template('G', v, 9);
    }

    /* H — sol-dikey, sağ-dikey, crossbar.
     * Çizim sırası: (1) sol üstten aşağı, (2) sağ üstten aşağı,
     *               (3) soldan sağa crossbar. */
    {
        float v[] = {
            15,10, 15,90,   /* stroke 1: left vertical */
            85,10, 85,90,   /* jump to stroke 2: right vertical */
            15,50, 85,50    /* jump to stroke 3: crossbar */
        };
        path_template('H', v, 6);
    }

    /* T (büyük) — üst yatay + dikey orta.
     * Sıra: (1) soldan sağa üst, (2) üstten aşağı dikey. */
    {
        float v[] = {
            15,15, 85,15,
            50,15, 50,90
        };
        path_template('T', v, 4);
    }

    /* t (küçük) — dikey önce + crossbar sonra.
     * Sıra: (1) üstten aşağı dikey, (2) soldan sağa crossbar. */
    {
        float v[] = {
            50,10, 50,90,
            25,35, 75,35
        };
        path_template('t', v, 4);
    }

    /* i — dikey gövde + üst nokta (kısa çizgi).
     * Sıra: (1) orta üstten aşağı gövde, (2) küçük nokta (tap hafifçe). */
    {
        float v[] = {
            50,30, 50,85,
            50,10, 50,18
        };
        path_template('i', v, 4);
    }

    /* x — iki çapraz.
     * Sıra: (1) sol-üstten sağ-alta, (2) sağ-üstten sol-alta. */
    {
        float v[] = {
            15,15, 85,85,
            85,15, 15,85
        };
        path_template('x', v, 4);
    }

    /* f — üst kıvrım + dikey + crossbar.
     * Sıra: (1) sağdan sola küçük yay ve aşağı dikey, (2) crossbar. */
    {
        float v[] = {
            75,20, 55,15, 45,25, 45,90,
            25,45, 60,45
        };
        path_template('f', v, 6);
    }

    /* k — dikey + üst-diagonal + alt-diagonal.
     * Sıra: (1) üstten aşağı dikey, (2) orta-sol'dan sağ-üste,
     *        (3) orta-sol'dan sağ-alta. */
    {
        float v[] = {
            20,10, 20,90,
            20,55, 80,15,
            20,55, 80,90
        };
        path_template('k', v, 6);
    }

    /* h — dikey + sağ hörgüç.
     * Sıra: (1) üstten aşağı dikey, (2) ortadan yukarı-sağa yay ve aşağı. */
    {
        float v[] = {
            20,10, 20,90,
            20,55, 40,40, 60,45, 65,60, 65,90
        };
        path_template('h', v, 7);
    }

    /* c — sağdan sola açılmış yay (sağ açık, saat yönü TERSİNE).
     * Sıra: sağ-üstten başla, tepeyi geç, sola, alta, sağ-alta inerek bit.
     * ESKİ template yanlış yönde (saat yönü, tepe açık) tanımlıydı → kullanıcının
     * doğru çizdiği 'c' hiç eşleşmiyordu. Açı aralığını ters yönde sweep edelim. */
    arc_stroke('c', 55, 50, 35,
               -(float)(M_PI/3), -(float)(5*M_PI/3));

    /* s — tepe yayı + alt ters yay */
    {
        const int N = 28;
        unistroke_point_t p[N];
        for (int i = 0; i < 14; i++) {
            float a = 0 + (float)M_PI * i / 13;
            p[i].x = (int16_t)(50 + 22 * cosf(a));
            p[i].y = (int16_t)(32 - 20 * sinf(a));
        }
        for (int i = 0; i < 14; i++) {
            float a = (float)M_PI + (float)M_PI * i / 13;
            p[14 + i].x = (int16_t)(50 + 22 * cosf(a));
            p[14 + i].y = (int16_t)(68 - 20 * sinf(a));
        }
        add_template('s', p, N);
    }

    /* u — sol-dikey aşağı, alt yayı AŞAĞI sallanan, sağ-dikey yukarı.
     * DÜZELTME: eski template'te sin işareti yanlıştı → yay YUKARI bump
     * yapıyordu (∩ = n şekli). Şimdi '-' işareti eklendi: a π→2π, sin goes
     * 0 → -1 → 0, y = 55 - 30*sin → 55 → 85 → 55 (DOWN at middle). */
    {
        const int N = 24;
        unistroke_point_t p[N];
        /* sol dikey aşağı */
        for (int i = 0; i < 8; i++) {
            p[i].x = 20;
            p[i].y = (int16_t)(15 + 40 * (float)i/7);
        }
        /* alt yay — DOWN dip */
        for (int i = 0; i < 8; i++) {
            float a = (float)M_PI + (float)M_PI * i / 7;
            p[8 + i].x = (int16_t)(50 + 30 * cosf(a));
            p[8 + i].y = (int16_t)(55 - 30 * sinf(a));
        }
        /* sağ dikey yukarı */
        for (int i = 0; i < 8; i++) {
            p[16 + i].x = 80;
            p[16 + i].y = (int16_t)(55 - 40 * (float)i/7);
        }
        add_template('u', p, N);
    }

    /* v — iki segment, köşe altta */
    two_seg('v', 15, 15, 50, 90, 85, 15);

    /* w — dört segment (dalgalı m/w) */
    {
        const int N = 28;
        unistroke_point_t p[N];
        float xs[5] = {10, 32, 50, 68, 90};
        float ys[5] = {15, 90, 40, 90, 15};
        for (int seg = 0; seg < 4; seg++) {
            for (int i = 0; i < 7; i++) {
                float t = (float)i / 6;
                p[seg*7 + i].x = (int16_t)(xs[seg] + (xs[seg+1]-xs[seg]) * t);
                p[seg*7 + i].y = (int16_t)(ys[seg] + (ys[seg+1]-ys[seg]) * t);
            }
        }
        add_template('w', p, N);
    }

    /* n — sol-dikey yukarı, üst yayı YUKARI bump, sağ-dikey aşağı.
     * DÜZELTME: eski template'te sin işareti yanlıştı → yay AŞAĞI dip
     * yapıyordu (⋃ = u şekli). Şimdi '+' işareti: sin π→2π, 0 → -1 → 0,
     * y = 50 + 30*sin → 50 → 20 → 50 (UP at middle). */
    {
        const int N = 24;
        unistroke_point_t p[N];
        for (int i = 0; i < 8; i++) {
            p[i].x = 20;
            p[i].y = (int16_t)(90 - 40 * (float)i/7);
        }
        for (int i = 0; i < 8; i++) {
            float a = (float)M_PI + (float)M_PI * i / 7;
            p[8 + i].x = (int16_t)(50 + 30 * cosf(a));
            p[8 + i].y = (int16_t)(50 + 30 * sinf(a));
        }
        for (int i = 0; i < 8; i++) {
            p[16 + i].x = 80;
            p[16 + i].y = (int16_t)(50 + 40 * (float)i/7);
        }
        add_template('n', p, N);
    }

    /* z — üst yatay, çapraz alt, alt yatay */
    three_seg('z', 15, 15, 85, 15, 15, 85, 85, 85);

    /* t — üst yatay + dikey (simple approximation) */
    two_seg('t', 15, 30, 85, 30, 50, 90);

    /* e — yatay ortadan sola, sonra C benzeri yay */
    {
        const int N = 28;
        unistroke_point_t p[N];
        /* orta yatay sola */
        for (int i = 0; i < 10; i++) {
            p[i].x = (int16_t)(80 - 50 * (float)i/9);
            p[i].y = 50;
        }
        /* C yayı */
        for (int i = 0; i < 18; i++) {
            float a = -(float)M_PI + (float)(1.5*M_PI) * i / 17;
            p[10 + i].x = (int16_t)(55 + 30 * cosf(a));
            p[10 + i].y = (int16_t)(55 + 30 * sinf(a));
        }
        add_template('e', p, N);
    }

    /* a (küçük) — tek stroke, Apple Scribble konvansiyonu:
     * sağ-üstten başla, saat yönünün TERSİNE loop yap (üst→sol→alt→sağ'a
     * geri), sonra sağ kenardan kısa kuyruk aşağı. Önceden 'a' template'i
     * yoktu → $1 en yakın şekle düşüyor ve 'w'/'e' gibi zikzag/yay şekillerle
     * yanlış eşleşiyordu. */
    {
        float v[] = {
            70, 30,   /* sağ-üst (başlangıç) */
            55, 22,
            35, 22,
            22, 38,
            22, 58,
            32, 76,
            52, 82,
            68, 74,
            70, 55,   /* loop kapanışı — sağ orta */
            72, 90    /* kuyruk */
        };
        path_template('a', v, 10);
    }

    /* a (alternatif) — bazı kullanıcılar oval'ı SAAT YÖNÜNDE çiziyor.
     * $1 ZAMANSAL yöne duyarlı olduğundan ayrı bir template gerekli.
     * Sıra: sol-üstten başla, CW: sağa-aşağı-sola, üste dön, kuyruk sağda. */
    {
        float v[] = {
            30, 22,   /* sol-üst başlangıç */
            50, 18,
            68, 25,
            75, 45,
            68, 68,
            50, 78,
            32, 72,
            25, 55,
            30, 40,   /* loop kapanışı (sol-üste yakın) */
            50, 55,
            70, 60,
            72, 90    /* kuyruk */
        };
        path_template('a', v, 12);
    }

    /* a (iki stroke) — kullanıcı oval'ı bir stroke, kuyruğu ayrı bir stroke
     * çizerse grace-period penceresi dışında iki ayrı stroke olarak gelir.
     * classify_multistroke'ta 'a' kuralı eklendi; yine de $1 fallback için
     * multi-stroke concat path'ine benzeyen bir template ekliyoruz:
     * oval (CCW) + sağ kenarda dik kuyruk. */
    {
        float v[] = {
            70, 50,
            55, 25,
            30, 28,
            22, 55,
            32, 78,
            58, 78,
            70, 60,   /* oval sonu */
            70, 25,   /* pen-up teleport (sağ-üste) */
            72, 92    /* kuyruk aşağı */
        };
        path_template('a', v, 9);
    }

    /* b (küçük) — stem aşağı + alttan saat yönünde küçük balon.
     * Sıra: (1) sol-üstten dik aşağı, (2) alttan yukarı-sağ-aşağı loop. */
    {
        float v[] = {
            25, 10,
            25, 90,   /* stem sonu */
            40, 82,
            60, 80,
            72, 68,
            68, 55,
            52, 50,
            32, 55,
            25, 62    /* kapanış */
        };
        path_template('b', v, 9);
    }

    /* d (küçük) — loop (saat tersine) + stem yukarı sağa + stem aşağı.
     * Apple Scribble 'd': önce oval CCW, sonra tam tepeden dik aşağı. */
    {
        float v[] = {
            70, 35,
            58, 22,
            38, 22,
            25, 40,
            28, 60,
            45, 78,
            62, 75,
            70, 60,   /* loop kapanışı */
            72, 90    /* stem aşağı */
        };
        path_template('d', v, 9);
    }

    /* g (küçük) — a gibi loop + uzun descender alt-sol kancaya kıvrılıyor. */
    {
        float v[] = {
            70, 30,
            55, 22,
            35, 22,
            22, 38,
            28, 58,
            48, 70,
            65, 65,
            70, 50,   /* loop kapanışı */
            68, 85,
            50, 95,
            28, 88    /* descender + sol hook */
        };
        path_template('g', v, 11);
    }

    /* j (küçük) — üst nokta + alt hook (descender + sol kıvrım).
     * path olarak: kısa dot, sonra stem, sonra hook. */
    {
        float v[] = {
            55, 12,
            55, 20,    /* dot */
            55, 38,
            55, 80,    /* stem aşağı */
            48, 92,
            32, 92,
            22, 82    /* sol hook */
        };
        path_template('j', v, 7);
    }

    /* m (küçük) — stem + 1. hörgüç + 2. hörgüç.
     * Sıra: sol-stem aşağı, yukarı, sağa-aşağı, yukarı, sağa-aşağı. */
    {
        float v[] = {
            15, 35,
            15, 90,   /* 1. stem */
            15, 45,
            25, 32,
            40, 32,
            50, 45,
            50, 90,   /* 2. stem */
            50, 45,
            60, 32,
            75, 32,
            85, 45,
            85, 90    /* 3. stem */
        };
        path_template('m', v, 12);
    }

    /* p (küçük) — uzun descender + üstte saat yönünde loop.
     * Sıra: üstten aşağı (uzun stem descender'a kadar), geri yukarı,
     * sağa loop aç-kapa. */
    {
        float v[] = {
            25, 35,
            25, 95,   /* descender */
            25, 35,   /* geri tepe */
            45, 25,
            65, 30,
            75, 45,
            65, 58,
            45, 60,
            25, 55    /* loop kapanışı */
        };
        path_template('p', v, 9);
    }

    /* q (küçük) — a gibi loop + sağ descender + kısa sağ hook. */
    {
        float v[] = {
            70, 30,
            55, 22,
            35, 22,
            22, 38,
            22, 58,
            35, 76,
            55, 78,
            70, 65,   /* loop kapanışı */
            72, 95,
            85, 92    /* sağ hook */
        };
        path_template('q', v, 10);
    }

    /* r (küçük) — kısa stem + üstte küçük sağa kıvrılan kanca.
     * Sıra: üstten aşağı stem, geri yukarı, sağa-aşağı kıvrım. */
    {
        float v[] = {
            25, 35,
            25, 90,   /* stem */
            25, 35,
            35, 28,
            55, 30,
            68, 42    /* küçük hook */
        };
        path_template('r', v, 6);
    }

    /* y (küçük) — sol diagonal + sağ diagonal descender.
     * v benzeri iki çapraz, ama sağ diagonal alta kadar devam ediyor ve
     * sola kıvrılıyor. Tek stroke: sol-üstten orta-alt'a, orta-alt'tan
     * sağ-üste, sağ-üstten aşağı descender'a. */
    {
        float v[] = {
            20, 30,
            50, 70,   /* köşe */
            50, 30,   /* dönüş: sağ-üst */
            50, 70,
            55, 90,
            40, 95,
            25, 90    /* descender + sol kanca */
        };
        path_template('y', v, 7);
    }

    /* ─── Büyük harfler (çizim sırası: tek stroke path, pen-up teleport'lar
     * düz çizgi olarak örneklenir). Kullanıcı template'in tariflediği SIRADA
     * çizmeli (Apple Scribble konvansiyonu). ────────────────────────────── */

    /* A — sol-alttan tepeye, tepeden sağ-alta, sonra crossbar.
     * Path: (15,90)→(50,10)→(85,90)→jump→(30,55)→(70,55) */
    {
        float v[] = {
            15, 90,
            50, 10,
            85, 90,
            30, 55,   /* teleport crossbar sol ucu */
            70, 55
        };
        path_template('A', v, 5);
    }

    /* B — sol dikey + üst balon + alt balon.
     * Sıra: üstten aşağı stem, teleport üste, üst balon saat yönünde, ortada
     * stem'e dön, alt balon saat yönünde, stem'de biter. */
    {
        float v[] = {
            20, 10, 20, 90,       /* stem */
            20, 10,               /* teleport üste */
            50, 12, 70, 28,
            50, 48, 20, 48,       /* üst balon */
            50, 50, 75, 68,
            50, 90, 20, 90        /* alt balon */
        };
        path_template('B', v, 13);
    }

    /* C — KALDIRILDI. Şekli 'c' küçük harf template'i ile birebir aynı,
     * $1 rotation/scale-invariant olduğundan boyut farkı kayboluyor. İki
     * template de varken hangisi kazanacağı rastgele oluyordu → çıktı
     * karışıyor. Çözüm: sadece 'c' tut, çıktı hep küçük. Büyük harf için
     * UI-düzeyinde ayrı caps lock gerek (mevcut input modelinde yok).
     * Aynı mantık: K, O, S, U, V, W, X, Z de kaldırıldı. */

    /* D — sol dikey + sağ büyük yarım-daire.
     * Sıra: üstten aşağı stem, teleport üste, sağa doğru yay yaparak alta. */
    {
        float v[] = {
            20, 10, 20, 90,       /* stem */
            20, 10,               /* teleport üste */
            55, 12, 75, 30,
            82, 50, 75, 70,
            55, 88, 20, 88        /* sağ yay + stem'e dön */
        };
        path_template('D', v, 10);
    }

    /* E — sol dikey + üst/orta/alt üç yatay çubuk.
     * Sıra: üstten aşağı stem, teleport üste, üst bar, teleport orta-sol'a,
     * orta bar, teleport alt-sol'a, alt bar. */
    {
        float v[] = {
            20, 10, 20, 90,       /* stem */
            20, 10, 75, 10,       /* üst bar (teleport otomatik düz çizgi) */
            20, 50, 65, 50,       /* orta bar */
            20, 90, 75, 90        /* alt bar */
        };
        path_template('E', v, 8);
    }

    /* F — sol dikey + üst + orta yatay (alt yok). */
    {
        float v[] = {
            20, 10, 20, 90,       /* stem */
            20, 10, 75, 10,       /* üst bar */
            20, 50, 65, 50        /* orta bar */
        };
        path_template('F', v, 6);
    }

    /* I — üst yatay + dikey + alt yatay (serif stil).
     * Büyük I'yı ayırt etmek için serif'le (küçük 'l' ve '1' ile çakışmasın). */
    {
        float v[] = {
            30, 10, 70, 10,       /* üst serif */
            50, 10, 50, 90,       /* dikey */
            30, 90, 70, 90        /* alt serif */
        };
        path_template('I', v, 6);
    }

    /* J — üst yatay + dikey aşağı + alt sol hook. */
    {
        float v[] = {
            35, 15, 75, 15,       /* üst bar */
            60, 15, 60, 75,       /* dikey */
            55, 88, 35, 88,
            22, 75                /* sol hook */
        };
        path_template('J', v, 7);
    }

    /* K — KALDIRILDI (k ile birebir aynı şekil; şekil-aynı büyük/küçük
     * çakışması için açıklama yukarıda C'de). */

    /* M — sol dikey + sağ diagonal aşağı + sağ diagonal yukarı + sağ dikey. */
    {
        float v[] = {
            15, 90, 15, 15,
            50, 70,
            85, 15, 85, 90
        };
        path_template('M', v, 5);
    }

    /* N — sol dikey + diagonal sağ-alt + sağ dikey yukarı. */
    {
        float v[] = {
            20, 90, 20, 15,
            80, 90,
            80, 15
        };
        path_template('N', v, 4);
    }

    /* O — KALDIRILDI (o ile birebir aynı şekil). */

    /* P — KALDIRILDI. Küçük 'p' ile scale_to_square normalize sonrası birebir
     * aynı şekle geliyor (her ikisinde de stem + üst balon, stem tam yükseklik).
     * u/U, c/C, o/O problemi ile aynı. Çözüm: sadece 'p' tut, her iki durumda
     * da küçük harf dön. 2-stroke geometrik P kuralı da 'p' döndürüyor artık. */

    /* Q — oval + sağ-alt diagonal kuyruk. */
    {
        float v[] = {
            50, 15, 25, 30,
            18, 55, 30, 80,
            55, 88, 78, 78,
            85, 55, 75, 30,
            50, 15,               /* oval kapanışı */
            60, 65, 90, 95        /* sağ-alt kuyruk */
        };
        path_template('Q', v, 11);
    }

    /* R — sol dikey + üst balon + sağ-alta diagonal. */
    {
        float v[] = {
            20, 10, 20, 90,
            20, 10,
            55, 12, 72, 28,
            55, 48, 20, 48,       /* üst balon */
            50, 48, 78, 90        /* sağ-alta diagonal */
        };
        path_template('R', v, 10);
    }

    /* S — KALDIRILDI (s ile aynı şekil). */
    /* U — KALDIRILDI (u ile aynı şekil). */
    /* V — KALDIRILDI (v ile aynı şekil). */
    /* W — KALDIRILDI (w ile aynı şekil). */
    /* X — KALDIRILDI (x ile aynı şekil). */

    /* Y — üst iki diagonal + alt dikey. Y'nin küçük 'y' versiyonu descender'lı
     * olduğundan şekli farklı → Y kalıyor. */
    {
        float v[] = {
            15, 15, 50, 50,
            85, 15, 50, 50,
            50, 90
        };
        path_template('Y', v, 5);
    }

    /* Z — KALDIRILDI (z ile aynı şekil). */

    s_tmpl_ready = true;
    ESP_LOGI(TAG, "Templates ready: %d", s_tmpl_count);
}

/* ==== Genel sınıflandırma ================================================ */

/* Filtrelenebilir $1 — exclude_digits=true olursa 0-9 template'leri atlanır.
 * Multi-stroke fallback'inde digit'lere düşmeyi engellemek için kullanılır
 * (çünkü kullanıcı rakamları neredeyse her zaman tek stroke'la çizer;
 * ns>=2 bir girişin rakam olma olasılığı çok düşük). */
static char classify_filtered(const unistroke_point_t* pts, size_t n,
                              bool exclude_digits)
{
    if (!s_tmpl_ready) init_templates();
    if (n < 4) return 0;
    if (path_length(pts, n) < 30.0f) return 0;

    float working[N_POINTS * 2];
    resample(pts, n, working);
    rotate_to_zero(working);
    scale_to_square(working, SQ_SIZE);
    translate_to_origin(working);

    /* Top-3 tutalım — tanı için ilk 3 aday + mesafelerini loglayabilelim. */
    float d1 = 1e9f, d2 = 1e9f, d3 = 1e9f;
    char  c1 = 0,    c2 = 0,    c3 = 0;

    for (int i = 0; i < s_tmpl_count; i++) {
        char tc = s_templates[i].ch;
        if (exclude_digits && tc >= '0' && tc <= '9') continue;
        float d = distance_at_best_angle(working, s_templates[i].pts);
        if (d < d1)      { d3 = d2; c3 = c2; d2 = d1; c2 = c1; d1 = d; c1 = tc; }
        else if (d < d2) { d3 = d2; c3 = c2; d2 = d; c2 = tc; }
        else if (d < d3) { d3 = d; c3 = tc; }
    }

    /* ESP_LOGI (not LOGD) — kullanıcı debug seviyesi açmadan da seri portta
     * görebilsin. Template mismatch debugging için kritik. */
    ESP_LOGI(TAG, "top3: '%c'=%.2f  '%c'=%.2f  '%c'=%.2f  (excl_digits=%d)",
             c1 ? c1 : '?', d1,
             c2 ? c2 : '?', d2,
             c3 ? c3 : '?', d3,
             exclude_digits);
    if (d1 > ACCEPT_THRESH) return 0;
    return c1;
}

char unistroke_classify(const unistroke_point_t* pts, size_t n)
{
    return classify_filtered(pts, n, false);
}

/* ==== Geometrik multi-stroke sınıflandırıcı ================================
 * $1'in eksiği: multi-stroke girişte tüm noktaları tek kesintisiz path gibi
 * işler → stroke'lar arasındaki "pen-up teleport"lar template ile
 * hizalanmazsa 6/8/9 gibi kıvrımlı rakamlara yanlış eşleşme yapar (ör. H→6).
 *
 * Bu fonksiyon stroke sayısı + her stroke'un geometrik karakteristiğini
 * (baskın yön, merkez, uzunluk) çıkarır, sonra bilinen multi-stroke harf
 * kalıplarıyla eşleştirir. Karar verirse karakter döndürür, veremezse 0
 * döner → çağıran $1'e düşer.
 * -------------------------------------------------------------------------
 */

typedef struct {
    float cx, cy;       /* merkez */
    float dx, dy;       /* ana eksen (normalize edilmiş yön vektörü) */
    float len;          /* stroke boyunca toplam arc length */
    float span;         /* bbox çapraz mesafesi (max uzunluk) */
    float bb_w, bb_h;   /* bbox genişlik/yükseklik */
    float x1, y1, x2, y2; /* bbox */
    float sx, sy;       /* stroke'un ilk noktası */
    float ex, ey;       /* stroke'un son noktası */
    bool  vertical;     /* |dy| > |dx| ve oran > 2× */
    bool  horizontal;   /* |dx| > |dy| ve oran > 2× */
    bool  diagonal;     /* ne tam dikey ne yatay */
} stroke_feat_t;

static void compute_feat(const unistroke_point_t* pts, uint16_t start,
                         uint16_t len, stroke_feat_t* f)
{
    if (len < 2) { memset(f, 0, sizeof(*f)); return; }
    float sx = 0, sy = 0;
    float x1 = 1e9, y1 = 1e9, x2 = -1e9, y2 = -1e9;
    float arc = 0;
    for (uint16_t i = 0; i < len; i++) {
        float x = pts[start + i].x;
        float y = pts[start + i].y;
        sx += x; sy += y;
        if (x < x1) x1 = x;
        if (x > x2) x2 = x;
        if (y < y1) y1 = y;
        if (y > y2) y2 = y;
        if (i > 0) {
            float ddx = x - pts[start + i - 1].x;
            float ddy = y - pts[start + i - 1].y;
            arc += sqrtf(ddx*ddx + ddy*ddy);
        }
    }
    f->cx = sx / len; f->cy = sy / len;
    f->x1 = x1; f->y1 = y1; f->x2 = x2; f->y2 = y2;
    f->bb_w = x2 - x1; f->bb_h = y2 - y1;
    f->sx = pts[start].x; f->sy = pts[start].y;
    f->ex = pts[start + len - 1].x; f->ey = pts[start + len - 1].y;
    float ax = f->ex - f->sx, ay = f->ey - f->sy;
    float alen = sqrtf(ax*ax + ay*ay);
    if (alen < 1.0f) { f->dx = 1; f->dy = 0; } else { f->dx = ax/alen; f->dy = ay/alen; }
    f->len = arc;
    f->span = sqrtf(f->bb_w*f->bb_w + f->bb_h*f->bb_h);
    /* Yön sınıfı: bbox oranlarına bak (yön vektörü kısa stroke'larda yanıltıcı).
     * Eşik 2.0× → 1.6× gevşetildi: kullanıcı H'nin dikey bacaklarını ~30°'ye
     * kadar eğik çizdiğinde bile "vertical" olarak sınıflansın ki
     * classify_multistroke 2V+1H H-kuralı yakalasın, aksi halde $1'e düşüyor
     * ve 3-stroke H, teleport'larla birleştirilince '6' gibi tek-stroke
     * kıvrımlı rakamlara yanlış eşleşiyor. */
    f->vertical   = (f->bb_h > f->bb_w * 1.6f) && (f->bb_h > 10.0f);
    f->horizontal = (f->bb_w > f->bb_h * 1.6f) && (f->bb_w > 10.0f);
    f->diagonal   = !f->vertical && !f->horizontal && (f->span > 15.0f);
}

/* Stroke'ları kanonik sıraya koy — karar kriterleri stroke sırasından
 * bağımsız olmalı. Burada stroke'ları x1'e göre (sol→sağ) sıralıyoruz,
 * ama bazı kurallar y'ye veya uzunluğa göre sıralama da ister → iki
 * yardımcı indeks dizisi döndürüyoruz. */
static void sort_by_x(const stroke_feat_t* f, uint8_t n, uint8_t* idx_x)
{
    for (uint8_t i = 0; i < n; i++) idx_x[i] = i;
    /* insertion sort — n küçük (1-10) */
    for (uint8_t i = 1; i < n; i++) {
        uint8_t v = idx_x[i]; uint8_t j = i;
        while (j > 0 && f[idx_x[j-1]].x1 > f[v].x1) {
            idx_x[j] = idx_x[j-1]; j--;
        }
        idx_x[j] = v;
    }
}

static void sort_by_len(const stroke_feat_t* f, uint8_t n, uint8_t* idx_l)
{
    for (uint8_t i = 0; i < n; i++) idx_l[i] = i;
    for (uint8_t i = 1; i < n; i++) {
        uint8_t v = idx_l[i]; uint8_t j = i;
        while (j > 0 && f[idx_l[j-1]].len < f[v].len) {
            idx_l[j] = idx_l[j-1]; j--;
        }
        idx_l[j] = v;
    }
}

/* İki stroke'un yakın bir köşede buluşup buluşmadığını kontrol et.
 * thresh px mesafe içinde uç-uç teması. İleride corner-based kurallar
 * (L köşe kontrolü vb.) için duruyor; şu an çağrısı yok, compiler'ın
 * uyarı verip -Werror ile fail etmemesi için __attribute__((unused)). */
__attribute__((unused))
static bool endpoints_meet(const stroke_feat_t* a, const stroke_feat_t* b,
                           float thresh)
{
    float t2 = thresh * thresh;
    float dx, dy;
    /* 4 kombinasyon: a.e-b.s, a.e-b.e, a.s-b.s, a.s-b.e */
    dx = a->ex - b->sx; dy = a->ey - b->sy; if (dx*dx + dy*dy <= t2) return true;
    dx = a->ex - b->ex; dy = a->ey - b->ey; if (dx*dx + dy*dy <= t2) return true;
    dx = a->sx - b->sx; dy = a->sy - b->sy; if (dx*dx + dy*dy <= t2) return true;
    dx = a->sx - b->ex; dy = a->sy - b->ey; if (dx*dx + dy*dy <= t2) return true;
    return false;
}

static char classify_multistroke(const unistroke_point_t* pts,
                                 const uint16_t* starts,
                                 const uint16_t* lens,
                                 uint8_t ns)
{
    if (ns < 2 || ns > 6) return 0;

    stroke_feat_t f[6];
    for (uint8_t i = 0; i < ns; i++) {
        compute_feat(pts, starts[i], lens[i], &f[i]);
    }

    uint8_t idx_x[6], idx_l[6];
    sort_by_x(f, ns, idx_x);
    sort_by_len(f, ns, idx_l);

    /* Saymak için: kaç dikey / yatay / çapraz stroke var */
    uint8_t nv = 0, nh = 0, nd = 0;
    for (uint8_t i = 0; i < ns; i++) {
        if (f[i].vertical) nv++;
        else if (f[i].horizontal) nh++;
        else if (f[i].diagonal) nd++;
    }

    /* Tüm stroke'ların kapsadığı global bbox — 'i' noktası gibi küçük
     * stroke'un büyüklere oranını anlamak için. */
    float gx1 = 1e9, gy1 = 1e9, gx2 = -1e9, gy2 = -1e9;
    for (uint8_t i = 0; i < ns; i++) {
        if (f[i].x1 < gx1) gx1 = f[i].x1;
        if (f[i].y1 < gy1) gy1 = f[i].y1;
        if (f[i].x2 > gx2) gx2 = f[i].x2;
        if (f[i].y2 > gy2) gy2 = f[i].y2;
    }
    float gw = gx2 - gx1, gh = gy2 - gy1;
    if (gw < 10 || gh < 10) return 0;

    /* ----- 2 strokes ------------------------------------------------------ */
    if (ns == 2) {
        const stroke_feat_t* a = &f[idx_l[0]]; /* uzun olan */
        const stroke_feat_t* b = &f[idx_l[1]]; /* kısa olan */

        /* a (küçük) — kapalı-loop stroke + sağ kenarda dikey kuyruk.
         * Çok sık: kullanıcı önce oval (geniş bbox, uç-uç yakın) sonra
         * sağda dik çizgi çiziyor. Hangi stroke oval, hangisi kuyruk:
         * oval'in bbox genişliği yüksekliğinden küçük ya da eşit ve
         * uçları birbirine yakın; kuyruk ise dikey.
         * Bu kuralı 'i'den ÖNCE koyuyoruz — 'i' kuralı geniş kapalı
         * loop'u asla yakalamaz (span check kısıtlıyor), ama yine de
         * öncelik için. */
        for (int tryA = 0; tryA < 2; tryA++) {
            const stroke_feat_t* loop  = (tryA == 0) ? &f[0] : &f[1];
            const stroke_feat_t* tail  = (tryA == 0) ? &f[1] : &f[0];
            /* loop: uç-uç yakın (~yakın-kapalı), minimum boyutlu, bbox
             * kabaca kare (0.6-1.6 oran). */
            float end_dx = loop->ex - loop->sx;
            float end_dy = loop->ey - loop->sy;
            float end_d  = sqrtf(end_dx*end_dx + end_dy*end_dy);
            bool loop_closed = (loop->span > 20.0f) && (end_d < loop->span * 0.45f);
            bool loop_square = (loop->bb_w > 15) && (loop->bb_h > 15) &&
                               (loop->bb_w < loop->bb_h * 1.8f) &&
                               (loop->bb_h < loop->bb_w * 1.8f);
            bool tail_vertical = tail->vertical && (tail->bb_h > gh * 0.45f);
            /* kuyruk sağda, loop solda */
            bool tail_right = tail->cx > loop->cx + loop->bb_w * 0.2f;
            /* kuyruk loop ile dikey olarak örtüşüyor (y aralığı kesişiyor) */
            bool y_overlap = (tail->y1 < loop->y2) && (tail->y2 > loop->y1);
            if (loop_closed && loop_square && tail_vertical &&
                tail_right && y_overlap) {
                /* '9' vs 'a' ayırma: '9'da kuyruk loop'un altından belirgin
                 * şekilde aşağıya uzanır; 'a'da kuyruk loop'un altıyla
                 * hemen hemen hizada biter. Kuyruk loop tabanının altına
                 * loop yüksekliğinin en az %40'ı kadar taşarsa '9' sayılır. */
                float below_extent = tail->y2 - loop->y2;
                if (below_extent > loop->bb_h * 0.40f) {
                    return '9';
                }
                return 'a';
            }
        }

        /* P (büyük) — sol dikey stem (uzun, tam yükseklik) + üst yarıda
         * sağa uzanan balon (kapalı/yarı-kapalı, üst yarıda ortalanmış).
         * Bu kural: '+' (alt yatay) ve 'T' (kısa dikey) kurallarından farklı
         * çünkü burada stem tam yükseklik, balon DIKEY DEĞİL. */
        for (int tryP = 0; tryP < 2; tryP++) {
            const stroke_feat_t* stem = (tryP == 0) ? &f[0] : &f[1];
            const stroke_feat_t* bulge= (tryP == 0) ? &f[1] : &f[0];
            /* stem: dikey, tam yükseklik (>0.7 global) */
            if (!stem->vertical) continue;
            if (stem->bb_h < gh * 0.7f) continue;
            /* stem global olarak SOLDA */
            if (stem->cx > gx1 + gw * 0.4f) continue;
            /* bulge: sağda, stem'in sağından başlıyor */
            if (bulge->x1 < stem->cx) continue;
            /* bulge üst yarıda ortalanmış (cy üst yarısında) */
            float bulge_rel = (bulge->cy - gy1) / (gh + 1.0f);
            if (bulge_rel > 0.5f) continue;
            /* bulge yükseklik ~ üst yarı (< stem yüksekliğinin %65'i) */
            if (bulge->bb_h > stem->bb_h * 0.65f) continue;
            /* bulge SAFI dikey OLMAMALI — balonun dikey bileşeni var ama
             * yatay gneişliği de yeterli. */
            if (bulge->bb_w < gw * 0.25f) continue;
            /* Her iki durumda da küçük 'p' dönüyoruz — P template'i
             * kaldırıldı (scale_to_square sonrası p ile birebir aynı). */
            return 'p';
        }

        /* i — bir uzun dikey gövde + küçücük bir nokta üstte */
        if (a->vertical && b->span < gh * 0.3f && b->cy < a->y1 + gh * 0.25f) {
            return 'i';
        }

        /* x — iki çapraz, bbox'ları büyük oranda örtüşüyor, merkezleri yakın */
        if (nd == 2) {
            float dcx = a->cx - b->cx, dcy = a->cy - b->cy;
            if (sqrtf(dcx*dcx + dcy*dcy) < gw * 0.3f &&
                /* yönler çapraz ters olmalı (dx işaretleri farklı VEYA dy işaretleri farklı) */
                ((a->dx * b->dx < 0) || (a->dy * b->dy < 0))) {
                return 'x';
            }
        }

        /* + — bir yatay + bir dikey, kesişim merkezde */
        if (nv == 1 && nh == 1) {
            const stroke_feat_t* v = f[0].vertical ? &f[0] : &f[1];
            const stroke_feat_t* h = f[0].horizontal ? &f[0] : &f[1];
            /* Yatay çizgi dikeyin üstünde mi (T), ortada (+), kısmen üstte (t), altta (L) */
            float rel_y = (h->cy - v->y1) / (v->y2 - v->y1 + 1e-3f);
            float hcx_rel = (h->cx - v->x1) / (v->bb_w + 1.0f);
            (void)hcx_rel;

            /* T — yatay üstte (rel_y < 0.2), dikey altta ortalanmış */
            if (rel_y < 0.25f) {
                /* dikey, yatayın orta noktasında mı? */
                if (fabsf(v->cx - h->cx) < h->bb_w * 0.35f) return 'T';
            }
            /* t — yatay hafif üstte (rel_y 0.2 ~ 0.45), cross pattern */
            if (rel_y >= 0.15f && rel_y < 0.5f) {
                if (fabsf(v->cx - h->cx) < h->bb_w * 0.4f) return 't';
            }
            /* + — merkezde kesişim (rel_y ~ 0.4-0.6, h cx ~ v cx) */
            if (rel_y >= 0.35f && rel_y <= 0.65f) {
                if (fabsf(v->cx - h->cx) < h->bb_w * 0.25f) return '+';
            }
            /* L — yatay alt kenarda, dikeyin sol ucundan sağa uzanıyor */
            if (rel_y > 0.75f) {
                if (h->cx > v->cx) return 'L';
            }
        }
    }

    /* ----- 3 strokes ------------------------------------------------------ */
    if (ns == 3) {
        /* H — 2 paralel dikey + 1 yatay (crossbar) ortada */
        if (nv == 2 && nh == 1) {
            /* dikeyleri bul, yatayı bul */
            const stroke_feat_t *v1 = NULL, *v2 = NULL, *h = NULL;
            for (uint8_t i = 0; i < 3; i++) {
                if (f[i].vertical)   { if (!v1) v1 = &f[i]; else v2 = &f[i]; }
                else if (f[i].horizontal) h = &f[i];
            }
            if (v1 && v2 && h) {
                /* dikeyler birbirine yakın değil, crossbar onları bağlar */
                float vsep = fabsf(v1->cx - v2->cx);
                if (vsep > gw * 0.35f) {
                    /* crossbar dikeylerin ortasında, y olarak ortalarında */
                    float avg_vy = (v1->cy + v2->cy) * 0.5f;
                    if (fabsf(h->cy - avg_vy) < gh * 0.25f &&
                        h->bb_w > vsep * 0.5f) {
                        return 'H';
                    }
                }
            }
        }

        /* k — 1 dikey (sol) + 2 çapraz (sağa açılan) */
        if (nv == 1 && nd == 2) {
            /* En solda dikey olmalı */
            const stroke_feat_t* v = NULL;
            const stroke_feat_t* d1 = NULL;
            const stroke_feat_t* d2 = NULL;
            for (uint8_t i = 0; i < 3; i++) {
                if (f[i].vertical)      v = &f[i];
                else if (f[i].diagonal) { if (!d1) d1 = &f[i]; else d2 = &f[i]; }
            }
            if (v && d1 && d2) {
                /* dikey en solda, çaprazlar sağında */
                if (v->cx < d1->cx && v->cx < d2->cx) {
                    /* çaprazların biri yukarı-sağa, diğeri aşağı-sağa açılıyor */
                    if ((d1->dy * d2->dy < 0) ||
                        (fabsf(d1->cy - d2->cy) > gh * 0.2f)) {
                        return 'k';
                    }
                }
            }
        }

        /* F — 1 dikey (sol) + 2 yatay (üstte ve ortada, sol kenardan sağa).
         * Dikey en solda; yatayların her ikisi de dikeyin sağına uzanıyor;
         * biri üstte (y ~ üst çeyrek), diğeri ortada (y ~ 0.4-0.6). */
        if (nv == 1 && nh == 2) {
            const stroke_feat_t *v = NULL, *h1 = NULL, *h2 = NULL;
            for (uint8_t i = 0; i < 3; i++) {
                if (f[i].vertical)   v = &f[i];
                else if (f[i].horizontal) { if (!h1) h1 = &f[i]; else h2 = &f[i]; }
            }
            if (v && h1 && h2) {
                /* top/mid ayır */
                const stroke_feat_t* top = h1->cy < h2->cy ? h1 : h2;
                const stroke_feat_t* mid = h1->cy < h2->cy ? h2 : h1;
                /* dikey yatayların soluna uzanır veya ayni hizada */
                if (v->x1 <= top->x1 + gw * 0.2f && v->x1 <= mid->x1 + gw * 0.2f) {
                    float top_rel = (top->cy - v->y1) / (v->bb_h + 1.0f);
                    float mid_rel = (mid->cy - v->y1) / (v->bb_h + 1.0f);
                    if (top_rel < 0.25f && mid_rel > 0.3f && mid_rel < 0.65f) {
                        return 'F';
                    }
                }
            }
        }
    }

    /* ----- 4 strokes ------------------------------------------------------ */
    /* (eski boş blok burada kapatılıyor — F bloğu yukarıda) */
    if (ns == 4) {
        /* E — 1 dikey (sol) + 3 yatay (üst/orta/alt, hepsi dikeyin sağında). */
        if (nv == 1 && nh == 3) {
            const stroke_feat_t *v = NULL;
            const stroke_feat_t *hs[3] = { NULL, NULL, NULL };
            uint8_t hi = 0;
            for (uint8_t i = 0; i < 4; i++) {
                if (f[i].vertical)   v = &f[i];
                else if (f[i].horizontal && hi < 3) hs[hi++] = &f[i];
            }
            if (v && hi == 3) {
                /* hs[0..2]'yi cy'ye göre sırala — top/mid/bot. İnsertion. */
                for (int i = 1; i < 3; i++) {
                    const stroke_feat_t* t = hs[i];
                    int j = i;
                    while (j > 0 && hs[j-1]->cy > t->cy) { hs[j] = hs[j-1]; j--; }
                    hs[j] = t;
                }
                const stroke_feat_t* top = hs[0];
                const stroke_feat_t* mid = hs[1];
                const stroke_feat_t* bot = hs[2];
                /* Hepsi dikey ile aynı x başlangıcına yakın, uzunlukları benzer */
                bool aligned =
                    fabsf(v->x1 - top->x1) < gw * 0.25f &&
                    fabsf(v->x1 - mid->x1) < gw * 0.25f &&
                    fabsf(v->x1 - bot->x1) < gw * 0.25f;
                float top_rel = (top->cy - v->y1) / (v->bb_h + 1.0f);
                float mid_rel = (mid->cy - v->y1) / (v->bb_h + 1.0f);
                float bot_rel = (bot->cy - v->y1) / (v->bb_h + 1.0f);
                bool spread = top_rel < 0.3f && mid_rel > 0.3f && mid_rel < 0.7f &&
                              bot_rel > 0.7f;
                if (aligned && spread) {
                    return 'E';
                }
            }
        }
    }

    return 0;
}

char unistroke_classify_multi(const unistroke_point_t* pts, size_t n,
                              const uint16_t* stroke_starts,
                              const uint16_t* stroke_lens,
                              uint8_t n_strokes)
{
    if (!s_tmpl_ready) init_templates();
    if (n < 4 || n_strokes == 0) return 0;

    /* Tek stroke → doğrudan $1 */
    if (n_strokes == 1) {
        return unistroke_classify(pts, n);
    }

    /* Önce geometrik multi-stroke kuralları */
    char c = classify_multistroke(pts, stroke_starts, stroke_lens, n_strokes);
    if (c) {
        ESP_LOGD(TAG, "geom-multi='%c' (ns=%d)", c, n_strokes);
        return c;
    }

    /* Geometrik karar veremedi → $1'e düş.
     * KRİTİK: ns>=2 olduğunda digit template'leri FİLTRELE. Sebep: rakamlar
     * (0-9) neredeyse her zaman tek stroke'la çizilir; kullanıcı pen-up
     * yapıp multi-stroke çizdiyse bu büyük ihtimalle bir HARFtir. Örn. H
     * harfi (3 stroke), concatenated path olarak $1'e verildiğinde
     * teleport'lar 6/8/9 gibi kıvrımlı rakamlara yakın bir şekil üretiyor
     * ve yanlışlıkla digit matching oluyordu. Digit'leri dışlamak bu
     * yanlış eşleşmeyi tamamen engelliyor. */
    return classify_filtered(pts, n, n_strokes >= 2);
}
