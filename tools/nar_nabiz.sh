#!/bin/zsh
# NAR-ESP32S3: kartta nabız ölçümü + aynı sesin bilgisayara kaydı (2026-11-09).
#   tools/nar_nabiz.sh [sn=15] [kanal=3]
# Kart "mic nabiz <sn> <kanal> 24 0 usb" ile hem analiz eder hem ham akışı USB'ye basar; mic_raw.py akışı WAV yapar,
# kartın [mic] nabiz/pencereler satırlarını "cihaz:" diye gösterir. Sonra ffmpeg ile MP3 (~/Desktop/NAR-kayitlar/*_nabiz.mp3).
# Karşılaştırma: numpy varsa Mac'te tools/nabiz_ref.py sonucu da yazılır. Hayvan ölçümü ve etiketli kayıt: tools/nar_olcum.py
SN=${1:-15}; KANAL=${2:-3}
DIZIN=${KAYIT_DIZINI:-$HOME/Desktop/NAR-kayitlar}; mkdir -p "$DIZIN"
cd "$(dirname $0)/.." || exit 1
PY=${MIC_PYTHON:-python3}
TMP=$(mktemp -d)/nabiz.wav
echo "steteskop gogse: $SN sn dinleniyor (kart analiz eder, ses de kaydedilir)..."
MIC_KOMUT="mic nabiz" MIC_EK="usb" MIC_BEKLE=${MIC_BEKLE:-45} $PY -u tools/mic_raw.py "$TMP" "$SN" "$KANAL" 24 0 || { echo "kayit alinamadi"; exit 2; }
[ -f "$TMP" ] || { echo "WAV uretilemedi"; exit 2; }
AD="$DIZIN/$(date +%F)_$(date +%H-%M-%S)_nabiz"
if command -v ffmpeg >/dev/null && ffmpeg -hide_banner -loglevel error -y -i "$TMP" -ac 1 -c:a libmp3lame -b:a 320k "$AD.mp3"; then
  echo "dosya: $AD.mp3"
else
  cp "$TMP" "$AD.wav"; echo "dosya: $AD.wav"
fi
echo "wav: $TMP"
# Karşılaştırma: kart algoritmasının bilgisayardaki referansı (tools/nabiz_ref.py, alg A1, insan profili).
# Not: bu WAV mic_raw.py'nin işlenmiş çıktısıdır (ilk 1 sn atılmış); birebir karşılaştırma ve etiketli kayıt için
# tools/nar_olcum.py kullanın (ham kaydı saklar, kartla aynı örnekler üzerinde hesaplar).
if REF=$($PY tools/nabiz_ref.py "$TMP" insan 2>/dev/null); then
  echo "$REF" | sed 's/^.*: alg=/Mac referansi: alg=/'
else
  echo "(Mac karsilastirmasi icin numpy gerekli: python3 -m pip install --user --break-system-packages numpy)"
fi
