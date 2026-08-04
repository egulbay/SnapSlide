# Donanım Rehberi

Malzeme listesi için: **[MALZEME.md](MALZEME.md)**

![Kart üst yüzü](images/pcb-ust-yuz.jpg)

## Pin bağlantıları

| İşlev | ESP32-S3 Pin | Not |
|---|---|---|
| I2S WS (LRCL) | GPIO11 | INMP441 |
| I2S SCK (BCLK) | GPIO12 | INMP441 |
| I2S SD (DOUT) | GPIO13 | INMP441 |
| I2C SDA | GPIO8 | MPU6050 (0x68) |
| I2C SCL | GPIO9 | MPU6050, 400 kHz |
| Motor kapısı | GPIO7 | MOSFET gate — **doğrudan motor değil** |
| RGB LED | GPIO48 | Dahili WS2812 |
| BOOT tuşu | GPIO0 | Dahili pull-up |

INMP441 `L/R` pini **GND**'ye bağlanır (kod `I2S_CHANNEL_FMT_ONLY_LEFT` kullanır).

---

## Motor sürücü devresi — kritik bölüm

Bu projedeki iki büyük arızanın (**motorun titreşimde takılı kalması** ve **BLE bağlantısının kopması**) ortak kökü motor sürüş devresidir. Aşağıdaki üç bileşen opsiyonel değildir.

```
                    VMOT (5V / batarya — ESP32'nin 3.3V regülatör çıkışı DEĞİL)
                      │
                      ├──────────────┬─────────────┐
                      │              │             │
                   ┌──┴──┐        ═══╪═══       ═══╪═══
                   │MOTOR│       C1 220µF      C2 100nF
                   │ ERM │      (elektrolit)   (seramik)
                   └──┬──┘          │             │
                      ├──────┐      │             │
                     ─┴─     │      │             │
                  D1 ▲ 1N5819│      │             │   D1: KATOT motorun + ucuna
                     ─┬─     │      │             │
                      │      │      │             │
                      └──────┤      │             │
                             │      │             │
                          ┌──┴──┐   │             │
              GPIO7 ──────┤ G   │   │             │
                     │    │ MOSFET (AO3400 / 2N7000 / IRLZ44N — logic level)
                     R1   │ D─S │   │             │
                    10kΩ  └──┬──┘   │             │
                     │       │      │             │
                    GND─────GND────GND───────────GND  (ortak toprak şart)
```

### 1. Flyback diyot (D1) — **BLE kopmalarının bir numaralı sebebi**

Motor endüktif bir yüktür. MOSFET kapandığı anda bobin akımı devam etmek ister ve **onlarca voltluk ters gerilim** üretir. Bu darbe:

- 2.4 GHz bandına geniş spektrumlu parazit basar → BLE paketleri kaybolur → **supervision timeout (0x08) ile bağlantı düşer**
- MOSFET'i zamanla öldürür
- Besleme hattında salınım yaratır

**Çözüm:** Motorun uçlarına ters yönde bir Schottky diyot (1N5819 / SS14) veya hızlı diyot (1N4148, küçük motorlar için). Katot motorun **+** ucuna bakar.

> Firmware bu arızayı sizin için teşhis eder: kopma motorun çalışmasından 1.5 saniye içinde olursa seri porta
> `>>> DIKKAT: kopma, motor calistiktan N ms SONRA oldu` satırı düşer. Bu satırı görüyorsanız sorun yazılımda değil, bu devrededir.

### 2. Gate pull-down direnci (R1) — **motorun takılı kalmasının donanım sebebi**

ESP32 reset atarken, boot yaparken veya firmware yüklenirken GPIO7 **yüksek empedanslı giriş** durumundadır — yani havada kalır. Havada kalan bir MOSFET kapısı kaçak akımla şarj olur ve motor **kendiliğinden dönmeye başlar**. Bu tam olarak "fişi çekene kadar durmuyor" tablosudur, çünkü sistem her reset attığında aynı pencere tekrarlanır.

**Çözüm:** Gate ile GND arasına **10 kΩ** direnç. Kapının varsayılan durumu artık "kapalı"dır ve MCU'nun ne yaptığından bağımsızdır.

> Firmware tarafında dört ayrı koruma katmanı var (bkz. [MIMARI.md](MIMARI.md#motor-güvenliği)), ama hiçbiri MCU resetteyken çalışamaz. Bu direnç o boşluğu kapatan tek şeydir.

### 3. Bulk kapasite (C1) + bypass (C2) — **brownout resetlerinin çaresi**

ERM motorunun kalkış akımı anlık olarak 200-300 mA'e çıkar. ESP32-S3'ün radyo TX darbesi (+9 dBm) ile aynı ana denk gelirse zayıf bir regülatör çöker → **brownout reset**.

**Çözüm:** Motor beslemesine paralel 220-470 µF elektrolitik (C1) + motor uçlarına 100 nF seramik (C2, fırça gürültüsünü bastırır).

> Firmware açılışta reset sebebini yazdırır. `BROWNOUT (besleme cokmesi - donanim!)` görüyorsanız sorun kesinlikle budur.

### 4. Motoru ayrı besleyin

Motoru ESP32'yi besleyen 3.3V LDO çıkışından **çekmeyin**. Bu regülatörler tipik olarak 500 mA verir ve MCU + radyo zaten bunun yarısını kullanır. Motoru 5V/VBUS hattından veya doğrudan bataryadan sürün, toprakları ortaklayın.

---

## Mikrofon yerleşimi

Titreşim motoru mikrofonun yanındaki **mekanik bir gürültü kaynağıdır**. Firmware motor çalışırken mikrofonu 180 ms boyunca sağırlaştırıyor, ama yine de:

- Mikrofonu motordan mümkün olduğunca uzağa koyun
- Aralarına yumuşak bir ayırıcı (köpük/silikon) yerleştirin
- Mikrofonun ses deliğini kasa ile tıkamayın

---

Motor plaketin alt yüzünde, mikrofon ise kablo ucunda ayrı duruyor:

![Kart alt yüzü ve titreşim motoru](images/pcb-alt-yuz.jpg)

---

## Doğrulama kontrol listesi

Cihazı çalıştırmadan önce:

- [ ] D1 diyodun yönü doğru mu? (katot = motorun + ucu, ters takılırsa kısa devre)
- [ ] R1 gate–GND arasında mı? (gate–VMOT arasında değil)
- [ ] MOSFET **logic-level** mi? (IRF540 gibi standart MOSFET'ler 3.3V ile tam açılmaz, ısınır)
- [ ] Motor ve ESP32 toprakları ortak mı?
- [ ] INMP441 L/R pini GND'de mi?
- [ ] Cihazı ilk açtığınızda motor sessiz mi? (dönüyorsa R1 yok veya bağlantısı kopuk)
