// ============================================================================
//  SnapSlide - Parmak siklatmasiyla sunum kontrolu
//  ESP32-S3 + INMP441 (I2S) + MPU6050 + titresim motoru + BLE HID klavye
//
//  Tek siklatma  -> SAG OK  (sonraki slayt)
//  Cift siklatma -> SOL OK  (onceki slayt)
//
//  BOOT tusu (GPIO0) basili tutma suresine gore:
//    1.5 - 5 sn  -> yeniden kalibrasyon
//    > 5 sn      -> BLE eslesmelerini sil + yeniden baslat
// ============================================================================
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "soc/gpio_reg.h"
#include "soc/soc.h"
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include <Wire.h>
#include <arduinoFFT.h>
#include <driver/i2s.h>
#include <math.h>
#include <string.h>

// ==================== PIN TANIMLARI (DEGISTIRME!) ====================
#define I2S_WS 11
#define I2S_SCK 12
#define I2S_SD 13
#define SDA_PIN 8
#define SCL_PIN 9
#define RGB_LED 48
#define MOTOR_PIN 7
#define BOOT_BUTTON 0

// ==================== SES / FFT ====================
#define SAMPLE_RATE 16000
#define FFT_SAMPLES 256 // pencere: 16 ms
// %50 ORTUSME. Bu, "bazen algilamiyor" sorununun en buyuk tek sebebinin
// panzehiri: siklatma ~3-5 ms'lik bir darbedir. Ortusmeyen pencerelerde darbe
// tam pencere sinirina denk gelirse enerjisi ikiye bolunur, her iki pencerede
// de esigin altinda kalir ve siklatma TAMAMEN kaybolur. 128 orneklik atlama
// ile her 8 ms'de bir yeni karar uretilir ve hicbir darbe bolunmez.
#define AUDIO_HOP 128

// Snap frekans bandi (Hz)
#define SNAP_FREQ_LOW 1800
#define SNAP_FREQ_HIGH 5500
// Dusuk frekans bandi (alkis / konusma filtresi)
#define LOW_FREQ_LOW 100
#define LOW_FREQ_HIGH 1000

// ==================== ZAMANLAMA ====================
#define SNAP_COOLDOWN_MS 150
// 1. ve 2. siklatma arasi minimum bosluk (cift siklatma hizli yapilir).
#define SECOND_SNAP_MIN_GAP_MS 90
// Cift siklatma bekleme penceresi. Tek siklatma ancak bu sure DOLDUKTAN sonra
// tetiklenir, yani bu sure dogrudan "sonraki slayt" gecikmesidir. Eski deger
// 1300 ms idi ve sunum sirasinda belirgin sekilde yavas hissettiriyordu;
// gercek bir cift siklatma en fazla ~400 ms surdugu icin 650 ms fazlasiyla
// yeterli ve iki kat daha canli.
#define DOUBLE_SNAP_WINDOW 650
#define ACTION_COOLDOWN_MS 900

// Hareket kapisi
#define MOTION_WINDOW_MS 400  // siklatma oncesi hareket ne kadar taze olmali
#define MOTION_GYRO_TH 0.9f   // rad/s (3 eksen mutlak toplami)
#define MOTION_ACC_TH 2.0f    // m/s^2, 1g'den sapma

// HID tus kodlari
#define KEY_RIGHT_ARROW 0x4F
#define KEY_LEFT_ARROW 0x50

// ==================== FFT ====================
// float (double degil): ESP32-S3 tek hassasiyetli donanim FPU'suna sahiptir,
// double islemler YAZILIMDA emule edilir. float'a gecmek FFT'yi yaklasik bir
// kat hizlandirir ve %50 ortusmeli analiz icin gereken CPU butcesini acar.
float vReal[FFT_SAMPLES];
float vImag[FFT_SAMPLES];
ArduinoFFT<float> FFT =
    ArduinoFFT<float>(vReal, vImag, FFT_SAMPLES, SAMPLE_RATE);

// ==================== GLOBAL DEGISKENLER ====================
Adafruit_MPU6050 mpu;
bool calibrated = false;
bool mpuFound = false;
volatile bool deviceConnected = false;

// BLE dayaniklilik / teshis durumu
volatile unsigned long lastVibrateMs = 0;
volatile unsigned long disconnectedSince = 0;
volatile bool everConnected = false;
volatile unsigned long connParamsDueAt = 0;
volatile uint16_t activeConnHandle = 0;
volatile bool hidSubscribed = false;  // host input report CCCD'sini acti mi
volatile bool linkEncrypted = false;  // baglanti sifreli mi (HID icin sart)
volatile bool bondWipePending = false; // bond uyusmazligi -> temizle
volatile uint32_t connectCount = 0;

// Kalibrasyon verileri
float calSnapRatioAvg = 0; // snap band orani ortalamasi
float calSnapRmsAvg = 0;   // snap RMS ortalamasi
float calSnapCrestAvg = 0; // snap tepe/RMS orani ortalamasi
float noiseFloor = 0;      // kalibrasyon anindaki gurultu zemini

// Cift siklatma state machine
enum SnapResult { SNAP_NONE, SNAP_SINGLE, SNAP_DOUBLE };
volatile SnapResult finalSnapResult = SNAP_NONE;
volatile int snapCount = 0;
volatile unsigned long snapWindowStart = 0;

volatile unsigned long lastMotionTime = 0;
volatile unsigned long lastActionTime = 0;
portMUX_TYPE snapMux = portMUX_INITIALIZER_UNLOCKED;

NimBLEHIDDevice *hid = nullptr;
NimBLECharacteristic *inputKeyboard = nullptr;

// Faz 2'de gercek ADC pil voltaj okumasi buraya eklenecek.
uint8_t readBatteryPercent() { return 100; }

// ==================== HID KEYBOARD DESCRIPTOR ====================
static const uint8_t hidReportDescriptor[] = {
    0x05, 0x01, // Usage Page (Generic Desktop)
    0x09, 0x06, // Usage (Keyboard)
    0xA1, 0x01, // Collection (Application)
    0x85, 0x01, //   Report ID (1)
    0x05, 0x07, //   Usage Page (Keyboard)
    0x19, 0xE0, //   Usage Min (Left Ctrl)
    0x29, 0xE7, //   Usage Max (Right GUI)
    0x15, 0x00, //   Logical Min (0)
    0x25, 0x01, //   Logical Max (1)
    0x75, 0x01, //   Report Size (1)
    0x95, 0x08, //   Report Count (8)
    0x81, 0x02, //   Input (Data,Var,Abs) - Modifiers
    0x95, 0x01, //   Report Count (1)
    0x75, 0x08, //   Report Size (8)
    0x81, 0x01, //   Input (Const) - Reserved
    0x95, 0x06, //   Report Count (6)
    0x75, 0x08, //   Report Size (8)
    0x15, 0x00, //   Logical Min (0)
    0x25, 0x65, //   Logical Max (101)
    0x05, 0x07, //   Usage Page (Keyboard)
    0x19, 0x00, //   Usage Min (0)
    0x29, 0x65, //   Usage Max (101)
    0x81, 0x00, //   Input (Data,Array,Abs) - Keys
    0xC0        // End Collection
};

