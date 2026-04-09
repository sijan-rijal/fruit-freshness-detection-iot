#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <esp_camera.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_AS7341.h>
#include <math.h>

#include <esp_sleep.h>


// IOT fruit freshness detection - case study for prototype development and testing
//this code reads banana condition with AS7341, reads temperature and humidity
//with HDC1080, performs computation, captures one camera image, uploads JSON and JPEG separately,
//shows result on OLED, then goes to deep sleep.

// maturity outputs used here
// - Green / Unripe
// - Proceeding from Unripe to Ripe (buffer zone added later)
// - Ripe
// - Proceeding from Ripe to Overripe (buffer zone added later)
// - Overripe
// - Spoiled
//
// - temperature and humidity are used for storage interpretation
// - they do not directly change spectral maturity stage
// - storage result is still useful for reporting and dashboard display


// logic checking order
// 1. no data / saturated reading
// 2. spoiled
// 3. weak reading

// 4. unripe
// 5. unripe -> ripe transition
// 6. ripe
// 7. ripe -> overripe transition
// 8. overripe
// 9. fallback by strongest visible band




// part 0 - camera pin file
//----------------------------------------------------------------------------------------------------------
//This part is responsible for loading the camera pin definitions from camera_pins.h
//This sketch file needs that so the ESP32 camera can be initialized with the correct hardware pin mapping

#if __has_include("camera_pins.h")
  #include "camera_pins.h"
#else
  #error "camera_pins.h is missing. Put camera_pins.h in the same sketch folder."
#endif


//part 1 - hardware & network configuration
//----------------------------------------------------------------------------------------------------------
// all settings are grouped here so tuning later will be easier
struct NetworkConfig {
  const char* ssid;
  const char* password;
  const char* readingUrl;
  const char* imageUrl;
  const char* deviceId;
};


//local server
const NetworkConfig NET_CFG = {
  "WIFI_SSID",
  "WIFI_PASSWORD",
  "http://192.168.178.90:5002/api/reading",
  "http://192.168.178.90:5002/api/upload_image",
  "banana-monitor-01"
};



// I2C pins
#define PIN_QWIIC_SDA 2
#define PIN_QWIIC_SCL 1

// OLED
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1

// HDC1080
#define HDC1080_ADDR 0x40


// part 2 - timing configuration
//----------------------------------------------------------------------------------------------------------
// this will control wake -> upload -> wait -> sleep cycle

struct TimingConfig {
  uint64_t deepSleepUs;
  uint32_t postUploadAwakeMs;
  int finalCountdownSec;
};

const TimingConfig TIME_CFG = {
  3ULL * 60ULL * 60ULL * 1000000ULL,    // for 3 hours sleep
  90UL * 1000UL,                        // for 90 seconds awake after upload
  10                                    //countdown to sleep
};


// part 3 - light control
//----------------------------------------------------------------------------------------------------------
// it is to manage As7341 on baord led used for spectral reading measurement. 
//it helps to define wheter theled is used, how strong it is and for how logn it will stay on.
//this is main tuning areas if the reading is reported as overexposed or underexposed

struct SensorLedConfig {
  bool useSensorLed;
  uint16_t ledCurrentMa;
  uint16_t ledSettleMs;
  uint16_t ledExtraOnMs;
};

const SensorLedConfig LED_CFG = {
  true,
  8,
  60,
  60
};



// part 4 - AS7341 exposure settings
//----------------------------------------------------------------------------------------------------------
// if light is still too bright after LED tuning, we will ahve to lower these slightly

struct As7341ReadConfig {
  uint8_t atime;
  uint16_t astep;
  as7341_gain_t gain;
};

const As7341ReadConfig AS7341_CFG = {
  60,
  599,
  AS7341_GAIN_64X
};


// part 5 - camera settings
//----------------------------------------------------------------------------------------------------------
//configures the ESP32 camera for lightweight image capture. 
//since the image is mainly for visual reference and dashboard display, the resolution and JPEG size are kept small so upload stays more reliable.

struct CameraConfigData {
  bool sendCameraImage;
  framesize_t frameSize;
  int jpegQuality;
  size_t maxJpegBytes;
  int flushFrameCount;
  int flushWaitMs;
};

const CameraConfigData CAM_CFG = {
  true,
  FRAMESIZE_QQVGA,
  18,
  22000,
  2,
  70
};


// part 6 - read stability
//----------------------------------------------------------------------------------------------------------
//defines how sensor readings are stabilized. 
//Also few dummy reads are used first to settle the sensors, and then several real reads are averaged to reduce noise and random variation.
// dummy reads settle the sensors, then real reads are averaged

struct StabilityConfig {
  int hdcDummyReads;
  int hdcRealReads;
  int hdcReadGapMs;

  int asDummyReads;
  int asRealReads;
  int asReadGapMs;
};

const StabilityConfig STAB_CFG = {
  1, 3, 25,
  1, 3, 30
};


// part 7 - upload settings
//----------------------------------------------------------------------------------------------------------
// responsible to control how Wifi and uploads behave including timeouts and retry counts for json and image uploads. 
//It will help prevent the board from staying awake too long during network problems.

struct UploadConfig {
  int wifiTimeoutMs;
  int jsonTimeoutMs;
  int imageTimeoutMs;
  int jsonRetries;
  int imageRetries;
};

const UploadConfig UP_CFG = {
  20000,
  18000,
  22000,
  2,
  2
};



// part 8 - human readable signal labels
//----------------------------------------------------------------------------------------------------------
//to define simple signal strength categories for sensor values such as low, moderate, strong, or sat

struct SignalLevelThresholds {
  uint16_t extremelyLowMax;
  uint16_t lowMax;
  uint16_t moderateMax;
  uint16_t strongMax;
  uint16_t veryStrongMax;
  uint16_t nearSaturationMax;
};

const SignalLevelThresholds SIG_CFG = {
  500,
  2000,
  8000,
  20000,
  50000,
  62000
};


// part 9 - reading quality thresholds
//----------------------------------------------------------------------------------------------------------
//this part checks whether the spectral reading is usable before maturity classification begins. 
//it defines when a scan is too dark,, when it is considered dark enough for spoiled detection, and when it is to bright or saturated.
// clearMinValid = too dark below this for normal stage decision
// clearSpoiledMax = darker range where spoiled logic can still matter
// clearSatWarn = too bright above this

struct ReadingQualityThresholds {
  uint16_t clearMinValid;
  uint16_t clearSpoiledMax;
  uint16_t clearSatWarn;
};

const ReadingQualityThresholds QUALITY_CFG = {
  600,
  2500,
  62000
};




// part 10 - banana stage thresholds
//----------------------------------------------------------------------------------------------------------
// this is the main maturity block for stages
//
// hard stages
// spoiled
// unripe
// ripe
// overripe
//
// buffer bands
// unripe to ripe
// ripe to overripe
// transition bands to reduce sudden jumps near the border

struct BananaStageThresholds {
  // spoiled
  float spoiledNirToClearMin;
  float spoiledF8OverF6Min;
  float spoiledF7OverF5Min;

  // overripe
  float overripeF7OverF5Min;
  float overripeF8OverF6Min;
  uint16_t overripeClearMin;

  // ripe
  float ripeF7OverF5Min;
  float ripeF7OverF5Max;
  float ripeF8OverF6Max;
  uint16_t ripeClearMin;

  // unripe
  float unripeF5OverF7Min;
  float unripeF4OverF8Min;
};

