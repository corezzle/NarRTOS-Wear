#!/bin/zsh
# NAR-ESP32S3 tek komutla kayıt (2026-11-02, saha testi için).
#   tools/nar_kayit.sh normal [sn]   → standart ayar (HPF açık), kanal 3, 48 kHz, 24 bit, MP3 320 kbit/s
#   tools/nar_kayit.sh kalp   [sn]   → HPF açık (2026-11-06 kullanıcı karşılaştırması), kanal 3, 48 kHz, 24 bit, 320 kbit/s; ham + _temiz MP3 + ritim
# Kayıt KAYIT_DIZINI'ne (varsayılan ~/Desktop/NAR-kayitlar) YYYY-MM-DD_HH-MM-SS_<mod>.mp3 olarak yazılır
# (ffmpeg yoksa .wav). Kart takılı değilse 60 sn bekler. Eski metin akışı: tools/mic_ham.py + tools/mic_wav.py.
MOD=${1:-normal}; SN=${2:-30}
DIZIN=${KAYIT_DIZINI:-$HOME/Desktop/NAR-kayitlar}; mkdir -p "$DIZIN"
cd "$(dirname $0)/.." || exit 1
case $MOD in normal) VAR=0 ;; kalp) VAR=0 ;; *) echo "kullanım: nar_kayit.sh normal|kalp [saniye]"; exit 1 ;; esac
# 2026-11-06: 48 kHz, ikili akış, 24 bit, MP3 320 kbit/s. BIT=16 ile 16 bit.
TMP=$(mktemp -d)/kayit.wav
for DENEME in 1 2 3; do
  python3 -u tools/mic_raw.py "$TMP" "$SN" 3 "${BIT:-24}" "$VAR" && [ -f "$TMP" ] && break
  echo "kayıt yarım kaldı, yeniden deneniyor ($DENEME/3)"
done
[ -f "$TMP" ] || { echo "kayıt alınamadı"; exit 2; }
AD="$DIZIN/$(date +%F)_$(date +%H-%M-%S)_${MOD}"
ffmpeg -hide_banner -loglevel error -y -i "$TMP" -ac 1 -c:a libmp3lame -b:a 320k "$AD.mp3" || { cp "$TMP" "$AD.wav"; echo "dosya: $AD.wav"; exit 0; }
echo "dosya: $AD.mp3"; echo "wav: $TMP"
if [ "$MOD" = kalp ]; then
  /opt/homebrew/bin/python3 tools/mic_temiz.py "$TMP" "${TMP%.wav}_temiz.wav" \
    && ffmpeg -hide_banner -loglevel error -y -i "${TMP%.wav}_temiz.wav" -ac 1 -c:a libmp3lame -b:a 320k "${AD}_temiz.mp3" \
    && echo "temiz: ${AD}_temiz.mp3"
fi
exit 0
