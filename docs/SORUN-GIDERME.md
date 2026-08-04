# Sorun Giderme

Her şeyden önce seri portu açın — firmware kendi teşhisini yazdırır:

```bash
pio device monitor    # 115200 baud
```

Açılışta gördüğünüz `[BOOT] Reset sebebi:` satırı çoğu sorunu tek başına çözer.

---

## Bağlantı sorunları

### "Bir kere bağlandı, sonra bir daha asla"

Bu **bond (eşleşme anahtarı) uyuşmazlığıdır**. Cihazdaki anahtar silinmiş (NVS temizlendi, firmware yeniden yüklendi) ama host'ta hâlâ duruyor. Host eski anahtarla bağlanmayı dener, şifreleme başarısız olur.

**Cihazı kapatıp açmak çözmez.** Her iki tarafın da temizlenmesi gerekir:

1. **Cihaz:** BOOT tuşunu **5 saniyeden uzun** basılı tutun (LED kırmızıya döner), bırakın → eşleşmeler silinir ve cihaz yeniden başlar
2. **Windows:** Ayarlar → Bluetooth → SnapSlide → **Cihazı kaldır**
3. Yeniden eşleştirin

> Firmware bunu genelde kendi yakalar: kopma sebebi `0x05`/`0x06`/`0x3D` ise bond'u otomatik siler ve 3× turuncu flaş verir. O flaşı gördüyseniz sadece 2. adımı yapın.

### "Bağlı görünüyor ama tuşlar çalışmıyor"

Seri portta şu satıra bakın:

```
[SEN] ... | ble=BAGLI abone=hayir ...
```

`abone=hayir` ise host input report bildirimlerine abone olmamış — HID enumerasyonu tamamlanmamış demektir.

- Windows'ta eşleşmeyi kaldırıp yeniden ekleyin
- Eşleştirmeyi **Ayarlar → Bluetooth ve cihazlar → Cihaz ekle → Bluetooth** üzerinden yapın (Denetim Masası'nın eski arayüzü HID-over-GATT'ta sorun çıkarabiliyor)
- Bilgisayarınızın Bluetooth sürücüsü BLE (4.0+) destekliyor mu kontrol edin

Ayrıca `[HID] UYARI: bildirim gonderilemedi` satırı görüyorsanız bağlantı şifrelenmemiştir.

### Sık sık kopuyor

Seri portta şu satırı arayın:

```
[BLE] >>> DIKKAT: kopma, motor calistiktan N ms SONRA oldu -> besleme/parazit suphesi
```

