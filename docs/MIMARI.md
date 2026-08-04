# Mimari

## Görev yapısı

Her iki görev de **core 1**'de çalışır ve ikisi de Task Watchdog'a abonedir.

```
┌─ sensorTask (core 1) ──────────────────────────────────┐
│  i2s_read(128 örnek)  ← döngü temposunu bu belirler    │
│        ↓  ~8 ms                                        │
│  kayan pencere (256 örnek, %50 örtüşme)                │
│        ↓                                               │
│  DC çıkar → RMS / crest → FFT → bant oranları          │
│        ↓                                               │
│  MPU6050 okuması (400 kHz I2C, ~0.5 ms)                │
│        ↓                                               │
│  karar → snapCount durum makinesi                      │
└────────────────────────┬───────────────────────────────┘
                         │ finalSnapResult (portMUX korumalı)
┌─ loop() (core 1) ──────┴───────────────────────────────┐
│  BLE HID tuş gönderimi → titreşim → LED                │
│  vibrateService()  · motorWatchdog()  · bleHealthCheck()│
└────────────────────────────────────────────────────────┘
```

---

## Sinyal işleme zinciri

### 1. Örtüşmeli pencereleme

Şıklatma **3–5 ms**'lik bir darbedir; analiz penceresi ise 256 örnek = **16 ms**. Örtüşmeyen pencerelerde darbe tam pencere sınırına denk gelirse enerjisi ikiye bölünür, her iki pencerede de eşiğin altında kalır ve şıklatma **tamamen kaybolur**.

Çözüm: her çağrıda sadece 128 yeni örnek okunur, pencerenin geri kalanı devralınır. Böylece **8 ms'de bir** yeni karar üretilir ve hiçbir darbe bölünmez.

### 2. DC bileşen çıkarma

INMP441 kayda değer bir DC ofset üretir. Bu ofset:
- RMS'i yapay olarak yükseltir → genlik eşikleri anlamını yitirir
- tepe/RMS oranını 1'e doğru ezer → **crest testi tamamen işe yaramaz hale gelir**

Her pencerede ortalama hesaplanıp çıkarılır. Crest faktörü ancak bundan sonra gerçek bir ayırt edici olur (sabit ton ≈ 1.4, gürültü ≈ 3.5, darbe > 6).

### 3. Onset (ani darbe) ölçüsü

Mutlak genlik eşiği yerine **göreceli** ölçü:

```
onset = mevcut RMS / uyarlanabilir gürültü tabanı
```

Gürültü tabanı yavaş bir EMA'dır ve yalnızca **darbe olmayan + motor kapalı** karelerde ilerler — böylece ne şıklatmanın kendisi ne de motorun titreşimi tabanı yukarı çekip sonraki şıklatmaları köreltebilir. Yükselirken temkinli (α=0.02), düşerken hızlı (α=0.20): oda sessizleşince çabuk uyum sağlar.

Bu sayede sistem sessiz bir toplantı odasında da, uğultulu bir konferans salonunda da aynı davranır.

### 4. Spektral ayırt etme

256 noktalı **float** FFT (ESP32-S3'ün FPU'su tek hassasiyetlidir; `double` yazılımda emüle edilir ve ~10 kat yavaştır).

| Bant | Aralık | Rol |
|---|---|---|
| Snap bandı | 1.8 – 5.5 kHz | şıklatmanın karakteristik enerjisi |
| Düşük bant | 100 Hz – 1 kHz | alkış/konuşma/kapı sesi elemesi |

### 5. İki kademeli karar

```
GÜÇLÜ  = tüm ölçüler geniş farkla sağlanıyor  → hareket doğrulaması ARANMAZ
NORMAL = ölçüler eşiğin üzerinde              → MPU hareket doğrulaması ŞART
GEVŞEK = düşürülmüş eşikler                   → yalnızca 2. şıklatma için
```

Bu **"VEYA"** yapısı kritik: sesi gayet net olan bir şıklatmanın MPU eşiğine takılıp elenmesi, kaçırılan algılamaların en büyük kaynağıydı. Aynı zamanda sınırdaki seslerde hareket şartı korunduğu için yanlış pozitifler artmaz.

Hareket doğrulaması hem **gyro büyüklüğünden** hem de **ivme darbesinden** tetiklenir — şıklatırken bilek yalnızca dönmez, keskin bir ivme darbesi de üretir.

### 6. Motor sağırlaştırma

Motor çalışırken ve sonrasındaki 180 ms boyunca hem karar verme hem de gürültü tabanı güncellemesi durdurulur. Motor mikrofonun yanındaki mekanik bir gürültü kaynağıdır ve bu olmadan hem sahte tetiklemeler hem de taban şişmesi kaynaklı kaçırmalar oluşur.

---

## Motor güvenliği