const BananaStageThresholds STAGE_CFG = {
  // spoiled
  0.30f,
  1.00f,
  1.30f,

  // overripe
  1.28f,
  0.95f,
  900,

  // ripe
  0.80f,
  1.15f,
  0.85f,
  1600,

  // unripe
  1.20f,
  1.10f
};

struct BananaTransitionThresholds {
  // unripe to ripe
  uint16_t unripeToRipeClearMin;
  float unripeToRipeF5OverF7Min;
  float unripeToRipeF5OverF7Max;
  float unripeToRipeF4OverF8Min;
  float unripeToRipeF4OverF8Max;

  // ripe to overripe
  uint16_t ripeToOverripeClearMin;
  float ripeToOverripeF7OverF5Min;
  float ripeToOverripeF7OverF5Max;
  float ripeToOverripeF8OverF6Min;
  float ripeToOverripeF8OverF6Max;
};

const BananaTransitionThresholds TRANSITION_CFG = {
  // unripe to ripe
  1200,
  1.00f,
  1.20f,
  1.00f,
  1.10f,

  // ripe to overripe
  1200,
  1.15f,
  1.28f,
  0.85f,
  0.95f
};



// part 11 - storage thresholds
//----------------------------------------------------------------------------------------------------------
// this part is for temp and humidity use
// HDC1080 is used to label storage as Good, Warm, Cold,  Dry, Humid or Poor
// but this does not replace maturity stage, it just gives storage context

struct StorageThresholds {
  float tempTooCold;
  float tempTooWarm;
  float rhTooDry;
  float rhTooHumid;
};

const StorageThresholds STORAGE_CFG = {
  13.0f,
  20.0f,
  85.0f,
  95.0f
};


// part 12 - global objects
//----------------------------------------------------------------------------------------------------------
//to create the main hardware objects and global state used across the sketch, such as the OLED display object, 
//the AS7341 sensor object, status flags, upload result flags, and the boot counter stored across deep sleep

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Adafruit_AS7341 as7341;

bool oledOk = false;
bool hdcOk  = false;
bool asOk   = false;
bool camOk  = false;

bool lastJsonUploadOk  = false;
int  lastJsonHttpCode  = 0;
bool lastImageUploadOk = false;
int  lastImageHttpCode = 0;

RTC_DATA_ATTR uint32_t BOOT_COUNTER = 0;


// part 13 - data structures
//----------------------------------------------------------------------------------------------------------
//this part defines the main data containers used in the program. 
//it will store spectral values, temperature and humidity, derived ratios, camera capture details. the final interpreted banana result that is shown and uploaded

struct SpectralData {
  float temperatureC = NAN;
  float humidityRH   = NAN;

  uint16_t f1_415 = 0;
  uint16_t f2_445 = 0;
  uint16_t f3_480 = 0;
  uint16_t f4_515 = 0;
  uint16_t f5_555 = 0;
  uint16_t f6_590 = 0;
  uint16_t f7_630 = 0;
  uint16_t f8_680 = 0;
  uint16_t clearCh = 0;
  uint16_t nirCh   = 0;

  float nirToClear = 0.0f;
  float f5ToF7     = 0.0f;
  float f7ToF5     = 0.0f;
  float f4ToF8     = 0.0f;
  float f8ToF6     = 0.0f;
};

struct CameraCapture {
  bool ok = false;
  bool uploaded = false;
  size_t jpegBytes = 0;
  int width = 0;
  int height = 0;
  String mime = "image/jpeg";
  camera_fb_t* fb = nullptr;
  uint32_t sampleId = 0;
};

struct BananaResult {
  String stage;
  String stageDetail;
  String stageAdvice;

  String storage;
  String storageAdvice;

  String readingQuality;
  String readingQualityAdvice;

  String spectralStory;
  String strongestChannel;
  String strongestVisibleBand;

  String eatRecommendation;
  String finalRecommendation;

  int spoilageScore = 0;
};

SpectralData data;
CameraCapture latestImage;
BananaResult latestResult;


// part 14 - small helpers
//----------------------------------------------------------------------------------------------------------
//shared utility functions used across the sketch, such as OLED text display, ratio calculation, JSON escaping, signal labeling,
//reading quality interpretation, storage labeling, strongest band detection, spectral summary text, and reset helpers only to make the main code cleaner.

void showMessage(const String &a, const String &b = "", const String &c = "", const String &d = "") {
  if (!oledOk) return;

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0, 0);
  display.println(a);
  if (b.length()) display.println(b);
  if (c.length()) display.println(c);
  if (d.length()) display.println(d);
  display.display();
}

float safeRatio(uint16_t numerator, uint16_t denominator) {
  return (float)numerator / (float)(denominator + 1);
}

String jsonEscape(const String &input) {
  String out;
  out.reserve(input.length() + 16);

  for (size_t i = 0; i < input.length(); i++) {
    char c = input[i];
    if (c == '"') out += "\\\"";
    else if (c == '\\') out += "\\\\";
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else out += c;
  }
  return out;
}

String getSignalLevelLabel(uint16_t value) {
  if (value == 0) return "No signal";
  if (value <= SIG_CFG.extremelyLowMax) return "Extremely low";
  if (value <= SIG_CFG.lowMax) return "Low";
  if (value <= SIG_CFG.moderateMax) return "Moderate";
  if (value <= SIG_CFG.strongMax) return "Strong";
  if (value <= SIG_CFG.veryStrongMax) return "Very strong";
  if (value <= SIG_CFG.nearSaturationMax) return "Near saturation";
  return "Saturated";
}

String getReadingQuality(uint16_t clearValue) {
  if (clearValue == 0) return "No AS7341 data";
  if (clearValue < QUALITY_CFG.clearMinValid) return "Too dark";
  if (clearValue >= QUALITY_CFG.clearSatWarn) return "Too bright / saturated";
  return "Usable";
}

String getReadingQualityAdvice(const String &quality) {
  if (quality == "Usable") return "Spectral reading is usable.";
  if (quality == "Too dark") return "Reading is too dark. Increase light or move sensor closer.";
  if (quality == "Too bright / saturated") return "Reading is too bright. Reduce LED current, gain, or distance.";
  if (quality == "No AS7341 data") return "Sensor returned no usable spectral data.";
  return "Check reading quality.";
}

String getStorageLabel(float t, float h) {
  if (isnan(t) || isnan(h)) return "Unknown";
  if (t > STORAGE_CFG.tempTooWarm && h < STORAGE_CFG.rhTooDry) return "Poor";
  if (t > STORAGE_CFG.tempTooWarm) return "Warm";
  if (t < STORAGE_CFG.tempTooCold) return "Cold";
  if (h < STORAGE_CFG.rhTooDry) return "Dry";
  if (h > STORAGE_CFG.rhTooHumid) return "Humid";
  return "Good";
}

String getStorageAdvice(const String &storage) {
  if (storage == "Good") return "Storage looks acceptable right now.";
  if (storage == "Warm") return "Too warm. Banana may ripen faster.";
  if (storage == "Cold") return "Too cold. Peel can darken unnaturally.";
  if (storage == "Dry") return "Air is too dry. Fruit may dehydrate faster.";
  if (storage == "Humid") return "Air is too humid. Watch for moisture-related spoilage.";
  if (storage == "Poor") return "Temperature and humidity are both unfavorable.";
  return "No storage recommendation available.";
}