Bu satır görünüyorsa **suç yazılımda değil**: motorun endüktif darbesi radyoyu bozuyor. Çözüm [DONANIM.md](DONANIM.md#1-flyback-diyot-d1--ble-kopmalarının-bir-numaralı-sebebi)'deki flyback diyot + bulk kapasite.

Kopma sebebi kodları:

| Kod | Anlam | Yapılacak |
|---|---|---|
| `0x08` | Supervision timeout | Radyo/besleme kesintisi → flyback diyot, mesafeyi azaltın |
| `0x06`, `0x3D` | Bond uyuşmazlığı | Yukarıdaki eşleşme sıfırlama adımları |
| `0x13` | Host kapattı | Windows uyku moduna girdi veya BT kapatıldı |
| `0x3E` | Bağlantı kurulamadı | Parazit; kanal yoğunluğu (Wi-Fi 2.4 GHz ile çakışma) |

### Cihaz hiç görünmüyor

```
[OK] BLE Klavye Aktif: SnapSlide (reklam: BASLADI)
```

`BASLAYAMADI` yazıyorsa yığın başlatılamamış — genelde NVS bozulmasıdır. Seri portta `[NVS] !!! DIKKAT` satırı varsa NVS silinmiştir, sorun kendiliğinden düzelecektir ama host'tan eşleşmeyi kaldırmanız gerekir.

---

## Teşhis modu — ayarlanacak parametreyi bulma

"Bazen algılamıyor" ayarlanabilir bir şikayet değil. Teşhis modu bunu **hangi ölçünün kaç kez elediği** sayısına indirger.

`src/main.cpp` başında açıktır (üretim için `0` yapın):

```c
#define SNAP_DIAG 1
```

Sesli her olay için tek satır düşer — her ölçü `ölçülen/eşik` formatında:

```
[DIAG] onset=2.4/3.0 crest=6.1/2.8 ratio=0.38/0.19 rms=310/180 low=0.21 hrk=E -> RED: onset
[DIAG] onset=7.8/3.0 crest=7.4/2.8 ratio=0.51/0.19 rms=890/180 low=0.14 hrk=E -> KABUL (güçlü)
```

Ve 3 saniyede bir özet:

```
[DIAG-OZET] aday=31 kabul=22(guclu 14) | RED: onset=6 crest=1 ratio=2 rms=0 alkis=0 hareket=3 sagir=1
```

**Nasıl okunur:** `RED:` satırındaki **en büyük sayı** ayarlanacak parametredir. Yukarıdaki örnekte `onset=6` baskın → `analyzeSnapAudio()` içinde `result.thOnset = 3.0f` değerini 2.4'e indirin.

Eşiği ölçülen değerlerin **hemen altına** çekin, sıfıra değil: reddedilen satırlardaki `onset` değerleri 2.3–2.7 arasındaysa 2.2 iyi bir seçimdir. Her seferinde **tek parametre** değiştirin ve testi tekrarlayın.

`hareket=N` baskınsa ses yeterli ama IMU doğrulaması eliyor demektir → `MOTION_GYRO_TH` değerini düşürün (0.9 → 0.6) veya `MOTION_WINDOW_MS`'i artırın (400 → 600).

`sagir=N` baskınsa şıklatmalarınız motorun titreşim maskesine denk geliyor → `MOTOR_MUTE_TAIL_MS` (180) veya `ACTION_COOLDOWN_MS` (900) çok uzun.

### Çift şıklatma zamanlaması

1. şıklatmadan sonraki adaylar aralıklarıyla birlikte raporlanır:

```
[DIAG] onset=5.2/3.0 ... -> RED: ... | 1.'den +780ms pencere=KAPALI gevsek=gecti
```

`pencere=KAPALI` **ve** `gevsek=gecti` görüyorsanız teşhis nettir: ikinci şıklatmanız algılanacak kadar güçlü ama **çok geç geliyor.** `DOUBLE_SNAP_WINDOW` değerini ölçülen aralığın üzerine çıkarın.

> ⚠️ **Ölü bölge:** Tek şıklatma penceresi kapanınca (650 ms) komut gönderilir ve `ACTION_COOLDOWN_MS` (900 ms) başlar. Yani **650–1550 ms arasına düşen ikinci şıklatma tamamen yutulur.** Doğal çift şıklatma ritminiz 650 ms'den yavaşsa çift şıklatma neredeyse hiç tutmaz. `DOUBLE_SNAP_WINDOW`'u ölçtüğünüz aralığa göre ayarlamak bunun tek çözümüdür — ama unutmayın, bu değer aynı zamanda **tek şıklatmanın gecikmesidir.**

---

## Algılama sorunları

### Şıklatmayı hiç algılamıyor

**Önce yeniden kalibre edin** — özellikle firmware'i güncellediyseniz. BOOT tuşunu 1.5–5 saniye basılı tutup bırakın (LED mavi olur), sonra LED sarı yanıp sönerken 10 kez şıklatın.

Sonra seri porttaki `[SEN]` satırını okuyun. Sessizken tipik bir satır:

```
[SEN] rms=45 floor=44 onset=1.0 ratio=0.12 low=0.31 crest=3.4 ...
```

Şıklatırken `onset` **3'ün üzerine** çıkmalı. Çıkmıyorsa:

| Belirti | Sebep | Çözüm |
|---|---|---|
| `rms` hiç değişmiyor | Mikrofon okumuyor | INMP441 bağlantısı, L/R pini GND'de mi |
| `onset` yükseliyor ama snap=hayir | Bant oranı düşük | Kalibrasyonu tekrarlayın; mikrofona daha yakın şıklatın |
| `floor` çok yüksek | Ortam gürültülü | Sessiz ortamda yeniden kalibre edin |
| Sadece bazen kaçırıyor | Hareket kapısı eliyor | Şıklatırken bileği daha belirgin hareket ettirin, veya `MOTION_GYRO_TH` değerini düşürün |

### Alkışı/öksürüğü şıklatma sanıyor

`low` (düşük bant oranı) değerine bakın — alkışta 0.7'nin üzerinde olmalı. Yanlış pozitifleri azaltmak için `main.cpp` içinde:

```c
bool notClap = result.lowBandRatio < 0.68f;   // 0.55'e düşürün → daha seçici
bool onsetOK = result.onset > 3.0f;           // 4.5'e çıkarın → daha seçici
```

### Çift şıklatma tek sayılıyor

İki şıklatma arası çok uzun. `DOUBLE_SNAP_WINDOW` (varsayılan 650 ms) süresini artırın — ama bu doğrudan tek şıklatmanın gecikmesidir, 900 ms'nin üzerine çıkarmayın.

Ya da 2. şıklatma gevşek eşiğe bile takılmıyordur; `[SNAP!] 2. Siklatma` satırı hiç görünmüyorsa ikinciyi biraz daha kuvvetli yapın.

### Hassasiyet ayarı

`src/main.cpp` içinde `analyzeSnapAudio()` fonksiyonundaki çarpanlar:

```c
bool ratioOK = result.snapBandRatio > (calSnapRatioAvg * 0.32f);  // ↓ = daha hassas
bool rmsOK   = result.rms           > (calSnapRmsAvg   * 0.18f);  // ↓ = daha hassas
bool onsetOK = result.onset > 3.0f;                                // ↓ = daha hassas
```

Her seferinde **tek bir değeri** değiştirin ve seri porttan sonucu izleyin.

---

## Motor sorunları

### Motor durmuyor / sürekli titriyor

Seri portta `motorKurt=` sayacına bakın (`[SEN]` satırının sonunda):

- **`motorKurt=0`** ve motor yine de takılıyorsa → sorun **donanımdadır**. MCU reset halindeyken GPIO7 havada kalıyor ve MOSFET kapısı kaçak akımla açılıyor. **Gate–GND arasına 10 kΩ direnç** gerekir: [DONANIM.md](DONANIM.md#2-gate-pull-down-direnci-r1--motorun-takılı-kalmasının-donanım-sebebi)
- **`motorKurt>0`** → donanım timer'ı çalışmıyor, yazılım yedeği devreye giriyor. Motor duruyor ama sebebi araştırılmalı; açılışta `[MOTOR] UYARI: donanim timer'i kurulamadi` satırı var mı bakın.

### Sürekli yeniden başlıyor

`[BOOT] Reset sebebi:` satırı:

| Sebep | Anlam | Çözüm |
|---|---|---|
| `BROWNOUT` | Besleme motor darbesinde çöküyor | Bulk kapasite + motoru ayrı besleyin |
| `TASK_WDT` | Bir görev 5 sn takıldı | Seri log'un son satırlarına bakın; genelde I2C'de asılı kalan MPU |
| `PANIC` | Crash | `monitor_filters = esp32_exception_decoder` açık, stack trace çözümlenmiş olarak gelir |
| `INT_WDT` | Kesmeler kapalı kaldı | Kritik bölümde uzun işlem |

---

## Fabrika ayarlarına dönüş

```bash
pio run --target erase        # tüm flash'ı siler (kalibrasyon + bond)
pio run --target upload
```

Ardından Windows'tan da eşleşmeyi kaldırmayı unutmayın.