Motorun titreşimde takılı kalması bu projedeki en ciddi arızaydı. **Kök neden:** eski sürümde motor kesme yedeği `esp_timer` ile kuruluyordu. Bu framework derlemesinde `CONFIG_ESP_TIMER_SUPPORTS_ISR_DISPATCH_METHOD` kapalıdır — yani tüm `esp_timer` geri çağrıları **tek bir görev kuyruğunda** sırayla işlenir. LED yedek kapatması `neopixelWrite → rmtWriteBlocking → xSemaphoreTake(portMAX_DELAY)` zincirini çağırıyordu. Core 1'de bir takılma olup RMT kanal mutex'i tutulu kalırsa `esp_timer` görevi sonsuza kadar bloklanır, kuyrukta arkasında bekleyen **motor kesme geri çağrısı hiç çalışmaz** ve MOTOR_PIN HIGH'ta kilitlenir.

Şimdi dört bağımsız katman var:

| # | Katman | Neyi kurtarır |
|---|---|---|
| 1 | **Donanım timer ISR'ı** | Kuyruk/görev/mutex yok, doğrudan GPIO register yazımı. `ESP_INTR_FLAG_IRAM` sayesinde NVS flash yazımı sırasında (cache kapalıyken) bile çalışır. |
| 2 | **Yazılım yedeği** (`motorWatchdog`) | Her iki görev de bağımsız kontrol eder; donanım timer'ı hiç kurulamamış olsa bile motor kesilir. Kurtarma sayısı seri porta raporlanır. |
| 3 | **Bloklamayan sürüş** | Titreşim bir durum makinesidir; motor açıkken kod hiçbir `delay()` içinde beklemez. "delay içinde donma → motor açık kaldı" senaryosu ortadan kalkar. |
| 4 | **Task Watchdog** | Gerçek bir donmada 5 sn'de panik/reboot. |

> Katman 1–4 yazılımdır ve **MCU reset halindeyken hiçbiri çalışamaz**. O boşluğu kapatan tek şey gate pull-down direncidir — bkz. [DONANIM.md](DONANIM.md#2-gate-pull-down-direnci-r1--motorun-takılı-kalmasının-donanım-sebebi).

---

## BLE dayanıklılığı

### Bağlantı parametreleri

Bağlantıdan ~1 sn sonra istenir (geri çağrının içinden değil — yığın geri çağrısının içinden yığına iş yaptırılmaz, katman-1 dersinin aynısı):

```
aralık 15–30 ms · latency 0 · supervision timeout 5 sn
```

Uzun supervision timeout kritik: motor akımı veya parazit kaynaklı kısa radyo kesintileri bu süre içinde toparlanırsa bağlantı **hiç düşmez**.

### Kademeli kurtarma (`bleHealthCheck`, saniyede bir)

| Süre | Aksiyon |
|---|---|
| Sürekli | Reklam durmuşsa yeniden başlat |
| 60 sn | Reklamı sıfırdan kur (yığın "yayınlıyorum" der ama radyoda görünmüyor olabilir) |
| 180 sn | Cihazı yeniden başlat — yalnızca bu oturumda daha önce bağlanıldıysa |

### Bond uyuşmazlığı otomatik onarımı

Kopma sebebi `0x05` / `0x06` / `0x3D` ise cihazdaki eşleşme anahtarı ile host'taki uyuşmuyor demektir. Bu, kopmanın **onarılamaz** türüdür: host her seferinde eski anahtarla bağlanmayı dener, şifreleme başarısız olur, bağlantı düşer. Kullanıcı açısından tam olarak *"bir kere bağlandı, sonra bir daha asla"* tablosudur ve cihazı kapatıp açmak **çözmez**.

Firmware bu kodları görünce kendi tarafındaki bond'u siler ve 3× turuncu flaş verir. **Host tarafındaki eşleşmenin de kaldırılması gerekir.**

### PnP ID

Windows, HID-over-GATT cihazlarını sürücüye bağlarken Device Information servisindeki **PnP ID** karakteristiğini okur. Bu alan boş kalırsa cihaz eşleşse bile klavye olarak enumerate edilmeyebilir — *"bağlı görünüyor ama tuşlar çalışmıyor"* arızasının klasik sebebi. Artık `0x02 / 0x303A / 0x8000 / 0x0110` olarak ayarlanır.

### Gönderim doğrulaması

`notify()` bir `bool` döndürür. Eskiden bu değer hiç kontrol edilmiyordu — yani "gönderdim" denip aslında hiçbir şey gitmemiş olabiliyordu. Artık başarısız gönderim bir kez tekrarlanır ve başarısızlık abonelik/şifreleme durumuyla birlikte loglanır.

Host'un CCCD aboneliği `onSubscribe` ile izlenir, **ama gönderim buna göre engellenmez**: bonded bir host'a yeniden bağlanıldığında CCCD host tarafında saklıdır ve `onSubscribe` tekrar tetiklenmeyebilir.

---

## Sıralama kuralı: önce radyo, sonra motor

Aksiyon uygulanırken BLE bildirimi motordan **önce** gönderilir. Motorun akım darbesi ve ürettiği parazit, aynı anda yapılan radyo iletimini bozabiliyor. `sendKeyPress` içindeki 25 ms bekleme, bildirimin radyodan çıkması için gereken ayrımı sağlar.