String getStrongestChannelName(const SpectralData &d) {
  uint16_t maxVal = d.f1_415;
  String name = "F1";
  if (d.f2_445 > maxVal) { maxVal = d.f2_445; name = "F2"; }
  if (d.f3_480 > maxVal) { maxVal = d.f3_480; name = "F3"; }
  if (d.f4_515 > maxVal) { maxVal = d.f4_515; name = "F4"; }
  if (d.f5_555 > maxVal) { maxVal = d.f5_555; name = "F5"; }
  if (d.f6_590 > maxVal) { maxVal = d.f6_590; name = "F6"; }
  if (d.f7_630 > maxVal) { maxVal = d.f7_630; name = "F7"; }
  if (d.f8_680 > maxVal) { maxVal = d.f8_680; name = "F8"; }
  if (d.clearCh > maxVal) { maxVal = d.clearCh; name = "Clear"; }
  if (d.nirCh > maxVal)   { maxVal = d.nirCh;   name = "NIR"; }
  return name;
}

String getStrongestVisibleBandName(const SpectralData &d) {
  uint16_t maxVal = d.f1_415;
  String name = "F1";
  if (d.f2_445 > maxVal) { maxVal = d.f2_445; name = "F2"; }
  if (d.f3_480 > maxVal) { maxVal = d.f3_480; name = "F3"; }
  if (d.f4_515 > maxVal) { maxVal = d.f4_515; name = "F4"; }
  if (d.f5_555 > maxVal) { maxVal = d.f5_555; name = "F5"; }
  if (d.f6_590 > maxVal) { maxVal = d.f6_590; name = "F6"; }
  if (d.f7_630 > maxVal) { maxVal = d.f7_630; name = "F7"; }
  if (d.f8_680 > maxVal) { maxVal = d.f8_680; name = "F8"; }
  return name;
}

String getSpectralStory(const SpectralData &d) {
  if (d.clearCh < QUALITY_CFG.clearMinValid) return "Too dark to interpret well";
  if (d.clearCh >= QUALITY_CFG.clearSatWarn) return "Too bright / reduce exposure";

  if (d.f5_555 > d.f7_630 * 1.15f && d.f4_515 > d.f8_680) {
    return "Green-side reflection stronger than red side";
  }

  if (d.f7_630 > d.f5_555 * 1.15f || d.f8_680 > d.f6_590) {
    return "Red and late-stage bands are rising";
  }

  if (d.nirToClear > 0.25f) {
    return "NIR is relatively stronger compared with overall brightness";
  }

  return "Mixed spectral balance";
}

void updateDerivedValues(SpectralData &d) {
  d.nirToClear = safeRatio(d.nirCh, d.clearCh);
  d.f5ToF7     = safeRatio(d.f5_555, d.f7_630);
  d.f7ToF5     = safeRatio(d.f7_630, d.f5_555);
  d.f4ToF8     = safeRatio(d.f4_515, d.f8_680);
  d.f8ToF6     = safeRatio(d.f8_680, d.f6_590);
}

void resetSpectralData(SpectralData &d) {
  d.f1_415 = d.f2_445 = d.f3_480 = d.f4_515 = 0;
  d.f5_555 = d.f6_590 = d.f7_630 = d.f8_680 = 0;
  d.clearCh = d.nirCh = 0;
  updateDerivedValues(d);
}

void releaseCameraCapture(CameraCapture &cap) {
  if (cap.fb) {
    esp_camera_fb_return(cap.fb);
    cap.fb = nullptr;
  }
}

void flushCameraFrames(int count, int waitMs) {
  if (!camOk) return;

  for (int i = 0; i < count; i++) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) esp_camera_fb_return(fb);
    delay(waitMs);
  }
}

String sleepLabelFromUs(uint64_t us) {
  uint64_t seconds = us / 1000000ULL;
  if (seconds % 3600ULL == 0) return String((unsigned long)(seconds / 3600ULL)) + " h";
  if (seconds % 60ULL == 0)   return String((unsigned long)(seconds / 60ULL)) + " min";
  return String((unsigned long)seconds) + " s";
}

void stopWifiAfterUpload() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

void turnOffBeforeSleep() {
  if (oledOk) {
    display.clearDisplay();
    display.display();
    display.ssd1306_command(SSD1306_DISPLAYOFF);
  }

  stopWifiAfterUpload();
}

String jsonStatusForOled() {
  return lastJsonUploadOk ? "OK" : "FAIL";
}

String imageStatusForOled() {
  if (!CAM_CFG.sendCameraImage) return "OFF";
  if (lastImageUploadOk) return "OK";
  if (lastImageHttpCode == -10) return "SKIP";
  return "FAIL";
}

String getShortStageForOled(const String &stage) {
  if (stage == "Green/Unripe") return "Unripe";
  if (stage == "Proceeding from Unripe to Ripe") return "U->Ripe";
  if (stage == "Ripe") return "Ripe";
  if (stage == "Proceeding from Ripe to Overripe") return "R->Over";
  if (stage == "Overripe") return "Overripe";
  if (stage == "Spoiled") return "Spoiled";
  if (stage == "Try Again") return "Try Again";
  if (stage == "Weak Reading") return "Weak";
  if (stage == "Overexposed Reading") return "Bright";
  if (stage == "No Data") return "No Data";
  return stage;
}

void showAwakeStatusScreen() {
  showMessage(
    "Banana: " + getShortStageForOled(latestResult.stage),
    "Storage: " + latestResult.storage,
    "Data: " + jsonStatusForOled(),
    "Image: " + imageStatusForOled()
  );
}

void showCountdownScreen(int secLeft) {
  showMessage(
    "Banana: " + getShortStageForOled(latestResult.stage),
    "Storage: " + latestResult.storage,
    "Data: " + jsonStatusForOled(),
    "Sleep in: " + String(secLeft)
  );
}

void showPostUploadStatusAndWait() {
  Serial.println();
  Serial.print("Holding awake for ");
  Serial.print(TIME_CFG.postUploadAwakeMs / 1000UL);
  Serial.println(" seconds before sleep...");

  stopWifiAfterUpload();

  uint32_t startMs = millis();
  int lastShownSec = -1;

  while (millis() - startMs < TIME_CFG.postUploadAwakeMs) {
    uint32_t elapsedMs = millis() - startMs;
    int remainingSec = (int)((TIME_CFG.postUploadAwakeMs - elapsedMs + 999UL) / 1000UL);
    if (remainingSec < 1) remainingSec = 1;

    if (remainingSec != lastShownSec) {
      lastShownSec = remainingSec;

      if (remainingSec > TIME_CFG.finalCountdownSec) {
        showAwakeStatusScreen();
      } else {
        showCountdownScreen(remainingSec);
      }

      Serial.print("Awake wait remaining: ");
      Serial.println(remainingSec);
    }

    delay(50);
  }
}


// part 15 - HDC1080
//----------------------------------------------------------------------------------------------------------
// this part is responssible for temperature and humidity
// a few reads are averaged so the value is more stable

bool hdc1080Begin() {
  Wire.beginTransmission(HDC1080_ADDR);
  if (Wire.endTransmission() != 0) return false;

  Wire.beginTransmission(HDC1080_ADDR);
  Wire.write(0x02);
  Wire.write(0x10);
  Wire.write(0x00);
  return (Wire.endTransmission() == 0);
}

