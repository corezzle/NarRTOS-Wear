#pragma once
/**
 * @file nabiz_alg.h
 * @brief Mikrofon kaydından nabız + solunum + mırlama/soluma kestirimi — alg A3 (2026-11-11; A2 + "pencere" ve
 *        "tutarli" düşük güvenli kabul yolları). Saf C, donanımdan bağımsız.
 *
 * Aynı kaynak hem kartta (mic_service.cpp) hem bilgisayarda derlenir (tools/nabiz_host.c); tools/nabiz_test.py
 * bilgisayardaki derlemeyi bağımsız Python referansıyla (tools/nabiz_ref.py) karşılaştırır. Üçü BİRLİKTE değişir,
 * NABIZ_ALG sürümü artar. Algoritmanın anlatımı: nabiz_ref.py başındaki açıklama.
 *
 * Kullanım:
 *   nabiz_akis_t a;  nabiz_akis_baslat(&a, saniye, bayt, nabiz_profil(i), &ortam);
 *   ... her veri parçasında nabiz_akis_besle(&a, veri, uzunluk);      (48 kHz, mono, 16 ya da 24 bit küçük uçlu)
 *   nabiz_sonuc_t s;  nabiz_analiz(&a, &s);  nabiz_satir(&s, tampon, n);
 *   nabiz_akis_bitir(&a);
 * Bütün büyük diziler ortam.al ile alınır (kartta PSRAM); yığın kullanımı küçüktür.
 * Eşikler BAŞLANGIÇ değerleridir: insan kayıtları ve yapay sinyallerle ayarlandı, hayvan kaydıyla doğrulanmadı.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NABIZ_ALG          "A3"
#define NABIZ_FS           48000
#define NABIZ_PENCERE_MAX  80          /* 8 sn pencere, 4 sn adım: 300 sn kayıtta 73 */

typedef struct {
    const char* ad;
    float hp, lp;            /* kalp bandı (Hz) */
    int   lag_min, lag_max;  /* 10 ms adım; nabız = 6000 / gecikme */
    int   sn;                /* varsayılan kayıt süresi */
    bool  mirlama, soluma;   /* bu türde aranır mı */
} nabiz_profil_t;

int                   nabiz_profil_sayisi(void);
const nabiz_profil_t* nabiz_profil(int i);            /* geçersiz dizin → 0 (insan) */
int                   nabiz_profil_bul(const char* ad); /* yoksa -1 */

/** Bellek ve zaman paylaşımı kancaları. nefes: uzun hesaplarda ara ara çağrılır (kartta vTaskDelay; NULL olabilir). */
typedef struct {
    void* (*al)(size_t);
    void  (*birak)(void*);
    void  (*nefes)(void);
} nabiz_ortam_t;

typedef struct { float b0, b1, b2, a1, a2, z1, z2; } nabiz_bq_t;

typedef struct {
    const nabiz_profil_t* p;
    nabiz_ortam_t o;
    int      bytes;
    int32_t  esik;                    /* doygunluk eşiği: (int)(0,999 × (tam ölçek − 1)) */
    float*   x1k;  uint32_t n1k, cap1k;   /* 1 kHz akış (48'lik ortalama, tam ölçeğe bölünmüş) */
    float*   envb; uint32_t nb, capb;     /* solunum zarfı, 100 Hz */
    double   acc48; int n48;
    double   acc12; int n12;
    nabiz_bq_t hpb1, hpb2, lpb;       /* solunum yolu: 4 kHz'de HP 200 Hz (4. derece) + LP 1500 Hz */
    float    bacc; int bn;
    uint32_t n4k; double sb_kare; uint32_t sb_n;       /* solunum bandı RMS (ilk 1 sn hariç) */
    uint32_t n_ornek, n_kalite, n_doygun; int32_t tepe; /* kalite (ilk 1 sn hariç) */
} nabiz_akis_t;

typedef struct {
    const nabiz_profil_t* p;
    char  durum[16];        /* OK | BELIRSIZ | KAT_SUPHESI | MIRLAMA | DOYGUN | SINYAL_YOK | KISA */
    float nabiz, r, r2;
    char  yontem[8];        /* "ac" | "vurus" | "pencere" | "tutarli" (son ikisi A3: DÜŞÜK GÜVEN) */
    float nabiz_ac, nabiz_vurus;
    int   nvurus; float yayilim, belirgin; int iki_tepe, vguven;
    float solunum, rs; char sdurum[16];   /* OK | BELIRSIZ | KISA | MIRLAMA | SOLUMA | KALP_SIZINTISI */
    int   mirlama; float mirlama_hz, mirlama_oran;
    int   soluma;  float soluma_hiz, soluma_r;
    uint32_t doygun, n_kalite;
    float tepe_db, seviye_db, sseviye_db, sure;
    int   pencere_n; float pencere_bpm[NABIZ_PENCERE_MAX], pencere_r[NABIZ_PENCERE_MAX];
    int   pen_iyi, pen_uyan; float r2t;   /* A3: net pencere sayısı, adaya yakın pencere sayısı, iki periyot sonraki r */
} nabiz_sonuc_t;

/** saniye: en uzun kayıt (tamponlar buna göre). bytes: 2 ya da 3. Bellek yoksa false. */
bool nabiz_akis_baslat(nabiz_akis_t* a, int saniye, int bytes, const nabiz_profil_t* p, const nabiz_ortam_t* o);
void nabiz_akis_besle(nabiz_akis_t* a, const uint8_t* d, uint32_t len);
void nabiz_akis_bitir(nabiz_akis_t* a);

/** Toplanan akışı çözümler. Bellek yetmezse false (out->durum "KISA" kalır). */
bool nabiz_analiz(nabiz_akis_t* a, nabiz_sonuc_t* out);

/** "alg=A2 profil=... durum=... nabiz=..." anahtar=değer satırı (nabiz_ref.satir ile aynı biçim). Dönüş: uzunluk. */
int nabiz_satir(const nabiz_sonuc_t* s, char* buf, size_t n);

#ifdef __cplusplus
}
#endif