// ==================== NVS ====================
// v6: analiz zinciri degisti (DC bilesen cikarildi, ortusmeli pencere).
// RMS olcegi artik farkli oldugundan ESKI KALIBRASYON GECERSIZ. Namespace'i
// yukseltmek cihazi bir kez yeniden kalibrasyona zorlar; aksi halde eski
// (DC ile sismis) esikler yeni sinyale gore fazla yuksek kalir ve cihaz
// hicbir siklatmayi algilamaz.
#define NVS_CAL_NS "snapcal_v6"

bool nvsSaveCalibration() {
  nvs_handle_t h;
  if (nvs_open(NVS_CAL_NS, NVS_READWRITE, &h) != ESP_OK)
    return false;
  nvs_set_u32(h, "sratio", (uint32_t)(calSnapRatioAvg * 1000.0f));
  nvs_set_u32(h, "srms", (uint32_t)(calSnapRmsAvg * 100.0f));
  nvs_set_u32(h, "crest", (uint32_t)(calSnapCrestAvg * 1000.0f));
  nvs_set_u32(h, "noise", (uint32_t)(noiseFloor * 100.0f));
  nvs_set_u8(h, "ok", 0xAB);
  esp_err_t err = nvs_commit(h);
  nvs_close(h);
  return (err == ESP_OK);
}

bool nvsLoadCalibration() {
  nvs_handle_t h;
  if (nvs_open(NVS_CAL_NS, NVS_READONLY, &h) != ESP_OK)
    return false;
  uint8_t ok = 0;
  nvs_get_u8(h, "ok", &ok);
  if (ok != 0xAB) {
    nvs_close(h);
    return false;
  }
  uint32_t v = 0;
  nvs_get_u32(h, "sratio", &v);
  calSnapRatioAvg = (float)v / 1000.0f;
  nvs_get_u32(h, "srms", &v);
  calSnapRmsAvg = (float)v / 100.0f;
  nvs_get_u32(h, "crest", &v);
  calSnapCrestAvg = (float)v / 1000.0f;
  nvs_get_u32(h, "noise", &v);
  noiseFloor = (float)v / 100.0f;
  nvs_close(h);
  return (calSnapRmsAvg > 0);
}

// ==================== LED ====================
static inline void ledOn(uint8_t r, uint8_t g, uint8_t b) {
  neopixelWrite(RGB_LED, r, g, b);
}
static inline void ledOff() { neopixelWrite(RGB_LED, 0, 0, 0); }

void flashLED(uint8_t r, uint8_t g, uint8_t b, int ms) {
  ledOn(r, g, b);
  delay(ms);
  ledOff();
}

void flashLEDTimes(uint8_t r, uint8_t g, uint8_t b, int ms, int times) {
  for (int i = 0; i < times; i++) {
    ledOn(r, g, b);
    delay(ms);
    ledOff();
    if (i < times - 1)
      delay(ms);
  }
}

// ==================== GUVENLIK: DONMA-KORUMALI MOTOR ====================
// KOK NEDEN (dogrulandi): Eski surumde motor kesme yedegi esp_timer ile
// kuruluyordu. Bu framework derlemesinde (arduino-esp32 2.0.17)
// CONFIG_ESP_TIMER_SUPPORTS_ISR_DISPATCH_METHOD *kapali*, yani TUM esp_timer
// callback'leri TEK bir task kuyrugunda sirayla islenir. LED yedek kapatmasi
// neopixelWrite -> rmtWriteBlocking -> xSemaphoreTake(..., portMAX_DELAY)
// zincirini cagiriyordu. Core 1'de bir takilma olup RMT kanal mutex'i tutulu
// kalirsa esp_timer task'i SONSUZA kadar blokluyor, arkasinda kuyrukta bekleyen
// motor kesme callback'i HIC calismiyor -> MOTOR_PIN HIGH'ta kilitli kaliyordu.
//
// COZUM - 4 BAGIMSIZ KATMAN (biri calismasa digeri yakalar):
//  1) Donanim timer ISR'i: kuyruk/task/mutex yok, dogrudan GPIO register
//     yazimi. ESP_INTR_FLAG_IRAM sayesinde NVS flash yazimi sirasinda
//     (cache kapaliyken) bile calisir.
//  2) Yazilim yedegi (motorWatchdog): iki task da bagimsiz olarak "motor
//     olmasi gerekenden uzun suredir acik mi" diye bakar ve register'a
//     dogrudan yazarak keser. Donanim timer'i hic kurulamamis olsa bile
//     (timerBegin NULL dondu) motor takili kalmaz.
//  3) Titresim artik BLOKLAMAYAN durum makinesi: motor acikken kod hicbir
//     delay() icinde beklemez, dolayisiyla "delay icinde donma -> motor acik
//     kaldi" senaryosu tamamen ortadan kalkar.
//  4) Task Watchdog: core 1 task'lari abone; gercek bir donmada 5 sn'de
//     panik/reboot, reset aninda gate pull-down direnci motoru kesin keser.

#define MOTOR_MAX_ON_MS 250 // motor tek seferde en fazla bu kadar acik kalir
#define MOTOR_SW_GRACE_MS 60 // yazilim yedegi donanim timer'indan bu kadar sonra
#define WDT_TIMEOUT_S 5

// ISR dogrudan register'a yaziyor; GPIO_OUT_W1TC_REG yalnizca GPIO 0-31 icindir.
static_assert(MOTOR_PIN < 32, "MOTOR_PIN 32'den kucuk olmali (GPIO_OUT_W1TC_REG)");

static hw_timer_t *motorKillTimer = nullptr;
static volatile unsigned long motorOnSince = 0;  // 0 = motor kapali
static volatile uint16_t motorOnBudgetMs = 0;
static volatile uint32_t motorSwRescues = 0;     // yazilim yedegi kac kez girdi

// Motor calisirken mikrofon sagir edilir. Titresim motoru mikrofonun HEMEN
// yanindaki mekanik bir gurultu kaynagidir: hem sahte siklatma tetikler hem de
// gurultu tabanini sisirerek SONRAKI gercek siklatmalarin kacirilmasina yol
// acar. Bu iki ariza da kullanicinin "motor" ve "algilamama" sikayetlerinin
// ortak koku.
#define MOTOR_MUTE_TAIL_MS 180
static volatile unsigned long audioMuteUntil = 0;

// Donanim timer kesmesi: kuyruk/task/mutex yok, sadece tek register yazimi.
static void IRAM_ATTR motorKillISR() {
  REG_WRITE(GPIO_OUT_W1TC_REG, (uint32_t)1 << MOTOR_PIN);
}

void safetyTimersInit() {
  // timer 0, bolen 80 -> 80MHz/80 = 1MHz, yani 1 tick = 1 us
  motorKillTimer = timerBegin(0, 80, true);
  if (motorKillTimer) {
    timerAttachInterruptFlag(motorKillTimer, &motorKillISR, true,
                             ESP_INTR_FLAG_IRAM);
  } else {
    Serial.println("[MOTOR] UYARI: donanim timer'i kurulamadi, yalnizca "
                   "yazilim yedegi aktif.");
  }
}