bool readHDC1080Single(float &tempC, float &humRH) {
  Wire.beginTransmission(HDC1080_ADDR);
  Wire.write(0x00);
  if (Wire.endTransmission() != 0) return false;

  delay(20);

  uint8_t n = Wire.requestFrom(HDC1080_ADDR, 4);
  if (n != 4) return false;

  uint16_t rawT = ((uint16_t)Wire.read() << 8) | Wire.read();
  uint16_t rawH = ((uint16_t)Wire.read() << 8) | Wire.read();

  tempC = ((float)rawT / 65536.0f) * 165.0f - 40.0f;
  humRH = ((float)rawH / 65536.0f) * 100.0f;
  return true;
}

bool readHDC1080Stable(float &tempC, float &humRH) {
  for (int i = 0; i < STAB_CFG.hdcDummyReads; i++) {
    float td, hd;
    readHDC1080Single(td, hd);
    delay(STAB_CFG.hdcReadGapMs);
  }

  int okCount = 0;
  float tSum = 0.0f;
  float hSum = 0.0f;

  for (int i = 0; i < STAB_CFG.hdcRealReads; i++) {
    float t, h;
    if (readHDC1080Single(t, h)) {
      tSum += t;
      hSum += h;
      okCount++;
    }
    delay(STAB_CFG.hdcReadGapMs);
  }

  if (okCount == 0) return false;

  tempC = tSum / (float)okCount;
  humRH = hSum / (float)okCount;
  return true;
}


// part 16 - AS7341
//----------------------------------------------------------------------------------------------------------
// for simple optical flow
// LED on then, wait then, remove old frames then,  capture image then read channels and finallyturn led off

bool beginAS7341() {
  if (!as7341.begin()) return false;

  as7341.setATIME(AS7341_CFG.atime);
  as7341.setASTEP(AS7341_CFG.astep);
  as7341.setGain(AS7341_CFG.gain);
  as7341.enableLED(false);

  return true;
}

void setSensorLED(bool turnOn) {
  if (!asOk) return;

  if (turnOn && LED_CFG.useSensorLed) {
    as7341.setLEDCurrent(LED_CFG.ledCurrentMa);
    as7341.enableLED(true);
  } else {
    as7341.enableLED(false);
  }
}

bool readAS7341StableWhileLEDOn(SpectralData &d) {
  if (!asOk) return false;

  for (int i = 0; i < STAB_CFG.asDummyReads; i++) {
    as7341.readAllChannels();
    delay(STAB_CFG.asReadGapMs);
  }

  uint32_t sumF1 = 0, sumF2 = 0, sumF3 = 0, sumF4 = 0, sumF5 = 0;
  uint32_t sumF6 = 0, sumF7 = 0, sumF8 = 0, sumClear = 0, sumNIR = 0;
  int okCount = 0;

  for (int i = 0; i < STAB_CFG.asRealReads; i++) {
    bool ok = as7341.readAllChannels();
    if (ok) {
      sumF1    += as7341.getChannel(AS7341_CHANNEL_415nm_F1);
      sumF2    += as7341.getChannel(AS7341_CHANNEL_445nm_F2);
      sumF3    += as7341.getChannel(AS7341_CHANNEL_480nm_F3);
      sumF4    += as7341.getChannel(AS7341_CHANNEL_515nm_F4);
      sumF5    += as7341.getChannel(AS7341_CHANNEL_555nm_F5);
      sumF6    += as7341.getChannel(AS7341_CHANNEL_590nm_F6);
      sumF7    += as7341.getChannel(AS7341_CHANNEL_630nm_F7);
      sumF8    += as7341.getChannel(AS7341_CHANNEL_680nm_F8);
      sumClear += as7341.getChannel(AS7341_CHANNEL_CLEAR);
      sumNIR   += as7341.getChannel(AS7341_CHANNEL_NIR);
      okCount++;
    }
    delay(STAB_CFG.asReadGapMs);
  }

  if (okCount == 0) return false;

  d.f1_415 = (uint16_t)(sumF1 / okCount);
  d.f2_445 = (uint16_t)(sumF2 / okCount);
  d.f3_480 = (uint16_t)(sumF3 / okCount);
  d.f4_515 = (uint16_t)(sumF4 / okCount);
  d.f5_555 = (uint16_t)(sumF5 / okCount);
  d.f6_590 = (uint16_t)(sumF6 / okCount);
  d.f7_630 = (uint16_t)(sumF7 / okCount);
  d.f8_680 = (uint16_t)(sumF8 / okCount);
  d.clearCh = (uint16_t)(sumClear / okCount);
  d.nirCh   = (uint16_t)(sumNIR / okCount);

  updateDerivedValues(d);
  return true;
}


// part 17 - camera
//----------------------------------------------------------------------------------------------------------
// lets keeo the image  lightweight so upload is more reliable

bool beginCamera() {
  camera_config_t config;
  memset(&config, 0, sizeof(config));

  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = CAM_CFG.frameSize;
  config.jpeg_quality = CAM_CFG.jpegQuality;

  config.fb_count = 1;
  config.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;

  #ifdef CAMERA_GRAB_LATEST
    config.grab_mode = CAMERA_GRAB_LATEST;
  #else
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  #endif

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed: 0x%x\n", err);
    return false;
  }

  return true;
}

CameraCapture captureCameraImage(uint32_t sampleId) {
  CameraCapture cap;
  cap.sampleId = sampleId;

  if (!camOk || !CAM_CFG.sendCameraImage) return cap;

  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Camera capture failed.");
    return cap;
  }

  cap.width = fb->width;
  cap.height = fb->height;
  cap.jpegBytes = fb->len;
  cap.ok = true;

  if (fb->format != PIXFORMAT_JPEG) {
    Serial.println("Camera did not return JPEG frame.");
    esp_camera_fb_return(fb);
    cap.ok = false;
    return cap;
  }

  if (fb->len > CAM_CFG.maxJpegBytes) {
    Serial.printf("JPEG too large, skipping image upload: %u bytes\n", (unsigned int)fb->len);
    esp_camera_fb_return(fb);
    cap.ok = true;
    cap.uploaded = false;
    return cap;
  }

  cap.fb = fb;
  return cap;
}


// part 18 - wifi and upload
// ----------------------------------------------------------------------------------------------------------
// will upload json and image separately
void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  WiFi.mode(WIFI_STA);
  WiFi.begin(NET_CFG.ssid, NET_CFG.password);

  Serial.print("Connecting to Wi-Fi");
  if (oledOk) showMessage("Wi-Fi", "Connecting...");

  uint32_t startMs = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startMs < (uint32_t)UP_CFG.wifiTimeoutMs) {
    delay(500);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Wi-Fi connected. IP: ");
    Serial.println(WiFi.localIP());
    Serial.print("RSSI: ");
    Serial.println(WiFi.RSSI());
  } else {
    Serial.println("Wi-Fi connection failed.");
  }
}

bool ensureWiFiConnected() {
  if (WiFi.status() == WL_CONNECTED) return true;
  connectWiFi();
  return (WiFi.status() == WL_CONNECTED);
}

bool beginHttpClientForUrl(HTTPClient &http, const String &url, WiFiClientSecure &secureClient) {
  if (url.startsWith("https://")) {
    secureClient.setInsecure();
    return http.begin(secureClient, url);
  }
  return http.begin(url);
}

