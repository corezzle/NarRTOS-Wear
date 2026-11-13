<div style="border-left:4px solid #12A0AD;background:rgba(18,160,173,.10);border-radius:0 10px 10px 0;padding:14px 18px;margin:18px 0"><div style="font-weight:800;margin-bottom:6px;color:inherit">Kod derlemeden, hazır sürümü yüklemek isteyenler</div><div style="line-height:1.6;color:inherit"><strong>Corezzle Imager</strong> masaüstü uygulaması NarRTOS'un hazır sürümünü karta yazar: kartı tak, kartını seç, düğmeye bas. Adım adım anlatım: <a href="/document-content/narrtos-wear-corezzle-imager-ile-yukleme-12">Corezzle Imager ile Yükleme</a>. Aşağıdaki adımlar yalnız kodu kendisi derleyip değiştirmek isteyenler için.</div></div>

<div style="border-left:4px solid #12A0AD;background:rgba(18,160,173,.10);border-radius:0 10px 10px 0;padding:14px 18px;margin:18px 0"><div style="font-weight:800;margin-bottom:6px;color:inherit">Kuruluma başlamadan önce</div><div style="line-height:1.6;color:inherit">Bilgisayarınızda VS Code yüklü değilse önce onu kurun: <a href="/document-content/vs-code-kurulumu-10">VS Code Kurulumu</a>. VS Code kurulu ama projeyi içinde nasıl indirip açacağınızı bilmiyorsanız <a href="/document-content/narrtos-wear-vs-code-kurulumu-11">NarRTOS için VS Code Kurulumu</a> sayfası, aşağıdaki ilk komutu nereye yapıştıracağınızı adım adım gösterir. Sonra buraya dönün.</div></div>

Bilgisayarınızda PlatformIO kuruluysa ve kart bağlıysa dört komut yeter:

```
git clone https://github.com/corezzle/NarRTOS-Wear.git
cd NarRTOS-Wear
pio run -e esp32s3_wearable -t upload
pio device monitor
```

Sırasıyla: projeyi indirir, klasörüne girer, derleyip karta yükler, cihazın çıktısını dinler.

![Dört komutun terminaldeki akışı](GORSEL: terminal-quick-start.gif)

*Dört komut uçtan uca: indirme, klasöre giriş, derleyip yükleme, açılış kaydı. Çıktılar kısaltıldı.*

<div style="border-left:4px solid #12A0AD;background:rgba(18,160,173,.10);border-radius:0 10px 10px 0;padding:14px 18px;margin:18px 0"><div style="font-weight:800;margin-bottom:6px;color:inherit">İlk kez mi kuruyorsunuz?</div><div style="line-height:1.6;color:inherit">Bu dört komut, gerekli programların zaten kurulu olduğunu varsayar. Hiç yapmadıysanız <strong>Başlarken</strong> bölümünden gidin. Python ve PlatformIO kurulumundan kabloyu takmaya kadar her adım tek tek anlatılıyor.</div></div>