static inline void motorOn(int ms) {
  if (ms > MOTOR_MAX_ON_MS)
    ms = MOTOR_MAX_ON_MS;
  if (motorKillTimer) {
    // Once yedegi kur, SONRA motoru ac -> arada donsa bile kesme garantili.
    timerAlarmDisable(motorKillTimer);
    timerWrite(motorKillTimer, 0);
    timerAlarmWrite(motorKillTimer, (uint64_t)ms * 1000ULL, false); // tek atim
    timerAlarmEnable(motorKillTimer);
  }
  motorOnBudgetMs = (uint16_t)ms;
  motorOnSince = millis();
  audioMuteUntil = millis() + ms + MOTOR_MUTE_TAIL_MS;
  digitalWrite(MOTOR_PIN, HIGH);
}

static inline void motorOff() {
  digitalWrite(MOTOR_PIN, LOW);
  motorOnSince = 0;
  if (motorKillTimer)
    timerAlarmDisable(motorKillTimer);
  lastVibrateMs = millis();
}

// KATMAN 2: iki task da cagirir. Donanim timer'i herhangi bir sebeple
// calismadiysa motoru kesin olarak kapatir ve olayi sayar.
static void motorWatchdog() {
  unsigned long since = motorOnSince;
  if (since == 0)
    return;
  if (millis() - since > (unsigned long)motorOnBudgetMs + MOTOR_SW_GRACE_MS) {
    REG_WRITE(GPIO_OUT_W1TC_REG, (uint32_t)1 << MOTOR_PIN);
    motorOnSince = 0;
    motorSwRescues++;
  }
}

// ==================== TITRESIM (BLOKLAMAYAN) ====================
// Diyot yok (flyback koruma yok) -> PWM KULLANILMIYOR, duz dijital ac/kapa
// (darbe basina sadece 2 anahtarlama gecisi, riski en aza indirir).
// Durum makinesi: motor acikken hicbir delay() calismaz.
static struct {
  uint8_t pulsesLeft;
  uint16_t onMs;
  uint16_t offMs;
  bool isOn;
  unsigned long nextAt;
} vib = {0, 0, 0, false, 0};

void vibrateAsync(int ms, int times) {
  if (ms > MOTOR_MAX_ON_MS)
    ms = MOTOR_MAX_ON_MS;
  if (times < 1)
    times = 1;
  vib.onMs = (uint16_t)ms;
  vib.offMs = (uint16_t)ms;
  vib.pulsesLeft = (uint8_t)times;
  vib.isOn = false;
  vib.nextAt = millis(); // hemen basla
}

// loop()'tan surekli cagrilir. Motoru zamaninda acar/kapatir.
void vibrateService() {
  if (vib.pulsesLeft == 0 && !vib.isOn)
    return;
  if ((long)(millis() - vib.nextAt) < 0)
    return;

  if (!vib.isOn) {
    if (vib.pulsesLeft == 0)
      return;
    motorOn(vib.onMs);
    vib.isOn = true;
    vib.nextAt = millis() + vib.onMs;
  } else {
    motorOff();
    vib.isOn = false;
    vib.pulsesLeft--;
    vib.nextAt = millis() + vib.offMs;
  }
}

// Yalnizca setup/kalibrasyon gibi loop()'un donmedigi yerlerde kullanilir.
void vibrateBlocking(int ms, int times) {
  if (ms > MOTOR_MAX_ON_MS)
    ms = MOTOR_MAX_ON_MS;
  for (int i = 0; i < times; i++) {
    motorOn(ms);
    delay(ms);
    motorOff();
    if (i < times - 1)
      delay(ms);
  }
}

// ==================== TASK WATCHDOG ====================
// Bu derlemede CHECK_IDLE_TASK_CPU1 kapali; loop() ve sensorTask core 1'de
// calistigi icin aksi halde HICBIR sey onlari izlemiyor.
static void wdtSubscribe(const char *who) {
  esp_err_t e = esp_task_wdt_add(NULL);
  if (e == ESP_ERR_INVALID_STATE) { // TWDT hic baslatilmamissa baslat
    esp_task_wdt_init(WDT_TIMEOUT_S, true);
    e = esp_task_wdt_add(NULL);
  }
  Serial.printf("[WDT] %s abone edildi: %s\n", who, esp_err_to_name(e));
}
static inline void wdtFeed() { esp_task_wdt_reset(); }

// ==================== RESET SEBEBI ====================
static const char *resetReasonName(esp_reset_reason_t r) {
  switch (r) {
  case ESP_RST_POWERON:   return "POWERON (normal acilis)";
  case ESP_RST_EXT:       return "EXT (harici reset)";
  case ESP_RST_SW:        return "SW (yazilim reset)";
  case ESP_RST_PANIC:     return "PANIC (crash!)";
  case ESP_RST_INT_WDT:   return "INT_WDT (kesme watchdog - kesmeler kapali kalmis!)";
  case ESP_RST_TASK_WDT:  return "TASK_WDT (task watchdog - DONMA yakalandi!)";
  case ESP_RST_WDT:       return "WDT (diger watchdog)";
  case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
  case ESP_RST_BROWNOUT:  return "BROWNOUT (besleme cokmesi - donanim!)";
  case ESP_RST_SDIO:      return "SDIO";
  default:                return "UNKNOWN";
  }
}

// ==================== FFT SES ANALIZI ====================
struct SnapAudioResult {
  float rms;
  float snapBandRatio;
  float lowBandRatio;
  float crest;
  float onset;          // rms / uyarlanabilir gurultu tabani
  bool valid;           // yeterli ornek okundu mu
  bool isSnapStrong;    // cok net siklatma -> hareket dogrulamasi gerekmez
  bool isSnapLike;      // normal esik (1. siklatma, hareketle birlikte)
  bool isSnapLikeLoose; // gevsek esik (2. siklatma -> daha hassas)
};

// Ortusmeli analiz icin kayan pencere. Her cagrida yalnizca AUDIO_HOP kadar
// YENI ornek okunur, pencerenin geri kalani onceki cagridan devralinir.
static float audioRing[FFT_SAMPLES];
static bool audioRingPrimed = false;
// Uyarlanabilir gurultu tabani. Yalnizca SAKIN ve MOTOR KAPALI karelerde
// guncellenir, boylece ne siklatmanin kendisi ne de motor gurultusu tabani
// yukari cekip sonraki siklatmalari koreltebilir.
static float noiseEma = 0;