String buildJsonPayload(const SpectralData &d, const BananaResult &r, const CameraCapture &cap, uint32_t sampleId) {
  String payload;
  payload.reserve(3200);

  unsigned long totalCycleSec =
    (unsigned long)(TIME_CFG.deepSleepUs / 1000000ULL) +
    (unsigned long)(TIME_CFG.postUploadAwakeMs / 1000UL);

  payload += "{";

  payload += "\"sample_id\":" + String(sampleId) + ",";
  payload += "\"device_id\":\"" + jsonEscape(String(NET_CFG.deviceId)) + "\",";
  payload += "\"device_millis\":" + String(millis()) + ",";
  payload += "\"sample_interval_sec\":" + String(totalCycleSec) + ",";
  payload += "\"led_enabled\":";
  payload += (LED_CFG.useSensorLed ? "true" : "false");
  payload += ",";
  payload += "\"led_current_ma\":" + String(LED_CFG.ledCurrentMa) + ",";
  payload += "\"wifi_rssi\":" + String(WiFi.RSSI()) + ",";

  payload += "\"readings\":{";
  payload += "\"F1\":" + String(d.f1_415) + ",";
  payload += "\"F2\":" + String(d.f2_445) + ",";
  payload += "\"F3\":" + String(d.f3_480) + ",";
  payload += "\"F4\":" + String(d.f4_515) + ",";
  payload += "\"F5\":" + String(d.f5_555) + ",";
  payload += "\"F6\":" + String(d.f6_590) + ",";
  payload += "\"F7\":" + String(d.f7_630) + ",";
  payload += "\"F8\":" + String(d.f8_680) + ",";
  payload += "\"Clear\":" + String(d.clearCh) + ",";
  payload += "\"NIR\":" + String(d.nirCh);
  payload += "},";

  payload += "\"environment\":{";
  payload += "\"temperature_c\":" + String(d.temperatureC, 2) + ",";
  payload += "\"humidity_rh\":" + String(d.humidityRH, 2) + ",";
  payload += "\"storage\":\"" + jsonEscape(r.storage) + "\"";
  payload += "},";

  payload += "\"ratios\":{";
  payload += "\"nir_to_clear\":" + String(d.nirToClear, 4) + ",";
  payload += "\"f5_to_f7\":" + String(d.f5ToF7, 4) + ",";
  payload += "\"f7_to_f5\":" + String(d.f7ToF5, 4) + ",";
  payload += "\"f4_to_f8\":" + String(d.f4ToF8, 4) + ",";
  payload += "\"f8_to_f6\":" + String(d.f8ToF6, 4);
  payload += "},";

  payload += "\"interpretation\":{";
  payload += "\"reading_quality\":\"" + jsonEscape(r.readingQuality) + "\",";
  payload += "\"reading_quality_advice\":\"" + jsonEscape(r.readingQualityAdvice) + "\",";
  payload += "\"spectral_story\":\"" + jsonEscape(r.spectralStory) + "\",";
  payload += "\"strongest_channel\":\"" + jsonEscape(r.strongestChannel) + "\",";
  payload += "\"strongest_visible_band\":\"" + jsonEscape(r.strongestVisibleBand) + "\"";
  payload += "},";

  payload += "\"classification\":{";
  payload += "\"stage\":\"" + jsonEscape(r.stage) + "\",";
  payload += "\"stage_detail\":\"" + jsonEscape(r.stageDetail) + "\",";
  payload += "\"stage_advice\":\"" + jsonEscape(r.stageAdvice) + "\",";
  payload += "\"spoilage_score\":" + String(r.spoilageScore);
  payload += "},";

  payload += "\"recommendations\":{";
  payload += "\"storage_advice\":\"" + jsonEscape(r.storageAdvice) + "\",";
  payload += "\"eat_recommendation\":\"" + jsonEscape(r.eatRecommendation) + "\",";
  payload += "\"final_recommendation\":\"" + jsonEscape(r.finalRecommendation) + "\"";
  payload += "},";

  payload += "\"camera\":{";
  payload += "\"capture_ok\":";
  payload += (cap.ok ? "true" : "false");
  payload += ",";
  payload += "\"image_uploaded\":";
  payload += (cap.uploaded ? "true" : "false");
  payload += ",";
  payload += "\"width\":" + String(cap.width) + ",";
  payload += "\"height\":" + String(cap.height) + ",";
  payload += "\"jpeg_bytes\":" + String((unsigned int)cap.jpegBytes);
  payload += "}";

  payload += "}";
  return payload;
}

bool uploadImageToServer(CameraCapture &cap, uint32_t sampleId) {
  if (!cap.ok || !cap.fb || !CAM_CFG.sendCameraImage) {
    cap.uploaded = false;
    lastImageUploadOk = false;
    lastImageHttpCode = -10;
    return false;
  }

  if (!ensureWiFiConnected()) {
    cap.uploaded = false;
    lastImageUploadOk = false;
    lastImageHttpCode = -1;
    Serial.println("Image upload skipped: Wi-Fi not connected.");
    return false;
  }

  String url = String(NET_CFG.imageUrl);
  bool success = false;

  for (int attempt = 1; attempt <= UP_CFG.imageRetries; attempt++) {
    HTTPClient http;
    WiFiClientSecure secureClient;
    http.setTimeout(UP_CFG.imageTimeoutMs);

    bool beginOk = beginHttpClientForUrl(http, url, secureClient);
    if (!beginOk) {
      lastImageHttpCode = -2;
      http.end();
      delay(250);
      continue;
    }

    http.addHeader("Content-Type", "image/jpeg");
    http.addHeader("X-Device-Id", String(NET_CFG.deviceId));
    http.addHeader("X-Sample-Id", String(sampleId));
    http.addHeader("X-Image-Width", String(cap.width));
    http.addHeader("X-Image-Height", String(cap.height));
    http.addHeader("X-Jpeg-Bytes", String((unsigned int)cap.jpegBytes));
    http.addHeader("Connection", "close");

    Serial.println("--- Uploading IMAGE ---");
    Serial.print("Attempt: "); Serial.println(attempt);
    Serial.print("Image bytes: "); Serial.println((unsigned int)cap.jpegBytes);

    int httpCode = http.sendRequest("POST", cap.fb->buf, cap.fb->len);
    lastImageHttpCode = httpCode;

    if (httpCode > 0) {
      String response = http.getString();
      Serial.print("Image HTTP code: ");
      Serial.println(httpCode);
      Serial.print("Image reply: ");
      Serial.println(response);

      if (httpCode >= 200 && httpCode < 300) {
        success = true;
        lastImageUploadOk = true;
        cap.uploaded = true;
        http.end();
        break;
      }
    } else {
      Serial.print("Image POST failed. Code: ");
      Serial.println(httpCode);
    }

    http.end();
    delay(300);
  }

  if (!success) {
    lastImageUploadOk = false;
    cap.uploaded = false;
  }

  return success;
}

