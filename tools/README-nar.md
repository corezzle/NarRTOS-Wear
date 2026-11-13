# nar — tek komutla test projeleri

`~/bin/nar` → `embedded-wear-os/tools/nar`. Projelere dokunmaz; elle yazdığın pio/esptool/pyocd
komutlarını senin yerine çalıştırır ve her komutu ekrana yazar (log: `~/.nar/last.log`).

```
nar                      menü (kart/port durumu + proje listesi)
nar test lte             narrtos-lte-test: derle + yükle + monitör   (nar lte  de aynı)
nar test lte lte_bridge  farklı env
nar test board --monitor yalnız monitör      --build yalnız derle   --upload yalnız yükle
nar test                 proje klasörünün içindeysen projeyi kendisi bulur
nar os                   embedded-wear-os nar_esp32s3
nar probe                ESP'yi SWD probuna çevir (GPIO5->TP1, GPIO4->TP2)
nar slm [--stay]         nRF9151'e SLM (tek oturum), sonra lte'ye dön + AT doğrula
nar modemfw              modem yazılımı PTI -> 2.0.4 (fmfu akışı)
nar at                   AT konsolu (lte + monitör, 'c' tuşu)
nar ports / nar list     takılı kart ve modu / projeler
--dry-run                yalnız komutları göster
```
ESP prob/köprü modundaysa yüklemeden önce 1200-baud dokunuşunu kendisi yapar; yükleme tutmazsa
"BOOT'a basılı tutarak tak" der ve portu bekler. Monitör açıkken (port meşgul) uyarıp durur.