SnapAudioResult analyzeSnapAudio() {
  SnapAudioResult result = {0, 0, 0, 0, 0, false, false, false, false};

  int32_t rawSamples[AUDIO_HOP];
  size_t bytesRead = 0;
  // i2s_read bu cagriyi dogal olarak ~8 ms'de bir dondurur (AUDIO_HOP /
  // SAMPLE_RATE); dongunun temposunu belirleyen sey budur.
  i2s_read(I2S_NUM_0, rawSamples, sizeof(rawSamples), &bytesRead,
           pdMS_TO_TICKS(60));
  int n = bytesRead / 4;
  if (n < AUDIO_HOP)
    return result;

  // Kayan pencere: eskiyi sola kaydir, yeni HOP orneklerini sona ekle.
  memmove(audioRing, audioRing + AUDIO_HOP,
          (FFT_SAMPLES - AUDIO_HOP) * sizeof(float));
  for (int i = 0; i < AUDIO_HOP; i++)
    audioRing[FFT_SAMPLES - AUDIO_HOP + i] = (float)(rawSamples[i] >> 14);

  if (!audioRingPrimed) {
    // Ilk cagri: pencerenin yarisi hala sifir, olcum yaniltici olur.
    audioRingPrimed = true;
    return result;
  }
  result.valid = true;

  // DC BILESENI CIKAR. INMP441 kayda deger bir DC ofset uretir. Ofset hem
  // RMS'i yapay olarak yukseltir (esikler anlamini yitirir) hem de tepe/RMS
  // oranini 1'e dogru ezerek crest testini tamamen ise yaramaz hale getirir
  // (eski kodda crest esigi bu yuzden 1.1 gibi hicbir sey elemeyen bir
  // degere dusurulmustu).
  float mean = 0;
  for (int i = 0; i < FFT_SAMPLES; i++)
    mean += audioRing[i];
  mean /= (float)FFT_SAMPLES;

  float sumSq = 0;
  float peak = 0;
  for (int i = 0; i < FFT_SAMPLES; i++) {
    float s = audioRing[i] - mean;
    vReal[i] = s;
    vImag[i] = 0;
    sumSq += s * s;
    float a = fabsf(s);
    if (a > peak)
      peak = a;
  }
  result.rms = sqrtf(sumSq / (float)FFT_SAMPLES);
  result.crest = (result.rms > 0.5f) ? (peak / result.rms) : 0;

  FFT.windowing(FFT_WIN_TYP_HAMMING, FFT_FORWARD);
  FFT.compute(FFT_FORWARD);
  FFT.complexToMagnitude();

  float freqResolution = (float)SAMPLE_RATE / FFT_SAMPLES;
  float totalEnergy = 0, snapBandEnergy = 0, lowBandEnergy = 0;

  for (int i = 1; i < FFT_SAMPLES / 2; i++) {
    float freq = i * freqResolution;
    float mag = vReal[i] * vReal[i];
    totalEnergy += mag;
    if (freq >= SNAP_FREQ_LOW && freq <= SNAP_FREQ_HIGH)
      snapBandEnergy += mag;
    if (freq >= LOW_FREQ_LOW && freq <= LOW_FREQ_HIGH)
      lowBandEnergy += mag;
  }
  if (totalEnergy > 0) {
    result.snapBandRatio = snapBandEnergy / totalEnergy;
    result.lowBandRatio = lowBandEnergy / totalEnergy;
  }

  // Motor calisirken (ve hemen sonrasinda) mikrofon sagir: ne karar ver ne de
  // gurultu tabanini guncelle.
  bool muted = ((long)(millis() - audioMuteUntil) < 0);

  // ONSET (ani darbe) OLCUSU. Eski "spike" testi bozuktu: sabit 50.0 esigi
  // yuzunden sessiz bir odada baseline hicbir zaman 50'yi gecmedigi icin test
  // KALICI OLARAK devre disi kaliyordu. Burada olcu tamamen goreceli:
  // mevcut RMS'in yavas gurultu tabanina orani.
  float floorRef = fmaxf(noiseEma, fmaxf(noiseFloor * 0.5f, 1.0f));
  result.onset = result.rms / floorRef;

  if (!muted) {
    // Taban yalnizca darbe OLMAYAN karelerde ilerler (hizli dusus, yavas
    // yukselis: ortam sessizlesince cabuk uyum saglar, gurultulenince temkinli).
    if (noiseEma <= 0)
      noiseEma = result.rms;
    else if (result.onset < 2.0f)
      noiseEma = noiseEma * 0.98f + result.rms * 0.02f;
    else if (result.rms < noiseEma)
      noiseEma = noiseEma * 0.80f + result.rms * 0.20f;
  }

  if (calibrated && !muted) {
    float crestRef = fmaxf(calSnapCrestAvg * 0.45f, 2.0f);

    bool ratioOK = result.snapBandRatio > (calSnapRatioAvg * 0.32f);
    bool notClap = result.lowBandRatio < 0.68f;
    bool rmsOK = result.rms > (calSnapRmsAvg * 0.18f);
    bool crestOK = result.crest > crestRef;
    bool onsetOK = result.onset > 3.0f;

    result.isSnapLike = ratioOK && notClap && rmsOK && crestOK && onsetOK;

    // GUCLU siklatma: her olcu genis farkla saglaniyor. Bu durumda hareket
    // dogrulamasi ARANMAZ. "Algilamama" sikayetlerinin buyuk kismi, sesi
    // gayet net olan bir siklatmanin MPU esigine takilmasindan kaynaklaniyordu.
    result.isSnapStrong = result.isSnapLike && result.onset > 6.0f &&
                          result.crest > (crestRef * 1.4f) &&
                          result.snapBandRatio > (calSnapRatioAvg * 0.55f);

    // GEVSEK ESIK: 2. siklatma icin. Cift siklatmada 2. darbe genelde daha
    // kisik ve hizli olur; 1. siklatma zaten hareketi ve onset'i dogruladigi
    // icin burada esikler dusurulur.
    result.isSnapLikeLoose = (result.snapBandRatio > calSnapRatioAvg * 0.18f) &&
                             (result.lowBandRatio < 0.80f) &&
                             (result.rms > calSnapRmsAvg * 0.09f) &&
                             (result.crest > crestRef * 0.65f) &&
                             (result.onset > 2.0f);
  }

  return result;
}

// ==================== MPU OKUMA ====================
struct MotionData {
  float gyroMag;
  float accMag;
  float gx, gy, gz;
  float ax, ay, az;
};

MotionData readMotion() {
  MotionData m = {0, 0, 0, 0, 0, 0, 0, 0};
  if (!mpuFound)
    return m;
  sensors_event_t a, g, temp;
  mpu.getEvent(&a, &g, &temp);
  m.gx = g.gyro.x;
  m.gy = g.gyro.y;
  m.gz = g.gyro.z;
  m.ax = a.acceleration.x;
  m.ay = a.acceleration.y;
  m.az = a.acceleration.z;
  m.gyroMag = fabsf(m.gx) + fabsf(m.gy) + fabsf(m.gz);
  m.accMag = sqrtf(m.ax * m.ax + m.ay * m.ay + m.az * m.az);
  return m;
}