bool uploadReadingToServer(const SpectralData &d, const BananaResult &r, const CameraCapture &cap, uint32_t sampleId) {
  if (!ensureWiFiConnected()) {
    lastJsonUploadOk = false;
    lastJsonHttpCode = -1;
    Serial.println("JSON upload skipped: Wi-Fi not connected.");
    return false;
  }

  String payload = buildJsonPayload(d, r, cap, sampleId);
  String url = String(NET_CFG.readingUrl);
  bool success = false;

  for (int attempt = 1; attempt <= UP_CFG.jsonRetries; attempt++) {
    HTTPClient http;
    WiFiClientSecure secureClient;
    http.setTimeout(UP_CFG.jsonTimeoutMs);

    bool beginOk = beginHttpClientForUrl(http, url, secureClient);
    if (!beginOk) {
      lastJsonHttpCode = -2;
      http.end();
      delay(250);
      continue;
    }

    http.addHeader("Content-Type", "application/json");
    http.addHeader("Connection", "close");

    Serial.println("--- Uploading JSON ---");
    Serial.print("Attempt: "); Serial.println(attempt);
    Serial.print("JSON bytes: "); Serial.println(payload.length());

    int httpCode = http.POST((uint8_t*)payload.c_str(), payload.length());
    lastJsonHttpCode = httpCode;

    if (httpCode > 0) {
      String response = http.getString();
      Serial.print("JSON HTTP code: ");
      Serial.println(httpCode);
      Serial.print("JSON reply: ");
      Serial.println(response);

      if (httpCode >= 200 && httpCode < 300) {
        success = true;
        lastJsonUploadOk = true;
        http.end();
        break;
      }
    } else {
      Serial.print("JSON POST failed. Code: ");
      Serial.println(httpCode);
    }

    http.end();
    delay(300);
  }

  if (!success) {
    lastJsonUploadOk = false;
  }

  return success;
}


// part 19 - classification logic
// ----------------------------------------------------------------------------------------------------------
// maturity is decided from spectral thresholds
// temp and humidity are still used for storage label and advice, but not
// to override the spectral maturity stage
//
// check order
// 1. no data / saturated
// 2. spoiled
// 3. weak reading
// 4. clearly unripe
// 5. unripe to ripe transition
// 6. clearly ripe
// 7. ripe to overripe transition
// 8. clearly overripe
// 9. fallback

bool isSpoiledPattern(const SpectralData &d, int &scoreOut) {
  bool darkEnough   = (d.clearCh < QUALITY_CFG.clearSpoiledMax);
  bool nirLate      = (d.nirToClear > STAGE_CFG.spoiledNirToClearMin);
  bool deepRedLate  = (d.f8_680 > (uint16_t)(d.f6_590 * STAGE_CFG.spoiledF8OverF6Min));
  bool redLate      = (d.f7_630 > (uint16_t)(d.f5_555 * STAGE_CFG.spoiledF7OverF5Min));

  scoreOut = 0;
  if (darkEnough)  scoreOut++;
  if (nirLate)     scoreOut++;
  if (deepRedLate) scoreOut++;
  if (redLate)     scoreOut++;

  int decaySigns = 0;
  if (nirLate) decaySigns++;
  if (deepRedLate) decaySigns++;
  if (redLate) decaySigns++;

  if (darkEnough && decaySigns >= 1) return true;
  if (!darkEnough && decaySigns == 3) return true;

  return false;
}

bool isClearlyUnripe(const SpectralData &d) {
  return (
    d.f5_555 > (uint16_t)(d.f7_630 * STAGE_CFG.unripeF5OverF7Min) &&
    d.f4_515 > (uint16_t)(d.f8_680 * STAGE_CFG.unripeF4OverF8Min)
  );
}

bool isUnripeToRipeTransition(const SpectralData &d) {
  return (
    d.clearCh >= TRANSITION_CFG.unripeToRipeClearMin &&
    d.f5ToF7 > TRANSITION_CFG.unripeToRipeF5OverF7Min &&
    d.f5ToF7 <= TRANSITION_CFG.unripeToRipeF5OverF7Max &&
    d.f4ToF8 > TRANSITION_CFG.unripeToRipeF4OverF8Min &&
    d.f4ToF8 <= TRANSITION_CFG.unripeToRipeF4OverF8Max
  );
}

bool isClearlyRipe(const SpectralData &d) {
  return (
    d.clearCh >= STAGE_CFG.ripeClearMin &&
    d.f7ToF5 >= STAGE_CFG.ripeF7OverF5Min &&
    d.f7ToF5 <= STAGE_CFG.ripeF7OverF5Max &&
    d.f8ToF6 <= STAGE_CFG.ripeF8OverF6Max
  );
}

bool isRipeToOverripeTransition(const SpectralData &d) {
  return (
    d.clearCh >= TRANSITION_CFG.ripeToOverripeClearMin &&
    (
      (d.f7ToF5 > TRANSITION_CFG.ripeToOverripeF7OverF5Min &&
       d.f7ToF5 <= TRANSITION_CFG.ripeToOverripeF7OverF5Max) ||
      (d.f8ToF6 > TRANSITION_CFG.ripeToOverripeF8OverF6Min &&
       d.f8ToF6 <= TRANSITION_CFG.ripeToOverripeF8OverF6Max)
    )
  );
}

bool isClearlyOverripe(const SpectralData &d) {
  return (
    d.clearCh >= STAGE_CFG.overripeClearMin &&
    (
      d.f7_630 > (uint16_t)(d.f5_555 * STAGE_CFG.overripeF7OverF5Min) ||
      d.f8_680 > (uint16_t)(d.f6_590 * STAGE_CFG.overripeF8OverF6Min)
    )
  );
}

