# SnapSlide

**Parmak şıklatarak sunum kontrolü.** Giyilebilir bir ESP32-S3 cihazı; parmak şıklatmanızı gerçek zamanlı FFT ile ayırt edip, bilgisayara **standart BLE HID klavye** olarak ok tuşu gönderir.

> Tek şıklatma → **sonraki slayt** · Çift şıklatma → **önceki slayt**

Sürücü yok, eşlik eden uygulama yok, kablo yok. Windows / macOS / Linux / Android / iOS — hepsi cihazı Bluetooth klavye olarak görür, PowerPoint'ten Google Slides'a her yerde çalışır.

---

## Neden ilginç?

Sunum yaparken elinizde klikır tutmak zorunda kalmazsınız. Ama asıl teknik problem şu: **parmak şıklatması ile alkışı, öksürüğü, kapı çarpmasını ve motorun kendi titreşimini ayırt etmek** — hem de 240 MHz'lik bir mikrodenetleyicide, gerçek zamanlı olarak.

Çözüm dört sinyalin birlikte değerlendirilmesi:

| Ölçü | Ne yakalar |
|---|---|
| **Spektral bant oranı** (1.8–5.5 kHz / toplam) | Şıklatmanın karakteristik tiz "çıt" enerjisi |
| **Düşük bant oranı** (100 Hz–1 kHz) | Alkış ve konuşmayı eler (bunlar pes ağırlıklıdır) |
| **Crest faktörü** (tepe / RMS) | Ani darbeyi sabit gürültüden/uğultudan ayırır |
| **Onset oranı** (RMS / uyarlanabilir gürültü tabanı) | "Ortama göre ani sıçrama" — sessiz ve gürültülü odada aynı davranır |

Buna MPU6050'den gelen **bilek hareketi doğrulaması** eklenir: sınırdaki sesler için hareket şartı aranır, çok net şıklatmalar tek başına kabul edilir.

---

## Özellikler

- **%50 örtüşmeli FFT analizi** — 8 ms'de bir karar; şıklatma pencere sınırına denk gelip kaybolmaz
- **Uyarlanabilir eşikler** — kalibrasyon + sürekli güncellenen gürültü tabanı, sabit sihirli sayı yok
- **Kendi kendini onaran BLE** — reklam yeniden kurulumu, bond uyuşmazlığı otomatik temizliği, kademeli kurtarma
- **4 katmanlı motor donma koruması** — donanım timer ISR'ı, yazılım yedeği, bloklamayan sürüş, task watchdog
- **Kalıcı kalibrasyon** — NVS'e yazılır, her açılışta hazır
- **Tek tuşla yönetim** — BOOT tuşu: 1.5-5 sn yeniden kalibrasyon, >5 sn BLE eşleşmelerini sıfırlama
- **Zengin seri teşhis** — her 3 saniyede tüm sinyal metrikleri + bağlantı durumu

---

## Donanım

| Bileşen | Model | Bağlantı |
|---|---|---|
| MCU | ESP32-S3 (Super Mini / DevKitC-1) | — |
| Mikrofon | INMP441 (I2S MEMS) | WS→11, SCK→12, SD→13 |
| IMU | MPU6050 (I2C) | SDA→8, SCL→9 |
| Geri bildirim | Titreşim motoru (ERM) | GPIO7 (MOSFET üzerinden) |
| Durum LED'i | Dahili WS2812 | GPIO48 |

> ⚠️ **Motor doğrudan GPIO'ya bağlanmaz.** MOSFET sürücü + flyback diyot + gate pull-down direnci gerekir. Ayrıntılar ve şema: **[docs/DONANIM.md](docs/DONANIM.md)**

---

## Kurulum

```bash
git clone https://github.com/egulbay/Snapslide.git
cd Snapslide
pio run --target upload      # PlatformIO
pio device monitor           # 115200 baud
```

İlk açılışta cihaz kalibrasyona girer: LED sarı yanıp sönerken **10 kez parmak şıklatın**. Ardından iki yeşil flaş + çift titreşim = hazır.

Sonra bilgisayarınızın Bluetooth ayarlarından **"SnapSlide"** cihazını eşleştirin.

---

## Durum göstergeleri

| LED | Anlam |
|---|---|
| Sarı yanıp sönme | Kalibrasyon — şıklatın |
| Mavi flaş | Kalibrasyon şıklatması kaydedildi |
| 2× yeşil flaş | Kalibrasyon tamamlandı |
| Yeşil | Tek şıklatma → sağ ok gönderildi |
| Mor | Çift şıklatma → sol ok gönderildi |
| Mavi (tuş basılıyken) | Bırakınca yeniden kalibre edilecek |
| Kırmızı (tuş basılıyken) | Bırakınca BLE eşleşmeleri silinecek |
| 3× turuncu flaş | Bond uyuşmazlığı temizlendi — host'tan da kaldırın |

---

## Ayar parametreleri

Tümü [`src/main.cpp`](src/main.cpp) başındaki tanımlarda:

```c
#define DOUBLE_SNAP_WINDOW 650    // çift şıklatma penceresi = tek şıklatma gecikmesi
#define ACTION_COOLDOWN_MS 900    // komut sonrası körlük süresi
#define SNAP_FREQ_LOW  1800       // şıklatma bandı alt sınırı (Hz)
#define SNAP_FREQ_HIGH 5500       // üst sınır
#define MOTION_GYRO_TH 0.9f       // hareket doğrulama eşiği (rad/s)
#define MOTOR_MAX_ON_MS 250       // motorun tek seferde açık kalabileceği azami süre
```

Hassasiyeti artırmak/azaltmak için: [docs/SORUN-GIDERME.md](docs/SORUN-GIDERME.md#hassasiyet-ayarı)

---

## Dokümantasyon

- **[docs/MIMARI.md](docs/MIMARI.md)** — sinyal işleme zinciri, görev yapısı, karar mekanizması
- **[docs/DONANIM.md](docs/DONANIM.md)** — bağlantı şeması, motor sürücü devresi, güç mimarisi
- **[docs/SORUN-GIDERME.md](docs/SORUN-GIDERME.md)** — bağlantı, algılama ve motor sorunları için teşhis rehberi

---

## Lisans

MIT — bkz. [LICENSE](LICENSE)