// ==================== KALIBRASYON ====================
void runCalibration() {
  Serial.println("[CAL] Kalibrasyon basliyor...");
  flashLED(0, 0, 100, 500);

  // Gurultu zeminini olc. Ortusmeli pencere dolana kadar birkac kare atla.
  noiseEma = 0;
  audioRingPrimed = false;
  float nSum = 0;
  int nCount = 0;
  for (int i = 0; i < 40; i++) {
    wdtFeed();
    SnapAudioResult r = analyzeSnapAudio();
    if (r.valid && i >= 5) {
      nSum += r.rms;
      nCount++;
    }
  }
  noiseFloor = (nCount > 0) ? (nSum / (float)nCount) : 2.0f;
  if (noiseFloor < 1.0f)
    noiseFloor = 1.0f;
  noiseEma = noiseFloor;
  Serial.printf("[CAL] Gurultu zemini: %.1f\n", noiseFloor);
  Serial.println("[CAL] 10 kez parmak siklatin!");

  int snaps = 0;
  float sumRatio = 0, sumRms = 0, sumCrest = 0;
  unsigned long lastDet = 0;

  // GUVENLIK: Kalibrasyon en fazla bu kadar surer. Aksi halde mikrofon zayif
  // okuyorsa veya kimse siklatmiyorsa dongu SONSUZA kadar takilir kalir.
  const unsigned long CAL_TIMEOUT_MS = 30000;
  unsigned long calStart = millis();

  // Kalibrasyon TAMAMEN sese dayali (MPU/hareket sarti yok -> MPU arizasi
  // olsa bile kalibrasyon takilmaz).
  while (snaps < 10) {
    wdtFeed();
    if (millis() - calStart > CAL_TIMEOUT_MS) {
      Serial.printf("[CAL] ZAMAN ASIMI (%d/10 snap alindi) -> guvenli cikis\n",
                    snaps);
      break;
    }

    if ((millis() / 300) % 2 == 0)
      ledOn(15, 15, 0);
    else
      ledOff();

    SnapAudioResult audio = analyzeSnapAudio();
    if (!audio.valid)
      continue;

    // Kalibrasyonda da onset kullan: mutlak esik yerine "ortama gore ani
    // yukselis". Boylece sessiz ve gurultulu odada ayni sekilde calisir.
    bool soundOK = (audio.onset > 3.0f && audio.crest > 2.0f &&
                    audio.snapBandRatio > 0.05f);

    if (soundOK && (millis() - lastDet > 700)) {
      snaps++;
      sumRatio += audio.snapBandRatio;
      sumRms += audio.rms;
      sumCrest += audio.crest;
      lastDet = millis();
      Serial.printf("[CAL] Snap %d/10 rms=%.0f ratio=%.2f crest=%.1f onset=%.1f\n",
                    snaps, audio.rms, audio.snapBandRatio, audio.crest,
                    audio.onset);
      flashLED(0, 0, 150, 120);
    }
  }

  // GUVENLIK: gercekte alinan snap sayisina bol (her zaman /10 degil).
  if (snaps > 0) {
    calSnapRatioAvg = sumRatio / (float)snaps;
    calSnapRmsAvg = sumRms / (float)snaps;
    calSnapCrestAvg = sumCrest / (float)snaps;
  } else {
    // Hic snap alinamadi -> makul varsayilanlarla devam et (cihaz kilitlenmesin;
    // kullanici BOOT tusuyla sonradan yeniden kalibre edebilir).
    calSnapRatioAvg = 0.15f;
    calSnapRmsAvg = (noiseFloor > 0 ? noiseFloor * 5.0f : 200.0f);
    calSnapCrestAvg = 5.0f;
    Serial.println("[CAL] UYARI: Snap alinamadi, varsayilan degerler kullanildi.");
  }

  bool saveOk = nvsSaveCalibration();
  calibrated = true;
  Serial.printf("[CAL] NVS kayit sonucu: %s\n",
                saveOk ? "BASARILI" : "BASARISIZ!");
  Serial.printf("[CAL] Tamamlandi! ratio=%.2f rms=%.0f crest=%.1f noise=%.1f\n",
                calSnapRatioAvg, calSnapRmsAvg, calSnapCrestAvg, noiseFloor);

  flashLEDTimes(0, 80, 0, 400, 2);
  vibrateBlocking(150, 2);
}

// ==================== BLE TESHIS / SAGLIK ====================
// HCI kopma sebebi cozumleyici. NimBLE HCI hatalarini 0x200 ofsetiyle verebilir.
static const char *bleDisconnectReason(int r) {
  int c = (r > 0x200) ? (r - 0x200) : r;
  switch (c) {
  case 0x05: return "AUTH_FAIL (kimlik dogrulama)";
  case 0x06: return "PINKEY_MISSING (bond uyusmazligi -> eslesme silinmeli)";
  case 0x08: return "SUPERVISION_TIMEOUT (radyo/besleme kesintisi)";
  case 0x13: return "REMOTE_USER_TERM (host kapatti)";
  case 0x16: return "LOCAL_HOST_TERM (cihaz kapatti)";
  case 0x22: return "LL_RESPONSE_TIMEOUT";
  case 0x28: return "INSTANT_PASSED";
  case 0x3D: return "MIC_FAILURE (sifreleme dogrulamasi)";
  case 0x3E: return "CONN_FAILED_TO_ESTABLISH";
  default:   return "diger";
  }
}

// Bond uyusmazligi kopmanin ONARILAMAZ turudur: cihazdaki anahtar silinmis
// (ornegin NVS temizlendi) ama host'ta hala duruyorsa, host her seferinde eski
// anahtarla baglanmayi dener, sifreleme basarisiz olur ve baglanti dusuur.
// Kullanici acisindan bu tam olarak "bir kere baglandi, sonra bir daha asla"
// arizasidir ve cihazi kapatip acmak COZMEZ. Bu kodlari gorunce cihaz kendi
// tarafindaki bond'u temizler; host'taki eslesmenin de silinmesi gerekir.
static bool isBondMismatch(int r) {
  int c = (r > 0x200) ? (r - 0x200) : r;
  return (c == 0x05 || c == 0x06 || c == 0x3D);
}

// KADEMELI KURTARMA. "Koptu ve geri gelmiyor" arizasinin panzehiri.
//   0-60 sn : reklam kapandiysa yeniden baslat
//   60 sn   : reklami sifirdan kur (yigin "yayinliyorum" der ama radyoda
//             gorunmuyor olabilir; stop+start bunu temizler)
//   180 sn  : cihazi yeniden baslat. Sadece bu oturumda daha once BAGLANDIYSAK.
#define BLE_REARM_MS 60000
#define BLE_REBOOT_MS 180000

static void bleHealthCheck() {
  // Bond temizligi: KESINLIKLE callback icinde degil, burada. (esp_timer/RMT
  // dersinin ayni si: yigin callback'inin icinden yigina is yaptirilmaz.)
  if (bondWipePending) {
    bondWipePending = false;
    int n = NimBLEDevice::getNumBonds();
    NimBLEDevice::deleteAllBonds();
    Serial.printf("[BLE] Bond uyusmazligi -> %d eslesme silindi. "
                  "SIMDI HOST'TAN DA 'SnapSlide' cihazini KALDIRIN.\n", n);
    flashLEDTimes(80, 40, 0, 150, 3);
  }

  // Baglantidan ~1 sn sonra parametre guncellemesi iste.
  if (connParamsDueAt != 0 && millis() >= connParamsDueAt) {
    connParamsDueAt = 0;
    NimBLEServer *srv = NimBLEDevice::getServer();
    if (srv != nullptr && deviceConnected) {
      // 15-30ms aralik, latency 0, supervision timeout 5 sn (birim 10ms).
      // Varsayilan timeout cok kisa; motor akimi/parazit kaynakli kisa
      // kesintiler bu sure icinde toparlanirsa baglanti HIC dusmez.
      srv->updateConnParams(activeConnHandle, 12, 24, 0, 500);
      Serial.println("[BLE] Param istegi: 15-30ms aralik, 5sn supervision timeout");
    }
  }

  if (deviceConnected) {
    disconnectedSince = 0;
    return;
  }

  if (disconnectedSince == 0)
    disconnectedSince = millis();
  unsigned long down = millis() - disconnectedSince;

  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  if (adv == nullptr)
    return;

  if (!adv->isAdvertising()) {
    bool ok = NimBLEDevice::startAdvertising();
    Serial.printf("[BLE] Reklam durmus -> yeniden baslatildi: %s\n",
                  ok ? "OK" : "BASARISIZ");
    return;
  }

  static bool reArmed = false;
  if (down < BLE_REARM_MS)
    reArmed = false;

  if (!reArmed && down >= BLE_REARM_MS) {
    reArmed = true;
    NimBLEDevice::stopAdvertising();
    bool ok = NimBLEDevice::startAdvertising();
    Serial.printf("[BLE] %lu sn baglanti yok -> reklam sifirdan kuruldu: %s\n",
                  down / 1000, ok ? "OK" : "BASARISIZ");
    return;
  }

  if (everConnected && down >= BLE_REBOOT_MS) {
    Serial.printf("[BLE] %lu sn baglanti yok -> CIHAZ YENIDEN BASLATILIYOR\n",
                  down / 1000);
    Serial.flush();
    delay(50);
    esp_restart();
  }
}