BananaResult classifyBanana(const SpectralData &d) {
  BananaResult result;

  result.stage = "Unknown";
  result.stageDetail = "No valid stage yet";
  result.stageAdvice = "Check reading";

  // temp and humidity are used here for storage meaning
  result.storage = getStorageLabel(d.temperatureC, d.humidityRH);
  result.storageAdvice = getStorageAdvice(result.storage);

  result.readingQuality = getReadingQuality(d.clearCh);
  result.readingQualityAdvice = getReadingQualityAdvice(result.readingQuality);
  result.spectralStory = getSpectralStory(d);
  result.strongestChannel = getStrongestChannelName(d);
  result.strongestVisibleBand = getStrongestVisibleBandName(d);
  result.eatRecommendation = "Monitor again later.";
  result.finalRecommendation = "Need more scans for confidence.";
  result.spoilageScore = 0;

  if (d.clearCh == 0) {
    result.stage = "No Data";
    result.stageDetail = "AS7341 returned no usable values";
    result.stageAdvice = "Check sensor wiring, light, and placement.";
    result.eatRecommendation = "No recommendation available.";
    result.finalRecommendation = "Re-scan after fixing the sensor reading.";
    return result;
  }

  if (d.clearCh >= QUALITY_CFG.clearSatWarn) {
    result.stage = "Overexposed Reading";
    result.stageDetail = "Reading is too bright or saturated";
    result.stageAdvice = "Reduce LED current, exposure, or distance.";
    result.eatRecommendation = "No recommendation available.";
    result.finalRecommendation = "Re-scan with lower exposure.";
    return result;
  }

  int spoilScore = 0;
  if (isSpoiledPattern(d, spoilScore)) {
    result.spoilageScore = spoilScore;
    result.stage = "Spoiled";
    result.stageDetail = "Very dark or strongly late-stage spectral pattern";
    result.stageAdvice = "Check carefully. Discard if smell, leakage, mold, or obvious decay is present.";
    result.eatRecommendation = "Do not consume if clear spoilage is present.";
    result.finalRecommendation = "This sample is closest to spoiled / black stage.";
    return result;
  }

  if (d.clearCh < QUALITY_CFG.clearMinValid) {
    result.stage = "Weak Reading";
    result.stageDetail = "Too dark for a confident non-spoiled classification";
    result.stageAdvice = "Use more light or move sensor closer.";
    result.eatRecommendation = "No recommendation available.";
    result.finalRecommendation = "Re-scan under more stable light.";
    return result;
  }

  if (isClearlyUnripe(d)) {
    result.stage = "Green/Unripe";
    result.stageDetail = "Green-side spectral bands are dominant";
    result.stageAdvice = "Not ready yet.";
    result.eatRecommendation = "Wait and monitor over the next few days.";
    result.finalRecommendation = "This sample is closest to the green stage.";
    result.spoilageScore = 0;
    return result;
  }

  if (isUnripeToRipeTransition(d)) {
    result.stage = "Proceeding from Unripe to Ripe";
    result.stageDetail = "Reading sits in the small buffer band between green and ripe";
    result.stageAdvice = "Ripening has started.";
    result.eatRecommendation = "Wait a bit longer for a fuller ripe stage, or keep monitoring.";
    result.finalRecommendation = "This banana is moving from unripe toward ripe.";
    result.spoilageScore = 0;
    return result;
  }

  if (isClearlyRipe(d)) {
    result.stage = "Ripe";
    result.stageDetail = "Balanced yellow stage";
    result.stageAdvice = "Good to eat.";
    result.eatRecommendation = "Eat now or within 1 to 2 days.";
    result.finalRecommendation = "This sample is closest to the ripe yellow stage.";
    result.spoilageScore = 1;
    return result;
  }

  if (isRipeToOverripeTransition(d)) {
    result.stage = "Proceeding from Ripe to Overripe";
    result.stageDetail = "Reading sits in the small buffer band between ripe and overripe";
    result.stageAdvice = "Still edible, but it is moving past the best eating window.";
    result.eatRecommendation = "Eat soon, ideally within 1 day.";
    result.finalRecommendation = "This banana is moving from ripe toward overripe.";
    result.spoilageScore = 1;
    return result;
  }

  if (isClearlyOverripe(d)) {
    result.stage = "Overripe";
    result.stageDetail = "Late-stage red and deep-red bands are rising";
    result.stageAdvice = "Eat very soon.";
    result.eatRecommendation = "Use today or very soon.";
    result.finalRecommendation = "This sample is closest to a dark yellow / spotted overripe stage.";
    result.spoilageScore = 2;
    return result;
  }

  if (result.strongestVisibleBand == "F4" || result.strongestVisibleBand == "F5") {
    result.stage = "Green/Unripe";
    result.stageDetail = "Fallback selected green side as dominant";
    result.stageAdvice = "Likely still not ready.";
    result.eatRecommendation = "Wait and monitor.";
    result.finalRecommendation = "Fallback points to unripe side.";
    result.spoilageScore = 0;
    return result;
  }

  if (result.strongestVisibleBand == "F6") {
    result.stage = "Ripe";
    result.stageDetail = "Fallback selected yellow band as dominant";
    result.stageAdvice = "Likely good to eat.";
    result.eatRecommendation = "Eat soon.";
    result.finalRecommendation = "Fallback points to ripe side.";
    result.spoilageScore = 1;
    return result;
  }

  if (result.strongestVisibleBand == "F7" || result.strongestVisibleBand == "F8") {
    result.stage = "Overripe";
    result.stageDetail = "Fallback selected late-stage band as dominant";
    result.stageAdvice = "Use soon.";
    result.eatRecommendation = "Best used now.";
    result.finalRecommendation = "Fallback points to overripe side.";
    result.spoilageScore = 2;
    return result;
  }

  result.stage = "Try Again";
  result.stageDetail = "Reading does not fit the stage rules clearly";
  result.stageAdvice = "Adjust placement and scan again.";
  result.eatRecommendation = "No recommendation available.";
  result.finalRecommendation = "Need a cleaner scan.";
  result.spoilageScore = 0;
  return result;
}


// part 20 - serial report
//----------------------------------------------------------------------------------------------------------
// structured report after each cycle for debugging purpose and final demo

void printBandLine(const String &name, int wavelength, uint16_t value, const String &meaning) {
  Serial.print(name);
  Serial.print("  ");

  if (wavelength > 0) {
    Serial.print(wavelength);
    Serial.print(" nm");
  } else {
    Serial.print("--");
  }

  Serial.print(" | Value: ");
  Serial.print(value);
  Serial.print(" | Level: ");
  Serial.print(getSignalLevelLabel(value));
  Serial.print(" | Meaning: ");
  Serial.println(meaning);
}

