/* nabiz_host.c: kart algoritmasını (src/services/nabiz_alg.c) BİLGİSAYARDA çalıştırır. tools/nabiz_test.py derler ve kullanır.
 * Derleme: cc -O2 -std=c99 -Wall -Wextra -Werror -I src/services tools/nabiz_host.c src/services/nabiz_alg.c -lm -o nabiz_host
 * Kullanım: nabiz_host <kayit.wav> [insan|kopek|kedi]     (48 kHz mono, 16 ya da 24 bit; veri karttaki gibi 960 baytlık parçalarla beslenir) */
#include "nabiz_alg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t u32(const unsigned char* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "kullanim: nabiz_host <kayit.wav> [profil]\n"); return 2; }
    int pi = argc > 2 ? nabiz_profil_bul(argv[2]) : 0;
    if (pi < 0) { fprintf(stderr, "bilinmeyen profil: %s\n", argv[2]); return 2; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END); long boy = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char* d = (unsigned char*)malloc((size_t)boy);
    if (!d || fread(d, 1, (size_t)boy, f) != (size_t)boy) { fprintf(stderr, "okunamadi\n"); return 2; }
    fclose(f);
    if (boy < 44 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) { fprintf(stderr, "WAV degil\n"); return 2; }
    int kanal = 0, bit = 0; uint32_t fs = 0; const unsigned char* veri = NULL; uint32_t vn = 0;
    for (long i = 12; i + 8 <= boy;) {
        uint32_t n = u32(d + i + 4);
        if (!memcmp(d + i, "fmt ", 4)) { kanal = d[i + 10] | d[i + 11] << 8; fs = u32(d + i + 12); bit = d[i + 22] | d[i + 23] << 8; }
        else if (!memcmp(d + i, "data", 4)) { veri = d + i + 8; vn = n; if ((long)(i + 8 + n) > boy) vn = (uint32_t)(boy - i - 8); break; }
        i += 8 + (long)n + (n & 1);
    }
    if (!veri || kanal != 1 || fs != NABIZ_FS || (bit != 16 && bit != 24)) { fprintf(stderr, "48 kHz mono 16/24 bit bekleniyor\n"); return 2; }
    const int bytes = bit / 8;
    nabiz_ortam_t o = { malloc, free, NULL };
    nabiz_akis_t a;
    int saniye = (int)(vn / (uint32_t)(bytes * NABIZ_FS)) + 1;
    if (!nabiz_akis_baslat(&a, saniye, bytes, nabiz_profil(pi), &o)) { fprintf(stderr, "bellek\n"); return 2; }
    for (uint32_t i = 0; i < vn; i += 960) nabiz_akis_besle(&a, veri + i, vn - i < 960 ? vn - i : 960);
    nabiz_sonuc_t* s = (nabiz_sonuc_t*)malloc(sizeof *s);
    if (!s || !nabiz_analiz(&a, s)) { fprintf(stderr, "analiz basarisiz\n"); return 2; }
    char l[400]; nabiz_satir(s, l, sizeof l); printf("%s\n", l);
    printf("pencereler:");
    for (int i = 0; i < s->pencere_n; i++) printf(" %.0f(%.2f)", (double)s->pencere_bpm[i], (double)s->pencere_r[i]);
    printf("\nek: mirlama_hz=%.1f mirlama_oran=%.2f soluma_hiz=%.0f soluma_r=%.2f iki_tepe=%d\n",
           (double)s->mirlama_hz, (double)s->mirlama_oran, (double)s->soluma_hiz, (double)s->soluma_r, s->iki_tepe);
    nabiz_akis_bitir(&a); free(s); free(d);
    return 0;
}