// ==================== BLE CALLBACK'LERI ====================
class ServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *s, NimBLEConnInfo &c) override {
    deviceConnected = true;
    everConnected = true;
    disconnectedSince = 0;
    connectCount++;
    activeConnHandle = c.getConnHandle();
    linkEncrypted = c.isEncrypted();
    connParamsDueAt = millis() + 1000; // guncellemeyi bleHealthCheck yapacak
    Serial.printf("[BLE] Baglandi! (%s) bond=%s sifreli=%s\n",
                  c.getAddress().toString().c_str(),
                  c.isBonded() ? "evet" : "hayir",
                  c.isEncrypted() ? "evet" : "hayir");
  }

  // Sifreleme baglantidan sonra tamamlanir; HID bildirimleri ancak bu
  // noktadan sonra gecerlidir.
  void onAuthenticationComplete(NimBLEConnInfo &c) override {
    linkEncrypted = c.isEncrypted();
    Serial.printf("[BLE] Kimlik dogrulama tamam: bond=%s sifreli=%s\n",
                  c.isBonded() ? "evet" : "hayir",
                  c.isEncrypted() ? "evet" : "hayir");
  }

  void onDisconnect(NimBLEServer *s, NimBLEConnInfo &c, int r) override {
    deviceConnected = false;
    hidSubscribed = false;
    linkEncrypted = false;
    disconnectedSince = millis();
    connParamsDueAt = 0;
    Serial.printf("[BLE] Baglanti kesildi. Sebep: 0x%02X (%s)\n", r,
                  bleDisconnectReason(r));

    if (isBondMismatch(r))
      bondWipePending = true; // temizligi bleHealthCheck yapacak

    // Kopmanin motorla ilintili olup olmadigini tek satirda soyler. Bu satir
    // gorunuyorsa suc yazilimda degil beslemede/parazitte: bulk kapasite,
    // motor uclarina seramik, flyback diyot.
    unsigned long sinceVib = millis() - lastVibrateMs;
    if (lastVibrateMs != 0 && sinceVib < 1500) {
      Serial.printf("[BLE] >>> DIKKAT: kopma, motor calistiktan %lu ms SONRA "
                    "oldu -> besleme/parazit suphesi (bkz. docs/DONANIM.md)\n",
                    sinceVib);
    }
    // NOT: Burada startAdvertising() CAGIRMIYORUZ. advertiseOnDisconnect(true)
    // zaten yigin teardown'u bitince yeniden baslatiyor; callback'in icinden
    // ikinci kez cagirmak yaris olusturup reklamin hic baslamamasina yol
    // acabiliyor. Yedek kurtarma noktasi bleHealthCheck().
  }
};

// Host'un input report'a abone olup olmadigini takip eder. "Bagli gorunuyor
// ama tuslar gitmiyor" arizasinin teshisi tam olarak budur.
class InputCB : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic *c, NimBLEConnInfo &info,
                   uint16_t subValue) override {
    hidSubscribed = (subValue > 0);
    Serial.printf("[HID] Host abonelik durumu: %s (0x%04X)\n",
                  hidSubscribed ? "ACIK" : "KAPALI", subValue);
  }
};

// ==================== TUS GONDERME ====================
bool sendKeyPress(uint8_t keyCode) {
  if (!deviceConnected || inputKeyboard == nullptr)
    return false;

  uint8_t report[8] = {0};
  report[2] = keyCode;
  inputKeyboard->setValue(report, 8);
  // notify() bool doner - eskiden donus degeri hic kontrol edilmiyordu, bu
  // yuzden "gonderdim" denip aslinda hicbir sey gitmemis olabiliyordu.
  // Bonded bir host'a yeniden baglanildiginda CCCD host tarafinda saklidir ve
  // onSubscribe tekrar tetiklenmeyebilir; bu yuzden hidSubscribed'a BAKIP
  // GONDERIMI ENGELLEMIYORUZ, sadece teshis icin logluyoruz.
  bool ok = inputKeyboard->notify();
  if (!ok) {
    vTaskDelay(pdMS_TO_TICKS(20));
    ok = inputKeyboard->notify(); // tek tekrar
  }

  vTaskDelay(pdMS_TO_TICKS(25));
  memset(report, 0, 8);
  inputKeyboard->setValue(report, 8);
  inputKeyboard->notify(); // tus birakma

  if (!ok) {
    Serial.printf("[HID] UYARI: bildirim gonderilemedi (abone=%s sifreli=%s). "
                  "Host'ta eslesmeyi kaldirip yeniden eslestirin.\n",
                  hidSubscribed ? "evet" : "hayir",
                  linkEncrypted ? "evet" : "hayir");
  }
  return ok;
}

// ==================== BOOT TUSU ====================
// Basili tutma suresine gore karar RELEASE aninda verilir; boylece tek bir
// tusla iki farkli islev sunulabilir ve kullanici LED'den ne olacagini gorur.
#define BTN_RECAL_MS 1500
#define BTN_BONDWIPE_MS 5000

static void handleBootButton() {
  static unsigned long pressedAt = 0;
  static bool wasDown = false;

  bool down = (digitalRead(BOOT_BUTTON) == LOW);

  if (down) {
    if (!wasDown) {
      wasDown = true;
      pressedAt = millis();
    }
    unsigned long held = millis() - pressedAt;
    // Canli geri bildirim: mavi = birakirsan kalibrasyon, kirmizi = bond silme
    if (held >= BTN_BONDWIPE_MS)
      ledOn(90, 0, 0);
    else if (held >= BTN_RECAL_MS)
      ledOn(0, 0, 90);
    return;
  }

  if (!wasDown)
    return;
  wasDown = false;
  unsigned long held = millis() - pressedAt;
  ledOff();

  if (held >= BTN_BONDWIPE_MS) {
    int n = NimBLEDevice::getNumBonds();
    NimBLEDevice::deleteAllBonds();
    Serial.printf("[BTN] %lu ms basili -> %d BLE eslesmesi silindi. "
                  "Host'tan da 'SnapSlide' cihazini kaldirin!\n", held, n);
    flashLEDTimes(90, 0, 0, 200, 3);
    Serial.flush();
    delay(100);
    esp_restart();
  } else if (held >= BTN_RECAL_MS) {
    Serial.printf("[BTN] %lu ms basili -> yeniden kalibrasyon\n", held);
    runCalibration();
  }
}