void printFullReport(const SpectralData &d, const BananaResult &r, const CameraCapture &cap, uint32_t sampleId) {
  Serial.println();
  Serial.println("FULL SENSOR REPORT");
  Serial.println("--------------------------------------------------");
  Serial.print("Boot counter               : "); Serial.println(BOOT_COUNTER);
  Serial.print("Sample ID                  : "); Serial.println(sampleId);
  Serial.print("Wi-Fi status               : "); Serial.println(WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected");
  Serial.print("Wi-Fi RSSI                 : "); Serial.println(WiFi.RSSI());

  Serial.println();
  Serial.println("[LIGHT / EXPOSURE]");
  Serial.print("LED enabled                : "); Serial.println(LED_CFG.useSensorLed ? "YES" : "NO");
  Serial.print("LED current (mA)           : "); Serial.println(LED_CFG.ledCurrentMa);
  Serial.print("LED settle (ms)            : "); Serial.println(LED_CFG.ledSettleMs);
  Serial.print("LED extra ON (ms)          : "); Serial.println(LED_CFG.ledExtraOnMs);
  Serial.print("ATIME                      : "); Serial.println(AS7341_CFG.atime);
  Serial.print("ASTEP                      : "); Serial.println(AS7341_CFG.astep);
  Serial.print("Gain                       : "); Serial.println("Configured in code");

  Serial.println();
  Serial.println("[ENVIRONMENT]");
  Serial.print("Temperature (C)            : "); Serial.println(d.temperatureC, 2);
  Serial.print("Humidity (%)               : "); Serial.println(d.humidityRH, 2);
  Serial.print("Storage                    : "); Serial.println(r.storage);
  Serial.print("Storage advice             : "); Serial.println(r.storageAdvice);

  Serial.println();
  Serial.println("[SPECTRAL]");
  printBandLine("F1", 415, d.f1_415, "Violet reflection");
  printBandLine("F2", 445, d.f2_445, "Blue-indigo reflection");
  printBandLine("F3", 480, d.f3_480, "Blue reflection");
  printBandLine("F4", 515, d.f4_515, "Cyan/green edge");
  printBandLine("F5", 555, d.f5_555, "Green region");
  printBandLine("F6", 590, d.f6_590, "Yellow/amber region");
  printBandLine("F7", 630, d.f7_630, "Orange/red region");
  printBandLine("F8", 680, d.f8_680, "Deep-red region");
  printBandLine("CL", 0, d.clearCh, "Overall visible brightness");
  printBandLine("NI", 910, d.nirCh, "Near-infrared reflection");

  Serial.println();
  Serial.println("[RATIOS]");
  Serial.print("F5/F7      : "); Serial.println(d.f5ToF7, 3);
  Serial.print("F7/F5      : "); Serial.println(d.f7ToF5, 3);
  Serial.print("F4/F8      : "); Serial.println(d.f4ToF8, 3);
  Serial.print("F8/F6      : "); Serial.println(d.f8ToF6, 3);
  Serial.print("NIR/Clear  : "); Serial.println(d.nirToClear, 3);

  Serial.println();
  Serial.println("[CLASSIFICATION]");
  Serial.print("Reading quality            : "); Serial.println(r.readingQuality);
  Serial.print("Reading advice             : "); Serial.println(r.readingQualityAdvice);
  Serial.print("Spectral story             : "); Serial.println(r.spectralStory);
  Serial.print("Strongest channel          : "); Serial.println(r.strongestChannel);
  Serial.print("Strongest visible band     : "); Serial.println(r.strongestVisibleBand);
  Serial.print("Spoilage score             : "); Serial.println(r.spoilageScore);
  Serial.print("Stage                      : "); Serial.println(r.stage);
  Serial.print("Stage detail               : "); Serial.println(r.stageDetail);
  Serial.print("Stage advice               : "); Serial.println(r.stageAdvice);
  Serial.print("Eat recommendation         : "); Serial.println(r.eatRecommendation);
  Serial.print("Final recommendation       : "); Serial.println(r.finalRecommendation);

  Serial.println();
  Serial.println("[IMAGE / UPLOAD]");
  Serial.print("Camera init                : "); Serial.println(camOk ? "OK" : "FAIL");
  Serial.print("Capture success            : "); Serial.println(cap.ok ? "YES" : "NO");
  Serial.print("Image uploaded             : "); Serial.println(cap.uploaded ? "YES" : "NO");
  Serial.print("JPEG bytes                 : "); Serial.println((unsigned int)cap.jpegBytes);
  Serial.print("Image size                 : "); Serial.print(cap.width); Serial.print("x"); Serial.println(cap.height);
  Serial.print("JSON upload success        : "); Serial.println(lastJsonUploadOk ? "YES" : "NO");
  Serial.print("JSON HTTP code             : "); Serial.println(lastJsonHttpCode);
  Serial.print("Image upload success       : "); Serial.println(lastImageUploadOk ? "YES" : "NO");
  Serial.print("Image HTTP code            : "); Serial.println(lastImageHttpCode);
  Serial.println("--------------------------------------------------");
}


// part 21 - main sensor and upload flow
//----------------------------------------------------------------------------------------------------------
// 1. read temp and humidity
// 2. turn on LED
// 3. wait a bit
// 4. flush old camera frames
// 5. capture image
// 6. read spectrum
// 7. LED off
// 8. classify maturity
// 9. compute storage from temp and humidity
// 10. upload image
// 11. upload JSON
void clearLastUploadFlags() {
  lastJsonUploadOk  = false;
  lastJsonHttpCode  = 0;
  lastImageUploadOk = false;
  lastImageHttpCode = 0;
}

void acquireEnvironmentData(SpectralData &d) {
  if (hdcOk) {
    if (!readHDC1080Stable(d.temperatureC, d.humidityRH)) {
      d.temperatureC = NAN;
      d.humidityRH   = NAN;
    }
  } else {
    d.temperatureC = NAN;
    d.humidityRH   = NAN;
  }
}

void acquireOpticalData(SpectralData &d, CameraCapture &cap, uint32_t sampleId) {
  if (asOk && LED_CFG.useSensorLed) {
    setSensorLED(true);

    if (LED_CFG.ledSettleMs > 0) {
      delay(LED_CFG.ledSettleMs);
    }

    if (camOk && CAM_CFG.sendCameraImage) {
      flushCameraFrames(CAM_CFG.flushFrameCount, CAM_CFG.flushWaitMs);
    }

    cap = captureCameraImage(sampleId);

    if (!readAS7341StableWhileLEDOn(d)) {
      resetSpectralData(d);
    }

    if (LED_CFG.ledExtraOnMs > 0) {
      delay(LED_CFG.ledExtraOnMs);
    }

    setSensorLED(false);
  } else {
    if (camOk && CAM_CFG.sendCameraImage) {
      flushCameraFrames(CAM_CFG.flushFrameCount, CAM_CFG.flushWaitMs);
    }

    cap = captureCameraImage(sampleId);

    if (asOk) {
      if (!readAS7341StableWhileLEDOn(d)) {
        resetSpectralData(d);
      }
    } else {
      resetSpectralData(d);
    }
  }
}

void computeBananaResult(const SpectralData &d, BananaResult &r) {
  r = classifyBanana(d);
}

void sendCollectedData(const SpectralData &d, const BananaResult &r, CameraCapture &cap, uint32_t sampleId) {
  if (cap.ok && cap.fb) {
    uploadImageToServer(cap, sampleId);
  } else {
    cap.uploaded = false;
  }

  uploadReadingToServer(d, r, cap, sampleId);
}

void sampleAllSensorsAndUpload(uint32_t sampleId) {
  clearLastUploadFlags();

  acquireEnvironmentData(data);
  acquireOpticalData(data, latestImage, sampleId);
  computeBananaResult(data, latestResult);
  sendCollectedData(data, latestResult, latestImage, sampleId);
  printFullReport(data, latestResult, latestImage, sampleId);
  releaseCameraCapture(latestImage);
}

void goToDeepSleep() {
  Serial.println();
  Serial.println("Going to deep sleep...");
  Serial.print("Sleep duration: ");
  Serial.println(sleepLabelFromUs(TIME_CFG.deepSleepUs));

  turnOffBeforeSleep();
  esp_sleep_enable_timer_wakeup(TIME_CFG.deepSleepUs);
  esp_deep_sleep_start();
}


// part 22 - setup
//----------------------------------------------------------------------------------------------------------
// all one time init happens here
void setup() {
  Serial.begin(115200);
  delay(250);

  BOOT_COUNTER++;
  uint32_t sampleId = BOOT_COUNTER;

  Wire.begin(PIN_QWIIC_SDA, PIN_QWIIC_SCL);
  delay(100);

  oledOk = display.begin(SSD1306_SWITCHCAPVCC, 0x3D);
  if (!oledOk) {
    oledOk = display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  }

  if (oledOk) {
    display.setRotation(2);
    showMessage("Booting...", "Sample: " + String(sampleId));
  }

  hdcOk = hdc1080Begin();
  asOk  = beginAS7341();
  camOk = beginCamera();

  Serial.println();
  Serial.println("BANANA MONITOR - FINAL SUBMISSION SKETCH");
  Serial.println("--------------------------------------------------");
  Serial.print("Boot counter : "); Serial.println(BOOT_COUNTER);
  Serial.print("Sample ID    : "); Serial.println(sampleId);
  Serial.print("OLED         : "); Serial.println(oledOk ? "OK" : "FAIL");
  Serial.print("HDC1080      : "); Serial.println(hdcOk ? "OK" : "FAIL");
  Serial.print("AS7341       : "); Serial.println(asOk ? "OK" : "FAIL");
  Serial.print("CAMERA       : "); Serial.println(camOk ? "OK" : "FAIL");
  Serial.print("Reading URL  : "); Serial.println(NET_CFG.readingUrl);
  Serial.print("Image URL    : "); Serial.println(NET_CFG.imageUrl);
  Serial.print("Sleep        : "); Serial.println(sleepLabelFromUs(TIME_CFG.deepSleepUs));
  Serial.print("Awake hold   : "); Serial.print(TIME_CFG.postUploadAwakeMs / 1000UL); Serial.println(" s");
  Serial.println("--------------------------------------------------");

  connectWiFi();

  if (oledOk) {
    showMessage(
      "Init done",
      "HDC:" + String(hdcOk ? "OK" : "FAIL"),
      "AS7:" + String(asOk ? "OK" : "FAIL"),
      "CAM:" + String(camOk ? "OK" : "FAIL")
    );
    delay(1000);
  }

  sampleAllSensorsAndUpload(sampleId);
  showPostUploadStatusAndWait();
  goToDeepSleep();
}


// part 23 - loop
//----------------------------------------------------------------------------------------------------------
// not used becuase device sleeps after each cycle
void loop() {
  // not used
}