#!/bin/zsh
# NAR-ESP32S3 hızlı yükleme (2026-11-02). BOOT'a basmak gerekmez (USB-Serial-JTAG indirme moduna kendisi girer).
# Bu SiP'in flash'ına esptool stub yazamıyor → --no-stub; tek büyük silme USB'yi düşürüyor → 256 KB parçalar.
# 2026-11-04: yarım yükleme bırakmaz. Bağlantı koparsa baştan değil kaldığı parçadan sürer, hepsi doğrulanana kadar
# bitmez (Ctrl+C ile durur). Kart değiştirilirse (MAC farklı) yazmaz, çıkar. Önyükleyici en son yazılır.
# Önce: pio run -e nar_esp32s3
P=~/.platformio/penv/bin/python; D=~/.platformio/packages/tool-esptoolpy
B="$(cd "$(dirname $0)/.." && pwd)/.pio/build/nar_esp32s3"
[ -f "$B/firmware.bin" ] || { echo "önce derleyin: pio run -e nar_esp32s3"; exit 1; }
T=$(mktemp -d); trap 'rm -rf $T' EXIT; (cd $T && split -b 262144 -a 2 -d "$B/firmware.bin" g)
ADRES=(); DOSYA=(); i=0
for f in $T/g??(N); do ADRES+=($(printf "0x%x" $((0x10000 + i*262144)))); DOSYA+=($f); i=$((i+1)); done
ADRES+=(0x8000 0xe000 0x0); DOSYA+=($B/partitions.bin $B/ota_data_initial.bin $B/bootloader.bin)
N=${#ADRES}; echo "$N parça, $(date +%T)"

port_bekle() {
  local t0=$(date +%s) son=0 g
  while true; do
    g=(/dev/cu.usbmodem*(N)); PORT=$g[1]
    [ -n "$PORT" ] && return
    if [ $(( $(date +%s) - son )) -ge 30 ] && [ $(( $(date +%s) - t0 )) -ge 5 ]; then
      echo "kart bekleniyor… ($((k-1))/$N yazıldı)"; son=$(date +%s)
    fi
    sleep 0.1
  done
}
esp() { "$P" $D/esptool.py --chip esp32s3 --port $PORT "$@" 2>&1 }

# Elle sürdürme: NAR_BASLA=11 NAR_MAC=<mac> yalnız önceki parçalar aynı kartta doğrulandıysa kullanılır.
MAC=${NAR_MAC:-}; k=${NAR_BASLA:-1}; hata=0
export PYTHONUNBUFFERED=1
while [ $k -le $N ]; do
  port_bekle
  # BOOT ile takıldıysa çip zaten yükleme modundadır: önce sıfırlamadan bağlan, olmazsa sıfırla.
  M=$(esp --before no_reset --after no_reset --no-stub read_mac | grep -m1 -oE "MAC: [0-9a-f:]+" | cut -d' ' -f2)
  [ -z "$M" ] && M=$(esp --before default_reset --after no_reset --no-stub read_mac | grep -m1 -oE "MAC: [0-9a-f:]+" | cut -d' ' -f2)
  [ -z "$M" ] && { sleep 1; continue; }
  if [ -n "$MAC" ] && [ "$M" != "$MAC" ]; then
    echo "HATA: kart değişti ($MAC → $M). Yazma durduruldu; $MAC kartında $((k-1))/$N parça var."; exit 2
  fi
  [ -z "$MAC" ] && echo "kart $M"
  MAC=$M
  # 2026-11-05: parça başına sıfırlama kartı USB'den düşürüyordu (her takışta tek parça yazıldı). Kalan parçalar
  # read_mac'in bıraktığı yükleme modunda, sıfırlamadan tek bağlantıyla yazılır.
  KALAN=(); for ((j=k; j<=N; j++)); do KALAN+=($ADRES[$j] $DOSYA[$j]); done
  HATA_S=""
  esp --before no_reset --after hard_reset --no-stub write_flash -z --flash_mode dio --flash_freq 40m --flash_size 32MB $KALAN |
    while IFS= read -r S; do
      case "$S" in
        *"Hash of data verified"*) echo "$k/$N tamam ($ADRES[$k])"; k=$((k+1)); hata=0 ;;
        *rror*|*Lost*) [ -z "$HATA_S" ] && HATA_S=${S[1,90]} ;;
      esac
    done
  if [ $k -le $N ]; then
    hata=$((hata+1))
    echo "$k/$N koptu, kaldığı yerden sürecek ($hata): $HATA_S"
    sleep 1
  fi
done
echo "dogrulanan: $N / $N"; echo "YÜKLEME TAMAM $(date +%T) (kart $MAC)"