// ==================== SENSOR GOREVI ====================
void sensorTask(void *p) {
  unsigned long lastSnap = 0, lastDbg = 0, lastBatt = 0;
  wdtSubscribe("sensorTask");
  while (1) {
    wdtFeed();
    motorWatchdog(); // KATMAN 2: motor takili kalamaz

    if (millis() - lastBatt > 60000) {
      lastBatt = millis();
      if (hid != nullptr)
        hid->setBatteryLevel(readBatteryPercent(), true);
    }

    handleBootButton();

    if (!calibrated) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    // Hareket okumasi. I2C artik 400 kHz -> okuma ~0.5 ms, her ses karesinde
    // (8 ms) yapilabiliyor. Eskiden ~16 ms'de bir orneklendigi icin hizli bir
    // bilek hareketi iki okuma arasina dusup tamamen kacabiliyordu.
    MotionData mot = readMotion();
    // Gyro VEYA ivme darbesi. Siklatirken bilek yalnizca donmez, keskin bir
    // ivme darbesi de uretir; tek basina gyro esigi cok fazla gercek
    // siklatmayi eliyordu.
    if (mot.gyroMag > MOTION_GYRO_TH ||
        fabsf(mot.accMag - 9.81f) > MOTION_ACC_TH)
      lastMotionTime = millis();

    SnapAudioResult audio = analyzeSnapAudio();
    if (!audio.valid) {
      continue; // i2s zaman asimi; dongu temposunu i2s_read belirliyor
    }

    if (millis() - lastDbg > 3000) {
      lastDbg = millis();
      NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
      bool advOn = (adv != nullptr) && adv->isAdvertising();
      Serial.printf("[SEN] rms=%.0f floor=%.0f onset=%.1f ratio=%.2f low=%.2f "
                    "crest=%.1f gyro=%.2f | snap=%s guclu=%s | ble=%s abone=%s "
                    "adv=%s motorKurt=%lu\n",
                    audio.rms, noiseEma, audio.onset, audio.snapBandRatio,
                    audio.lowBandRatio, audio.crest, mot.gyroMag,
                    audio.isSnapLike ? "EVET" : "hayir",
                    audio.isSnapStrong ? "EVET" : "hayir",
                    deviceConnected ? "BAGLI" : "yok",
                    hidSubscribed ? "evet" : "hayir",
                    advOn ? "acik" : "KAPALI",
                    (unsigned long)motorSwRescues);
    }

    bool isActionCooldown = (millis() - lastActionTime < ACTION_COOLDOWN_MS);

    // GUVENLIK AGI: MPU bulunamazsa hareket sarti otomatik saglanmis sayilir
    // -> snap yine calisir (tamamen sese dayali fallback).
    bool motionOK = !mpuFound || ((millis() - lastMotionTime) < MOTION_WINDOW_MS);

    // Ilk snap: ya SES TEK BASINA cok net (isSnapStrong), ya da normal esik +
    // hareket dogrulamasi. Bu "VEYA" yapisi kacirmalari azaltirken, sinirda
    // olan seslerde hareket sartini korudugu icin yanlis pozitifleri artirmaz.
    bool isFirstSnap = (snapCount == 0) &&
                       (audio.isSnapStrong || (audio.isSnapLike && motionOK));
    bool isSecondSnap = (snapCount == 1) && audio.isSnapLikeLoose;

    if (!isActionCooldown) {
      if (isFirstSnap && (millis() - lastSnap > SNAP_COOLDOWN_MS)) {
        snapCount = 1;
        snapWindowStart = millis();
        lastSnap = millis();
        Serial.printf("[SNAP!] 1. Siklatma (onset=%.1f crest=%.1f %s)\n",
                      audio.onset, audio.crest,
                      audio.isSnapStrong ? "GUCLU" : "hareket+ses");
      } else if (isSecondSnap && (millis() - lastSnap > SECOND_SNAP_MIN_GAP_MS)) {
        snapCount = 2;
        lastSnap = millis();
        Serial.printf("[SNAP!] 2. Siklatma (onset=%.1f)\n", audio.onset);
      }
    }

    // Karar mekanizmasi
    if (snapCount == 2) {
      portENTER_CRITICAL(&snapMux);
      finalSnapResult = SNAP_DOUBLE;
      portEXIT_CRITICAL(&snapMux);
      snapCount = 0;
    } else if (snapCount == 1 &&
               (millis() - snapWindowStart > DOUBLE_SNAP_WINDOW)) {
      portENTER_CRITICAL(&snapMux);
      finalSnapResult = SNAP_SINGLE;
      portEXIT_CRITICAL(&snapMux);
      snapCount = 0;
    }
  }
}

// ==================== SETUP ====================
void setup() {
  // EN ONCE motoru guvene al. Bu satirdan onceki her is, GPIO7'nin yuksek
  // empedansli GIRIS olarak bekledigi suredir.
  pinMode(MOTOR_PIN, OUTPUT);
  digitalWrite(MOTOR_PIN, LOW);
  safetyTimersInit(); // motor kesme ISR'i ilk aktivasyondan ONCE hazir olmali

  Serial.begin(115200);
  delay(2000);
  Serial.println("\n\n===== SnapSlide =====");

  // Arizanin turunu tartismaya son verir: DONMA mi (TASK_WDT/INT_WDT),
  // besleme cokmesi mi (BROWNOUT), yoksa crash mi (PANIC).
  esp_reset_reason_t rr = esp_reset_reason();
  Serial.printf("[BOOT] Reset sebebi: %d -> %s\n", (int)rr, resetReasonName(rr));
  if (rr == ESP_RST_BROWNOUT) {
    Serial.println("[BOOT] >>> BROWNOUT: besleme motor darbesinde cokuyor. "
                   "Bulk kapasite / flyback diyot sart (docs/DONANIM.md).");
  }

  pinMode(BOOT_BUTTON, INPUT_PULLUP);
  ledOff(); // baslangicta LED kapali oldugundan emin ol

  // NVS
  esp_err_t err = nvs_flash_init();
  Serial.printf("[NVS] nvs_flash_init sonuc: 0x%X (%s)\n", err,
                esp_err_to_name(err));
  if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
      err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    Serial.println("[NVS] !!! DIKKAT: NVS bolumu siliniyor - kayitli "
                   "kalibrasyon VE BLE eslesmeleri kaybolacak. Baglanti "
                   "kurulamazsa host'tan da eslesmeyi kaldirin !!!");
    nvs_flash_erase();
    err = nvs_flash_init();
  }
  if (err == ESP_OK)
    Serial.println("[OK] NVS Aktif.");

  // MPU6050
  Wire.end();
  delay(200);
  // 400 kHz: hareket okumasi ses karesi basina yapilabilsin diye (100 kHz'de
  // tek okuma ~2 ms suruyor ve 8 ms'lik butceyi zorluyordu).
  Wire.begin(SDA_PIN, SCL_PIN, 400000);
  // I2C zaman asimini kisalt: baglanti anlik koparsa dongu 1sn degil
  // sadece ~25ms takilir (gevsek MPU baglantisina karsi donmayi engeller).
  Wire.setTimeOut(25);
  delay(200);

  if (mpu.begin(0x68, &Wire)) {
    mpuFound = true;
    mpu.setAccelerometerRange(MPU6050_RANGE_4_G);
    mpu.setGyroRange(MPU6050_RANGE_250_DEG);
    mpu.setFilterBandwidth(MPU6050_BAND_260_HZ);
    Serial.println("[OK] MPU6050 Hazir.");
  } else {
    Serial.println("[UYARI] MPU6050 bulunamadi! (sadece ses moduna dusuluyor)");
  }

  // I2S Mikrofon
  i2s_config_t cfg = {.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
                      .sample_rate = SAMPLE_RATE,
                      .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
                      .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
                      .communication_format = I2S_COMM_FORMAT_STAND_I2S,
                      .dma_buf_count = 8,
                      .dma_buf_len = 128};
  i2s_pin_config_t pins = {.mck_io_num = -1,
                           .bck_io_num = I2S_SCK,
                           .ws_io_num = I2S_WS,
                           .data_out_num = -1,
                           .data_in_num = I2S_SD};
  i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pins);
  i2s_start(I2S_NUM_0);
  Serial.println("[OK] I2S Mikrofon Aktif.");

  // Kalibrasyon yukle
  if (nvsLoadCalibration()) {
    calibrated = true;
    noiseEma = noiseFloor;
    Serial.printf("[OK] Kalibrasyon yuklendi: ratio=%.2f rms=%.0f crest=%.1f\n",
                  calSnapRatioAvg, calSnapRmsAvg, calSnapCrestAvg);
  }

  // BLE Keyboard
  NimBLEDevice::init("SnapSlide");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  NimBLEDevice::setSecurityAuth(true, false, true); // bonding=evet, MITM=hayir, SC=evet
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  Serial.printf("[BLE] Kayitli eslesme sayisi: %d\n", NimBLEDevice::getNumBonds());

  NimBLEServer *srv = NimBLEDevice::createServer();
  srv->setCallbacks(new ServerCB());
  srv->advertiseOnDisconnect(true);
  hid = new NimBLEHIDDevice(srv);
  inputKeyboard = hid->getInputReport(1);
  inputKeyboard->setCallbacks(new InputCB());
  hid->setManufacturer("SnapSlide");
  hid->setHidInfo(0x00, 0x01);
  // PnP ID: eskiden HIC AYARLANMIYORDU. Windows, HID over GATT cihazlarini
  // surucuye baglarken Device Information servisindeki PnP ID'yi okur; bos
  // kalirsa cihaz eslesse bile klavye olarak enumerate EDILMEYEBILIR ("bagli
  // gorunuyor ama tuslar calismiyor" arizasinin klasik sebebi).
  // 0x02 = USB Implementers Forum, 0x303A = Espressif'in resmi USB VID'i.
  hid->setPnp(0x02, 0x303A, 0x8000, 0x0110);
  hid->setReportMap((uint8_t *)hidReportDescriptor,
                    sizeof(hidReportDescriptor));
  hid->setBatteryLevel(readBatteryPercent());

  // SIRA ONEMLI: once sunucuyu (ve servisleri) baslat, SONRA reklam ver.
  // Aksi halde o pencerede baglanan bir host eksik GATT tablosu goruyor ve
  // baglantiyi dusurebiliyor.
  srv->start();

  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  adv->setAppearance(0x03C1); // HID Keyboard
  adv->addServiceUUID(hid->getHidService()->getUUID());
  // Hizli reklam araligi (birim 0.625ms): 20ms-40ms. Kopma sonrasi host'un
  // cihazi tarama penceresinde yakalama sansini artirir.
  adv->setMinInterval(0x20);
  adv->setMaxInterval(0x40);
  adv->setName("SnapSlide");
  adv->enableScanResponse(true);
  bool advOk = adv->start();
  Serial.printf("[OK] BLE Klavye Aktif: SnapSlide (reklam: %s)\n",
                advOk ? "BASLADI" : "BASLAYAMADI!");

  // Kalibrasyon
  if (!calibrated)
    runCalibration();

  // Sensor gorevi
  xTaskCreatePinnedToCore(sensorTask, "Sensor", 8192, NULL, 1, NULL, 1);

  // loop() task'ini TWDT'ye EN SON abone et: setup() icindeki delay(2000) ve
  // 30 sn surebilen kalibrasyon bittikten sonra, yoksa bosuna reboot atar.
  wdtSubscribe("loopTask");
  Serial.println("===== SISTEM HAZIR =====");
  Serial.println("BOOT tusu: 1.5-5 sn = yeniden kalibrasyon | >5 sn = "
                 "BLE eslesmelerini sil\n");
}

// ==================== LOOP ====================
void loop() {
  wdtFeed(); // loop() core 1'de; TWDT'yi burada beslemezsek 5sn'de reboot olur
  vibrateService(); // bloklamayan titresim motoru
  motorWatchdog();  // KATMAN 2

  // BLE saglik kontrolu artik saniyede bir (eskiden 3 sn'lik hata ayiklama
  // yazdirmasina bagliydi, yani kurtarma da 3 sn'de bir denenebiliyordu).
  static unsigned long lastHealth = 0;
  if (millis() - lastHealth > 1000) {
    lastHealth = millis();
    bleHealthCheck();
  }

  // 1) COOLDOWN: komut gonderildikten sonra korluk suresi
  if (millis() - lastActionTime < ACTION_COOLDOWN_MS) {
    portENTER_CRITICAL(&snapMux);
    finalSnapResult = SNAP_NONE;
    portEXIT_CRITICAL(&snapMux);
    // LED'i aksiyon sonrasi kapat (bloklamadan)
    if (millis() - lastActionTime > 250)
      ledOff();
    delay(5);
    return;
  }
  ledOff();

  // 2) SNAP SONUCUNU AL
  SnapResult localResult = SNAP_NONE;
  portENTER_CRITICAL(&snapMux);
  if (finalSnapResult != SNAP_NONE) {
    localResult = finalSnapResult;
    finalSnapResult = SNAP_NONE;
  }
  portEXIT_CRITICAL(&snapMux);

  // 3) AKSIYONU UYGULA
  // SIRA ONEMLI: once BLE bildirimi, sonra motor. Motorun akim darbesi ve
  // uretttigi parazit, ayni anda yapilan radyo iletimini bozabiliyor
  // (supervision timeout / 0x08 kopmalari). sendKeyPress icindeki 25 ms
  // bekleme, bildirimin radyodan cikmasi icin gereken ayrimi saglar.
  if (localResult == SNAP_SINGLE) {
    Serial.println("[AKSIYON] Tek siklatma -> SAG OK");
    lastActionTime = millis();
    if (deviceConnected)
      sendKeyPress(KEY_RIGHT_ARROW);
    else
      Serial.println("[AKSIYON] (BLE bagli degil - tus gonderilmedi)");
    ledOn(0, 40, 0);
    vibrateAsync(120, 1);
  } else if (localResult == SNAP_DOUBLE) {
    Serial.println("[AKSIYON] Cift siklatma -> SOL OK");
    lastActionTime = millis();
    if (deviceConnected)
      sendKeyPress(KEY_LEFT_ARROW);
    else
      Serial.println("[AKSIYON] (BLE bagli degil - tus gonderilmedi)");
    ledOn(60, 0, 60);
    vibrateAsync(80, 2);
  }

  delay(5);
}
