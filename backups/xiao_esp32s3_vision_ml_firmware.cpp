// =============================================================================
//  XIAO ESP32S3 SENSE / XIAO ML KIT  -  FULL VISION ML  -  firmware-v005-self  (USB + WiFi + Bluetooth link)
//
//  Collect small images, train a tiny CNN, and classify what the camera sees,
//  all on the microcontroller. A companion web page can do the training faster.
//
//  By Jeremy Ellis   (MIT license, use at your own risk)
//  With free tier assistance from Claude (code overview), ChatGPT (critique),
//  Gemini (research) and Copilot (alternate).
//  GitHub   https://github.com/hpssjellis
//  LinkedIn https://www.linkedin.com/in/jeremy-ellis-4237a9bb/
//
// -----------------------------------------------------------------------------
//  NEW IN v005-self: ALL FUNCTIONS AND VARIABLES PRE-DECLARED FOR ARDUINO IDE COMPATIBILITY
//    - Full self-contained implementation with strict top-down declaration ordering.
//    - Fixed compiler issues regarding missing function scopes and forward references.
//    - ONE TEXT PROTOCOL ON USB, WiFi AND BLUETOOTH, WITH FILE TRANSFER.
// =============================================================================

// Remove the // below AFTER copying myWeights.h from the SD card into the sketch folder.
//#define USE_BAKED_WEIGHTS

#ifdef USE_BAKED_WEIGHTS
  #include "myWeights.h"
#endif

// ---- WiFi settings ---------------------------------------------------
#define MY_USE_WIFI 1                      // 1 = WiFi + WebSocket link, 0 = USB serial only
const char* myWifiSsid = "";               // your router; leave empty to always use the board's own network
const char* myWifiPass = "";
const char* myApSsid   = "XIAO-Vision";    // fallback network the board makes (join it, then use 192.168.4.1)
const char* myApPass   = "xiaovision";
const char* myHostName = "xiao-vision";    // page address xiao-vision.local
const unsigned long myWifiTimeoutMs = 12000;

// ---- Bluetooth settings (v004) -------------------------------------------------
#define MY_USE_BLE 1                       // 1 = Web Bluetooth link (needs library NimBLE-Arduino 2.x), 0 = leave it out
const char* myBleName = "XIAO-Vision";
#define MY_BLE_SERVICE_UUID "7a1b0001-5d7c-4f6e-9a3e-0f2c4b1d8e01"
#define MY_BLE_RX_UUID      "7a1b0002-5d7c-4f6e-9a3e-0f2c4b1d8e01"   // the page writes commands here
#define MY_BLE_TX_UUID      "7a1b0003-5d7c-4f6e-9a3e-0f2c4b1d8e01"   // the board sends lines here (notify)
#define MY_BLE_RING 4096                   // bytes the page may have in flight

#include "esp_camera.h"
#include "img_converters.h"
#include "FS.h"
#include "SD.h"
#include "SPI.h"
#include <vector>
#include <algorithm>
#include <U8g2lib.h>
#include <Wire.h>
#include "mbedtls/base64.h"
#if MY_USE_WIFI
  #include <WiFi.h>
  #include <ESPmDNS.h>
  #include <WebSocketsServer.h>
#endif
#if MY_USE_BLE
  #include <NimBLEDevice.h>
#endif


// =============================================================================
//  SETTINGS YOU MAY CHANGE
// =============================================================================

// ---- Classes -----------------------------------------------------------------
#define NUM_CLASSES 3
String myClassLabels[NUM_CLASSES] = {"0Blank", "1Cup", "2Pen"};

// ---- Model layout -----------------------------------------------------------
#define INPUT_SIZE    64
#define CONV1_FILTERS 4
#define CONV2_FILTERS 8

// ---- Training settings --------------------------------------------------------------
float LEARNING_RATE    = 0.0003;
int   BATCH_SIZE       = 6;
int   TARGET_EPOCHS    = 20;
int   VALIDATION_IMAGES = 3;

// ---- Touch pad (A0) -----------------------------------------------------------
const int myThresholdPress   = 1100;
const int myThresholdRelease = 900;

// ---- Camera picture settings --------------------------------------------------
#define MY_CAM_HMIRROR        1
#define MY_CAM_VFLIP          1
#define MY_CAM_BRIGHTNESS     1
#define MY_CAM_AE_LEVEL       1
#define MY_CAM_WARMUP_FRAMES  5


// =============================================================================
//  DERIVED SIZES
// =============================================================================
#define CONV1_IN_STRIDE  27
#define CONV2_IN_STRIDE  (CONV1_FILTERS * 9)

#define CONV1_WEIGHTS    (CONV1_IN_STRIDE * CONV1_FILTERS)
#define CONV2_WEIGHTS    (CONV2_IN_STRIDE * CONV2_FILTERS)

#define CONV1_OUTPUT_SIZE (INPUT_SIZE - 2)
#define POOL1_OUTPUT_SIZE (CONV1_OUTPUT_SIZE / 2)
#define CONV2_OUTPUT_SIZE (POOL1_OUTPUT_SIZE - 2)
#define FLATTENED_SIZE    (CONV2_OUTPUT_SIZE * CONV2_OUTPUT_SIZE * CONV2_FILTERS)

#define CONV1_OUT_FLOATS  (CONV1_OUTPUT_SIZE * CONV1_OUTPUT_SIZE * CONV1_FILTERS)
#define POOL1_OUT_FLOATS  (POOL1_OUTPUT_SIZE * POOL1_OUTPUT_SIZE * CONV1_FILTERS)

#define OUTPUT_WEIGHTS   (FLATTENED_SIZE * NUM_CLASSES)

#define MY_EXPECTED_WEIGHT_BYTES ((size_t)(CONV1_WEIGHTS + CONV1_FILTERS + CONV2_WEIGHTS + CONV2_FILTERS + OUTPUT_WEIGHTS + NUM_CLASSES) * 4)

static_assert(INPUT_SIZE % 2 == 0, "INPUT_SIZE must be even");
static_assert(CONV2_OUTPUT_SIZE >= 1, "INPUT_SIZE is too small");
static_assert(NUM_CLASSES >= 2, "Need at least 2 classes");


// =============================================================================
//  XIAO ESP32S3 CAMERA PINS
// =============================================================================
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM     10
#define SIOD_GPIO_NUM     40
#define SIOC_GPIO_NUM     39
#define Y9_GPIO_NUM       48
#define Y8_GPIO_NUM       11
#define Y7_GPIO_NUM       12
#define Y6_GPIO_NUM       14
#define Y5_GPIO_NUM       16
#define Y4_GPIO_NUM       18
#define Y3_GPIO_NUM       17
#define Y2_GPIO_NUM       15
#define VSYNC_GPIO_NUM    38
#define HREF_GPIO_NUM     47
#define PCLK_GPIO_NUM     13


// =============================================================================
//  GLOBAL VARIABLES
// =============================================================================
U8G2_SSD1306_72X40_ER_1_HW_I2C u8g2(U8G2_R2, U8X8_PIN_NONE);

bool mySDavailable = false;
bool myCameraOk    = false;
bool myWeightsTrained = false;

const int myTotalItems = NUM_CLASSES + 2;
int  myMenuIndex = 1;
bool myIsSelected = false;
unsigned long myLastActivityTime = 0;
unsigned long myLastTapTime = 0;
const int myTapCooldown = 250;

struct TouchState {
  bool isTouching = false;
  int tapCount = 0;
  unsigned long firstTapTime = 0;
  unsigned long lastReleaseTime = 0;
  unsigned long lastCheckTime = 0;
};
TouchState myTouch;
const unsigned long myTapWindow = 800;
const int           myLongPressTaps = 3;
const unsigned long myDebounceMs = 50;

uint8_t* myRgbBuffer = nullptr;
int myResizeLut[INPUT_SIZE];

float* myInputBuffer = nullptr;
float* myConv1_w = nullptr;
float* myConv1_b = nullptr;
float* myConv2_w = nullptr;
float* myConv2_b = nullptr;
float* myOutput_w = nullptr;
float* myOutput_b = nullptr;

float* myConv1_output = nullptr;
float* myPool1_output = nullptr;
float* myConv2_output = nullptr;
float* myDense_output = nullptr;

bool myTrainingBuffersReady = false;
float* myConv1_w_grad = nullptr;
float* myConv1_b_grad = nullptr;
float* myConv2_w_grad = nullptr;
float* myConv2_b_grad = nullptr;
float* myOutput_w_grad = nullptr;
float* myOutput_b_grad = nullptr;
float* myConv1_w_m = nullptr;
float* myConv1_w_v = nullptr;
float* myConv1_b_m = nullptr;
float* myConv1_b_v = nullptr;
float* myConv2_w_m = nullptr;
float* myConv2_w_v = nullptr;
float* myConv2_b_m = nullptr;
float* myConv2_b_v = nullptr;
float* myOutput_w_m = nullptr;
float* myOutput_w_v = nullptr;
float* myOutput_b_m = nullptr;
float* myOutput_b_v = nullptr;
float* myDense_grad = nullptr;
float* myConv2_grad = nullptr;
float* myPool1_grad = nullptr;
float* myConv1_grad = nullptr;

struct MyBufSpec {
  float** ptr;
  size_t count;
  bool zero;
};

struct TrainingItem {
  String path;
  int label;
};
std::vector<TrainingItem> myTrainingData;

bool myDebugStream = false;
unsigned long myDebugLastSeen = 0;

#if MY_USE_WIFI
WebSocketsServer myWs(81);
#endif
bool     myWifiUp = false;
int      myWsClient = -1;
uint8_t* myLkBuf = nullptr;
size_t   myLkTotal = 0;
size_t   myLkGot = 0;
uint32_t myLkSum = 0;
bool     myLkActive = false;
bool     myLkCapture = false;
String   myLkLine;

char     myKeyQueue[16];
uint8_t  myKeyHead = 0;
uint8_t  myKeyTail = 0;
bool     myLkSnapWanted = false;
unsigned long myLkSnapCount = 0;

bool     myPutActive = false;
uint8_t* myPutBuf = nullptr;
size_t   myPutTotal = 0;
size_t   myPutGot = 0;
uint32_t myPutSum = 0;
String   myPutPath;

#if MY_USE_BLE
NimBLEServer*         myBleServer = nullptr;
NimBLECharacteristic* myBleTx = nullptr;
bool     myBleConnected = false;
uint16_t myBleMtu = 23;
uint8_t  myBleRxRing[MY_BLE_RING];
volatile uint16_t myBleRxHead = 0;
volatile uint16_t myBleRxTail = 0;
String   myBleRxLine;
#endif


// =============================================================================
//  FORWARD DECLARATIONS (Full Set for Arduino IDE Pre-declaration Rule)
// =============================================================================
inline float clip_value(float v, float mn = -100, float mx = 100);
inline float leaky_relu(float x);
inline float leaky_relu_deriv(float x);

void myShowMessage(const char* l1, const char* l2 = nullptr, const char* l3 = nullptr);
void myRenderRgbToOLED(int imageCount);
void myDisplayImageOnOLED(camera_fb_t* fb, int imageCount);

int  myReadTouch();
void myResetTouchState();
void myUpdateTouchState();
int  myCheckTouchInput();
void myCheckTouchBackground();
int  myPeekTouchAction();

void   myAllocateMemory();
size_t myTrainingBytesNeeded();
bool   myAllocateTrainingBuffers();
void   myResetAdam();
static void myDumpArray(File& file, const char* name, const float* data, int size);
void   myExportHeader();
bool   myLoadWeights();
void   mySaveWeights();

static bool myJsonInt(const String& t, const char* key, int& out);
static int  myJsonStringArray(const String& t, const char* key, std::vector<String>& out);
void myLoadConfig();

bool myHandleDebugChar(char c);
static void myPrintB64(const uint8_t* d, size_t n);
void myDebugSendFrame(char kind, int n, camera_fb_t* fb, int pred, const float* logits);

size_t myB64(char* dst, size_t cap, const uint8_t* d, size_t n);
void   myLkWifiBegin();
void   myLkPoll();
void   myLkEmit(const char* s, size_t n);
void   myLkReply(const String& s);
bool   myLkHandleChar(char c);
void   myLkHandleLine(const String& line);
void   myLkAppend(const uint8_t* d, size_t n);
void   myLkFinish();
int    myLkHex(char c);
void   myLkServeSnap();
void   myLkInfo();
void   myLkWriteConfig(const String& json);
void   myKeyPush(char c);
bool   myKeyAvailable();
char   myKeyRead();
bool   myLkPathOk(const String& p);
void   myLkStat(String path);
void   myLkList(String path);
void   myLkRead(const String& args);
void   myLkPutBegin(const String& args);
void   myLkPutChunk(const String& line);
void   myLkPutFinish();
void   myLkBleBegin();
void   myBleService();
void   myBleFeed(char c);
void   myBleSend(const char* s, size_t n);

#if MY_USE_BLE
class MyBleServerCb : public NimBLEServerCallbacks {
 public:
  void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override;
  void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override;
  void onMTUChange(uint16_t mtu, NimBLEConnInfo& connInfo) override;
};
class MyBleRxCb : public NimBLECharacteristicCallbacks {
 public:
  void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) override;
};
#endif

#if MY_USE_WIFI
void   myLkWsEvent(uint8_t num, WStype_t type, uint8_t* p, size_t len);
#endif

void   myResizeToInput(const uint8_t* rgb240, float* dst);
bool   myLoadImageFromFile(const char* path, float* buf);
String myUniqueImagePath(const String& folder);

void myForwardPass(float* input, float* logits);
void myBackwardDense(int label);
void myBackwardConv2();
void myBackwardPool1();
void myBackwardConv1();
void myAdamUpdate(float* w, float* g, float* m, float* v, int size, int step);
void myUpdateWeights(int step);

void myActionCollect(int classIdx);
bool myTrainingExitRequested();
void myActionTrain();
void myActionInfer();

String myMenuLabel(int item);
void myResetMenuState();
void myDrawMenu();
void myExecuteMenuItem(int idx);
void myHandleMenuNavigation();


// =============================================================================
//  SETUP AND LOOP
// =============================================================================
void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000);
  delay(1000);

  Serial.println("\n=== XIAO ESP32S3 Vision ML (firmware-v005-self) ===");
  Serial.printf("Layout: INPUT_SIZE %d, CONV1_FILTERS %d, CONV2_FILTERS %d, NUM_CLASSES %d\n",
                INPUT_SIZE, CONV1_FILTERS, CONV2_FILTERS, NUM_CLASSES);
  Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());
  Serial.printf("Free PSRAM: %d bytes\n", ESP.getFreePsram());

  myRgbBuffer = (uint8_t*)ps_malloc(240 * 240 * 3);
  if (!myRgbBuffer) {
    Serial.println("FATAL: could not allocate the picture buffer. Is PSRAM set to OPI PSRAM?");
    myShowMessage("PSRAM ERROR!", "Set PSRAM to", "OPI PSRAM");
    while (1) { delay(1000); }
  }

  for (int i = 0; i < INPUT_SIZE; i++) {
    myResizeLut[i] = min((int)((i + 0.5) * 240.0 / INPUT_SIZE), 239);
  }

  pinMode(A0, INPUT);
  u8g2.begin();
  u8g2.setFont(u8g2_font_6x10_tf);

  pinMode(21, OUTPUT);
  digitalWrite(21, HIGH);
  delay(100);
  Serial.println("Checking SD card...");
  SPI.begin();
  SPI.setFrequency(400000);
  mySDavailable = SD.begin(21, SPI, 400000, "/sd", 5, false);

  if (!mySDavailable) {
    SD.end();
    Serial.println("No SD card - continuing without it");
    myShowMessage("No SD card");
    delay(2000);
  } else {
    Serial.println("SD card mounted successfully");
    myLoadConfig();
  }

  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM; config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM; config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM; config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM; config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM; config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM; config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM; config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM; config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_240X240;
  config.jpeg_quality = 12;
  config.fb_count = 1;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  esp_err_t camErr = esp_camera_init(&config);
  if (camErr != ESP_OK) {
    Serial.printf("Camera init FAILED: 0x%x\n", camErr);
  } else {
    myCameraOk = true;
    Serial.println("Camera initialized"); Serial.flush();
    sensor_t* s = esp_camera_sensor_get();
    if (s != NULL) {
      s->set_hmirror(s, MY_CAM_HMIRROR);
      s->set_vflip(s, MY_CAM_VFLIP);
      s->set_brightness(s, MY_CAM_BRIGHTNESS);
      s->set_ae_level(s, MY_CAM_AE_LEVEL);
    }
    Serial.println("[boot 2] camera warm-up"); Serial.flush();
    for (int i = 0; i < MY_CAM_WARMUP_FRAMES; i++) {
      camera_fb_t* warm = esp_camera_fb_get();
      if (warm) esp_camera_fb_return(warm);
      delay(60);
    }
  }

  esp_log_level_set("*", ESP_LOG_WARN);
  esp_log_level_set("esp_camera", ESP_LOG_ERROR);

  Serial.println("[boot 3] camera done"); Serial.flush();
  myLkWifiBegin();
  myLkBleBegin();

  Serial.println("[boot 9] allocating model memory"); Serial.flush();
  myAllocateMemory();
  Serial.println("[boot 10] memory ready"); Serial.flush();

  Serial.printf("Weight file for this layout: %u bytes\n", (unsigned)MY_EXPECTED_WEIGHT_BYTES);
  Serial.printf("Training on the device needs about %u KB of PSRAM (free now: %u KB)\n",
                (unsigned)(myTrainingBytesNeeded() / 1024), (unsigned)(ESP.getFreePsram() / 1024));

#ifdef USE_BAKED_WEIGHTS
  memcpy(myConv1_w,  myModel_conv1_w,  CONV1_WEIGHTS  * sizeof(float));
  memcpy(myConv1_b,  myModel_conv1_b,  CONV1_FILTERS  * sizeof(float));
  memcpy(myConv2_w,  myModel_conv2_w,  CONV2_WEIGHTS  * sizeof(float));
  memcpy(myConv2_b,  myModel_conv2_b,  CONV2_FILTERS  * sizeof(float));
  memcpy(myOutput_w, myModel_output_w, OUTPUT_WEIGHTS * sizeof(float));
  memcpy(myOutput_b, myModel_output_b, NUM_CLASSES    * sizeof(float));
  Serial.println("Baked-in weights loaded from myWeights.h");
  myWeightsTrained = true;
#endif

  if (myLoadWeights()) {
    Serial.println("SD weights loaded - overriding baked-in weights");
  }

  myLastActivityTime = millis();
  myResetMenuState();
  delay(2000);

  Serial.println("System ready - Tap A0 to navigate, 3+ taps to select");
  myDrawMenu();
}

void loop() {
  myLkPoll();
  myHandleMenuNavigation();
}


// =============================================================================
//  MATH HELPERS
// =============================================================================
inline float clip_value(float v, float mn, float mx) {
  if (isnan(v) || isinf(v)) return 0;
  return constrain(v, mn, mx);
}

inline float leaky_relu(float x) { return x > 0 ? x : 0.1f * x; }
inline float leaky_relu_deriv(float x) { return x > 0 ? 1.0f : 0.1f; }


// =============================================================================
//  OLED HELPERS
// =============================================================================
void myShowMessage(const char* l1, const char* l2, const char* l3) {
  u8g2.firstPage();
  do {
    u8g2.setFont(u8g2_font_5x7_tf);
    if (l1) u8g2.drawStr(0, 10, l1);
    if (l2) u8g2.drawStr(0, 22, l2);
    if (l3) u8g2.drawStr(0, 34, l3);
  } while (u8g2.nextPage());
}

void myRenderRgbToOLED(int imageCount) {
  int myOledWidth  = u8g2.getDisplayWidth();
  int myOledHeight = u8g2.getDisplayHeight();
  int myScaleX = 240 / myOledWidth;
  int myScaleY = 240 / myOledHeight;

  u8g2.firstPage();
  do {
    for (int myOledX = 0; myOledX < myOledWidth; myOledX++) {
      for (int myOledY = 0; myOledY < myOledHeight; myOledY++) {
        size_t myPixelIndex = ((myOledY * myScaleY) * 240 + (myOledX * myScaleX)) * 3;
        uint8_t myBrightness = (myRgbBuffer[myPixelIndex]     +
                                myRgbBuffer[myPixelIndex + 1] +
                                myRgbBuffer[myPixelIndex + 2]) / 3;
        if (myBrightness > 100) u8g2.drawPixel(myOledX, myOledY);
      }
    }
    if (imageCount >= 0) {
      u8g2.setFont(u8g2_font_ncenB10_tr);
      u8g2.setColorIndex(0);
      u8g2.drawBox(0, 0, 20, 15);
      u8g2.setColorIndex(1);
      u8g2.setCursor(3, 10);
      u8g2.print(String(imageCount));
    } else {
      u8g2.setFont(u8g2_font_5x7_tf);
      u8g2.setColorIndex(0);
      u8g2.drawBox(50, 0, 22, 8);
      u8g2.setColorIndex(1);
      u8g2.drawStr(52, 7, "LIVE");
    }
  } while (u8g2.nextPage());
}

void myDisplayImageOnOLED(camera_fb_t* fb, int imageCount) {
  if (!myRgbBuffer) return;
  if (!fmt2rgb888(fb->buf, fb->len, fb->format, myRgbBuffer)) {
    Serial.println("Failed to convert JPEG to RGB888 for OLED");
    return;
  }
  myRenderRgbToOLED(imageCount);
}


// =============================================================================
//  TOUCH INPUT
// =============================================================================
int myReadTouch() {
  int sum = 0;
  for (int i = 0; i < 3; i++) {
    sum += analogRead(A0);
    delayMicroseconds(100);
  }
  return sum / 3;
}

void myResetTouchState() {
  myTouch.isTouching = false;
  myTouch.tapCount = 0;
  myTouch.firstTapTime = 0;
  myTouch.lastReleaseTime = 0;
  myTouch.lastCheckTime = 0;
}

void myUpdateTouchState() {
  unsigned long now = millis();
  if (now - myTouch.lastCheckTime < 20) return;
  myTouch.lastCheckTime = now;

  int val = myReadTouch();
  bool touchActive = myTouch.isTouching ? (val > myThresholdRelease) : (val > myThresholdPress);

  if (touchActive && !myTouch.isTouching) {
    if (now - myTouch.lastReleaseTime < myDebounceMs) return;
    myTouch.isTouching = true;

    if (myTouch.tapCount == 0 || (now - myTouch.firstTapTime < myTapWindow)) {
      if (myTouch.tapCount == 0) myTouch.firstTapTime = now;
      myTouch.tapCount++;
      Serial.printf("Tap #%d\n", myTouch.tapCount);
    } else {
      myTouch.tapCount = 1;
      myTouch.firstTapTime = now;
      Serial.println("Tap #1 (new window)");
    }
  }

  if (!touchActive && myTouch.isTouching) {
    myTouch.isTouching = false;
    myTouch.lastReleaseTime = now;
  }
}

int myCheckTouchInput() {
  myUpdateTouchState();
  unsigned long now = millis();

  if (myTouch.tapCount > 0 && !myTouch.isTouching) {
    if (now - myTouch.firstTapTime > myTapWindow) {
      int result = (myTouch.tapCount >= myLongPressTaps) ? 2 : 1;
      int count = myTouch.tapCount;
      myResetTouchState();
      if (result == 2) Serial.printf("LONG PRESS detected (%d taps)\n", count);
      else             Serial.printf("TAP detected (%d tap%s)\n", count, count > 1 ? "s" : "");
      return result;
    }
  }
  return 0;
}

void myCheckTouchBackground() {
  myUpdateTouchState();
}

int myPeekTouchAction() {
  myUpdateTouchState();
  unsigned long now = millis();
  if (myTouch.tapCount > 0 && !myTouch.isTouching) {
    if (now - myTouch.firstTapTime > myTapWindow) {
      return (myTouch.tapCount >= myLongPressTaps) ? 2 : 1;
    }
  }
  return 0;
}


// =============================================================================
//  MEMORY
// =============================================================================
void myAllocateMemory() {
  if (myInputBuffer != nullptr) return;

  Serial.println("\n=== Allocating inference memory ===");

  myInputBuffer = (float*)ps_malloc(INPUT_SIZE * INPUT_SIZE * 3 * sizeof(float));
  myConv1_w  = (float*)ps_malloc(CONV1_WEIGHTS * sizeof(float));
  myConv1_b  = (float*)ps_malloc(CONV1_FILTERS * sizeof(float));
  myConv2_w  = (float*)ps_malloc(CONV2_WEIGHTS * sizeof(float));
  myConv2_b  = (float*)ps_malloc(CONV2_FILTERS * sizeof(float));
  myOutput_w = (float*)ps_malloc(OUTPUT_WEIGHTS * sizeof(float));
  myOutput_b = (float*)ps_malloc(NUM_CLASSES * sizeof(float));

  myConv1_output = (float*)ps_malloc(CONV1_OUT_FLOATS * sizeof(float));
  myPool1_output = (float*)ps_malloc(POOL1_OUT_FLOATS * sizeof(float));
  myConv2_output = (float*)ps_malloc(FLATTENED_SIZE * sizeof(float));
  myDense_output = (float*)ps_malloc(NUM_CLASSES * sizeof(float));

  if (!myInputBuffer || !myConv1_w || !myConv1_b || !myConv2_w || !myConv2_b ||
      !myOutput_w || !myOutput_b || !myConv1_output || !myPool1_output ||
      !myConv2_output || !myDense_output) {
    Serial.println("FATAL: PSRAM allocation failed! Even inference does not fit.");
    myShowMessage("PSRAM ERROR!", "Model too big", "or no PSRAM");
    while (1) { delay(1000); }
  }

  Serial.printf("Free PSRAM after inference allocation: %d bytes\n", ESP.getFreePsram());

  float c1std = sqrt(2.0 / (9.0 * 3));
  for (int i = 0; i < CONV1_WEIGHTS; i++) myConv1_w[i] = ((float)rand() / RAND_MAX - 0.5f) * 2.0f * c1std;
  for (int i = 0; i < CONV1_FILTERS; i++) myConv1_b[i] = 0;

  float c2std = sqrt(2.0 / (double)CONV2_IN_STRIDE);
  for (int i = 0; i < CONV2_WEIGHTS; i++) myConv2_w[i] = ((float)rand() / RAND_MAX - 0.5f) * 2.0f * c2std;
  for (int i = 0; i < CONV2_FILTERS; i++) myConv2_b[i] = 0;

  float dstd = sqrt(2.0 / FLATTENED_SIZE);
  for (int i = 0; i < OUTPUT_WEIGHTS; i++) myOutput_w[i] = ((float)rand() / RAND_MAX - 0.5f) * 2.0f * dstd;
  for (int i = 0; i < NUM_CLASSES; i++) myOutput_b[i] = 0;
  Serial.println("Random starting weights set");
}

size_t myTrainingBytesNeeded() {
  size_t floats = (size_t)(CONV1_WEIGHTS + CONV1_FILTERS + CONV2_WEIGHTS + CONV2_FILTERS + OUTPUT_WEIGHTS + NUM_CLASSES) * 3;
  floats += (size_t)FLATTENED_SIZE * 2 + POOL1_OUT_FLOATS + CONV1_OUT_FLOATS;
  return floats * sizeof(float);
}

bool myAllocateTrainingBuffers() {
  if (myTrainingBuffersReady) return true;

  size_t need = myTrainingBytesNeeded();
  if (ESP.getFreePsram() < need + 32768) {
    Serial.printf("Training needs about %u KB of PSRAM but only %u KB is free.\n",
                  (unsigned)(need / 1024), (unsigned)(ESP.getFreePsram() / 1024));
    return false;
  }

  MyBufSpec specs[] = {
    { &myConv1_w_grad,  CONV1_WEIGHTS,  false }, { &myConv1_b_grad,  CONV1_FILTERS, false },
    { &myConv2_w_grad,  CONV2_WEIGHTS,  false }, { &myConv2_b_grad,  CONV2_FILTERS, false },
    { &myOutput_w_grad, OUTPUT_WEIGHTS, false }, { &myOutput_b_grad, NUM_CLASSES,   false },
    { &myConv1_w_m, CONV1_WEIGHTS,  true }, { &myConv1_w_v, CONV1_WEIGHTS,  true },
    { &myConv1_b_m, CONV1_FILTERS,  true }, { &myConv1_b_v, CONV1_FILTERS,  true },
    { &myConv2_w_m, CONV2_WEIGHTS,  true }, { &myConv2_w_v, CONV2_WEIGHTS,  true },
    { &myConv2_b_m, CONV2_FILTERS,  true }, { &myConv2_b_v, CONV2_FILTERS,  true },
    { &myOutput_w_m, OUTPUT_WEIGHTS, true }, { &myOutput_w_v, OUTPUT_WEIGHTS, true },
    { &myOutput_b_m, NUM_CLASSES,    true }, { &myOutput_b_v, NUM_CLASSES,    true },
    { &myDense_grad, FLATTENED_SIZE,    false },
    { &myConv2_grad, FLATTENED_SIZE,    false },
    { &myPool1_grad, POOL1_OUT_FLOATS,  false },
    { &myConv1_grad, CONV1_OUT_FLOATS,  false }
  };
  const int n = sizeof(specs) / sizeof(specs[0]);

  bool ok = true;
  for (int i = 0; i < n; i++) {
    *specs[i].ptr = specs[i].zero ? (float*)ps_calloc(specs[i].count, sizeof(float))
                                  : (float*)ps_malloc(specs[i].count * sizeof(float));
    if (!*specs[i].ptr) { ok = false; break; }
  }
  if (!ok) {
    Serial.println("Training buffer allocation failed - freeing what was allocated");
    for (int i = 0; i < n; i++) {
      if (*specs[i].ptr) { free(*specs[i].ptr); *specs[i].ptr = nullptr; }
    }
    return false;
  }

  myTrainingBuffersReady = true;
  Serial.printf("Training buffers allocated. Free PSRAM: %u KB\n", (unsigned)(ESP.getFreePsram() / 1024));
  return true;
}

void myResetAdam() {
  memset(myConv1_w_m, 0, CONV1_WEIGHTS * sizeof(float));  memset(myConv1_w_v, 0, CONV1_WEIGHTS * sizeof(float));
  memset(myConv1_b_m, 0, CONV1_FILTERS * sizeof(float));  memset(myConv1_b_v, 0, CONV1_FILTERS * sizeof(float));
  memset(myConv2_w_m, 0, CONV2_WEIGHTS * sizeof(float));  memset(myConv2_w_v, 0, CONV2_WEIGHTS * sizeof(float));
  memset(myConv2_b_m, 0, CONV2_FILTERS * sizeof(float));  memset(myConv2_b_v, 0, CONV2_FILTERS * sizeof(float));
  memset(myOutput_w_m, 0, OUTPUT_WEIGHTS * sizeof(float)); memset(myOutput_w_v, 0, OUTPUT_WEIGHTS * sizeof(float));
  memset(myOutput_b_m, 0, NUM_CLASSES * sizeof(float));   memset(myOutput_b_v, 0, NUM_CLASSES * sizeof(float));
}


// =============================================================================
//  SAVING AND LOADING WEIGHTS
// =============================================================================
static void myDumpArray(File& file, const char* name, const float* data, int size) {
  file.printf("const float %s[] = { ", name);
  for (int i = 0; i < size; i++) {
    file.print(data[i], 6);
    file.print("f");
    if (i < size - 1) file.print(", ");
    if ((i + 1) % 8 == 0) file.println();
  }
  file.println(" };");
}

void myExportHeader() {
  if (!mySDavailable) {
    Serial.println("No SD card - cannot export header");
    return;
  }
  if (!SD.exists("/header")) SD.mkdir("/header");
  File file = SD.open("/header/myWeights.h", FILE_WRITE);
  if (!file) return;
  file.println("#ifndef MY_MODEL_H\n#define MY_MODEL_H");
  file.println("// ======================================================");
  file.println("// After copying this file to your sketch folder, make the");
  file.println("// following lines in the main sketch match the training:");
  file.println("//");
  file.printf( "//   #define INPUT_SIZE %d\n", INPUT_SIZE);
  file.printf( "//   #define CONV1_FILTERS %d\n", CONV1_FILTERS);
  file.printf( "//   #define CONV2_FILTERS %d\n", CONV2_FILTERS);
  file.printf( "//   #define NUM_CLASSES %d\n", NUM_CLASSES);
  file.print(  "//   String myClassLabels[NUM_CLASSES] = {");
  for (int i = 0; i < NUM_CLASSES; i++) {
    file.printf("\"%s\"", myClassLabels[i].c_str());
    if (i < NUM_CLASSES - 1) file.print(", ");
  }
  file.println("};");
  file.println("//");
  file.println("// Then remove the // in front of:  #define USE_BAKED_WEIGHTS");
  file.println("// ======================================================");
  myDumpArray(file, "myModel_conv1_w",  myConv1_w,  CONV1_WEIGHTS);
  myDumpArray(file, "myModel_conv1_b",  myConv1_b,  CONV1_FILTERS);
  myDumpArray(file, "myModel_conv2_w",  myConv2_w,  CONV2_WEIGHTS);
  myDumpArray(file, "myModel_conv2_b",  myConv2_b,  CONV2_FILTERS);
  myDumpArray(file, "myModel_output_w", myOutput_w, OUTPUT_WEIGHTS);
  myDumpArray(file, "myModel_output_b", myOutput_b, NUM_CLASSES);
  file.println("#endif");
  file.close();
  Serial.println("Wrote /header/myWeights.h");
}

bool myLoadWeights() {
  if (!mySDavailable) {
    Serial.println("No SD card - skipping weight load");
    return false;
  }
  if (!SD.exists("/header/myWeights.bin")) {
    Serial.println("No SD weights file found");
    return false;
  }
  Serial.println("Loading weights from SD...");
  File f = SD.open("/header/myWeights.bin", FILE_READ);
  if (!f) return false;

  if ((size_t)f.size() != MY_EXPECTED_WEIGHT_BYTES) {
    Serial.printf("REFUSED myWeights.bin: file is %u bytes but this sketch needs %u bytes\n",
                  (unsigned)f.size(), (unsigned)MY_EXPECTED_WEIGHT_BYTES);
    f.close();
    return false;
  }

  f.read((uint8_t*)myConv1_w,  CONV1_WEIGHTS * 4);
  f.read((uint8_t*)myConv1_b,  CONV1_FILTERS * 4);
  f.read((uint8_t*)myConv2_w,  CONV2_WEIGHTS * 4);
  f.read((uint8_t*)myConv2_b,  CONV2_FILTERS * 4);
  f.read((uint8_t*)myOutput_w, OUTPUT_WEIGHTS * 4);
  f.read((uint8_t*)myOutput_b, NUM_CLASSES * 4);
  f.close();
  Serial.println("Weights loaded successfully");
  myWeightsTrained = true;
  return true;
}

void mySaveWeights() {
  if (!mySDavailable) {
    Serial.println("No SD card - cannot save weights");
    return;
  }
  if (!SD.exists("/header")) SD.mkdir("/header");
  File f = SD.open("/header/myWeights.bin", FILE_WRITE);
  if (f) {
    f.write((uint8_t*)myConv1_w,  CONV1_WEIGHTS * 4);
    f.write((uint8_t*)myConv1_b,  CONV1_FILTERS * 4);
    f.write((uint8_t*)myConv2_w,  CONV2_WEIGHTS * 4);
    f.write((uint8_t*)myConv2_b,  CONV2_FILTERS * 4);
    f.write((uint8_t*)myOutput_w, OUTPUT_WEIGHTS * 4);
    f.write((uint8_t*)myOutput_b, NUM_CLASSES * 4);
    f.close();
    Serial.println("Weights saved to SD");
  }
  myExportHeader();
}


// =============================================================================
//  CONFIG.JSON HELPERS
// =============================================================================
static bool myJsonInt(const String& t, const char* key, int& out) {
  String k("\"");
  k += key;
  k += "\"";
  int p = t.indexOf(k.c_str());
  if (p < 0) return false;
  p = t.indexOf(':', p);
  if (p < 0) return false;
  p++;
  int len = (int)t.length();
  while (p < len && (t[p] == ' ' || t[p] == '\n' || t[p] == '\r' || t[p] == '\t')) p++;
  bool neg = false;
  if (p < len && t[p] == '-') { neg = true; p++; }
  if (p >= len || t[p] < '0' || t[p] > '9') return false;
  long v = 0;
  while (p < len && t[p] >= '0' && t[p] <= '9') { v = v * 10 + (t[p] - '0'); p++; }
  out = (int)(neg ? -v : v);
  return true;
}

static int myJsonStringArray(const String& t, const char* key, std::vector<String>& out) {
  String k("\"");
  k += key;
  k += "\"";
  int p = t.indexOf(k.c_str());
  if (p < 0) return -1;
  p = t.indexOf('[', p);
  if (p < 0) return -1;
  p++;
  int len = (int)t.length();
  out.clear();
  while (p < len) {
    char c = t[p];
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == ',') { p++; continue; }
    if (c == ']') break;
    if (c != '"') return -1;
    p++;
    String s("");
    while (p < len && t[p] != '"') {
      if (t[p] == '\\' && p + 1 < len) p++;
      s += t[p];
      p++;
    }
    if (p >= len) return -1;
    p++;
    out.push_back(s);
  }
  return (int)out.size();
}

void myLoadConfig() {
  if (!mySDavailable) return;
  if (!SD.exists("/header/config.json")) {
    Serial.println("No /header/config.json - using compiled labels");
    return;
  }
  File f = SD.open("/header/config.json", FILE_READ);
  if (!f) return;
  size_t sz = f.size();
  if (sz == 0 || sz > 4096) {
    f.close();
    return;
  }
  String txt;
  txt.reserve(sz + 1);
  while (f.available()) txt += (char)f.read();
  f.close();

  std::vector<String> names;
  int n = myJsonStringArray(txt, "classes", names);
  if (n == NUM_CLASSES) {
    for (int i = 0; i < NUM_CLASSES; i++) {
      myClassLabels[i] = names[i];
    }
  }
}


// =============================================================================
//  DEBUG FRAMES
// =============================================================================
bool myHandleDebugChar(char c) {
  if (myLkHandleChar(c)) return true;
  if (c == 'D') {
    if (!myDebugStream) Serial.println("Debug frames ON");
    myDebugStream = true;
    myDebugLastSeen = millis();
    return true;
  }
  if (c == 'd') {
    if (myDebugStream) Serial.println("Debug frames OFF");
    myDebugStream = false;
    return true;
  }
  return false;
}

static void myPrintB64(const uint8_t* d, size_t n) {
  unsigned char out[520];
  while (n > 0) {
    size_t take = n > 384 ? 384 : n;
    size_t ol = 0;
    mbedtls_base64_encode(out, sizeof(out), &ol, d, take);
    Serial.write(out, ol);
    d += take;
    n -= take;
  }
}

void myDebugSendFrame(char kind, int n, camera_fb_t* fb, int pred, const float* logits) {
  if (!myDebugStream || !fb) return;
  if (millis() - myDebugLastSeen > 15000) { myDebugStream = false; return; }
  const bool inf = (kind == 'I' && logits != nullptr);
  const int hn = CONV2_OUTPUT_SIZE * CONV2_OUTPUT_SIZE;
  size_t cap = 512 + (size_t)NUM_CLASSES * 48 + ((size_t)hn / 3 + 2) * 4 + ((fb->len + 2) / 3) * 4;
  char* b = (char*)ps_malloc(cap);
  if (!b) return;
  size_t o = 0;
  o += snprintf(b + o, cap - o, "@F %c %d %d ", kind, n, pred);
  if (inf) {
    for (int i = 0; i < NUM_CLASSES; i++) o += snprintf(b + o, cap - o, "%s%.4f", i ? "," : "", myDense_output[i]);
    o += snprintf(b + o, cap - o, " ");
    for (int i = 0; i < NUM_CLASSES; i++) o += snprintf(b + o, cap - o, "%s%.4f", i ? "," : "", logits[i]);
  } else {
    o += snprintf(b + o, cap - o, "- -");
  }
  o += snprintf(b + o, cap - o, " %dx%dx%d ", INPUT_SIZE, CONV1_FILTERS, CONV2_FILTERS);
  if (inf) {
    int c = ((INPUT_SIZE / 2) * INPUT_SIZE + INPUT_SIZE / 2) * 3;
    o += snprintf(b + o, cap - o, "%.4f,%.4f,%.4f", myInputBuffer[c], myInputBuffer[c + 1], myInputBuffer[c + 2]);
  } else {
    o += snprintf(b + o, cap - o, "-");
  }
  o += snprintf(b + o, cap - o, " ");
  if (inf) {
    static uint8_t heat[CONV2_OUTPUT_SIZE * CONV2_OUTPUT_SIZE];
    float lo = 1e30f, hi = -1e30f;
    for (int i = 0; i < hn; i++) {
      float m = myConv2_output[i];
      for (int f = 1; f < CONV2_FILTERS; f++) { float v = myConv2_output[f * hn + i]; if (v > m) m = v; }
      if (m < lo) lo = m;
      if (m > hi) hi = m;
    }
    float span = hi - lo;
    if (span < 1e-9f) span = 1.0f;
    for (int i = 0; i < hn; i++) {
      float m = myConv2_output[i];
      for (int f = 1; f < CONV2_FILTERS; f++) { float v = myConv2_output[f * hn + i]; if (v > m) m = v; }
      heat[i] = (uint8_t)(255.0f * (m - lo) / span + 0.5f);
    }
    o += snprintf(b + o, cap - o, "%d ", CONV2_OUTPUT_SIZE);
    o += myB64(b + o, cap - o, heat, hn);
  } else {
    o += snprintf(b + o, cap - o, "0 -");
  }
  o += snprintf(b + o, cap - o, " ");
  o += myB64(b + o, cap - o, fb->buf, fb->len);
  b[o] = 0;
  myLkEmit(b, o);
  free(b);
}


// =============================================================================
//  DEVICE LINK (WiFi / USB Protocol)
// =============================================================================
size_t myB64(char* dst, size_t cap, const uint8_t* d, size_t n) {
  size_t ol = 0;
  mbedtls_base64_encode((unsigned char*)dst, cap, &ol, d, n);
  return ol;
}

void myLkWifiBegin() {
#if MY_USE_WIFI
  bool joined = false;
  if (strlen(myWifiSsid) > 0) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(myWifiSsid, myWifiPass);
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < myWifiTimeoutMs) { delay(250); }
    joined = (WiFi.status() == WL_CONNECTED);
  }
  if (joined) {
    Serial.print("WiFi connected. Page address: ws://"); Serial.print(WiFi.localIP()); Serial.println(":81");
  } else {
    if (strlen(myWifiSsid) > 0) { WiFi.disconnect(true); delay(100); }
    WiFi.mode(WIFI_AP);
    delay(100);
    WiFi.softAP(myApSsid, myApPass);
  }
  WiFi.setSleep(false);
  if (MDNS.begin(myHostName)) {}
  myWs.onEvent(myLkWsEvent);
  myWs.begin();
  myWifiUp = true;
#endif
}

void myLkPoll() {
#if MY_USE_WIFI
  if (myWifiUp) myWs.loop();
#endif
  myBleService();
  myLkServeSnap();
}

void myLkEmit(const char* s, size_t n) {
  Serial.write((const uint8_t*)s, n);
  Serial.println();
#if MY_USE_WIFI
  if (myWifiUp && myWsClient >= 0) myWs.sendTXT((uint8_t)myWsClient, s, n);
#endif
#if MY_USE_BLE
  if (myBleConnected && myBleTx) myBleSend(s, n);
#endif
}

void myLkReply(const String& s) {
  myLkEmit(s.c_str(), s.length());
}

bool myLkHandleChar(char c) {
  if (!myLkCapture) {
    if (c == '@') { myLkCapture = true; myLkLine = "@"; return true; }
    return false;
  }
  if (c == '\r') return true;
  if (c == '\n') { myLkCapture = false; myLkHandleLine(myLkLine); myLkLine = ""; return true; }
  if (myLkLine.length() < 2000) myLkLine += c;
  return true;
}

int myLkHex(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return 0;
}

void myLkHandleLine(const String& line) {
  if (line.startsWith("@model ")) {
    int sp = line.indexOf(' ', 7);
    if (sp < 0) { myLkReply("E,bad @model line"); return; }
    size_t n = (size_t)line.substring(7, sp).toInt();
    if (n != MY_EXPECTED_WEIGHT_BYTES) {
      myLkReply("E,size mismatch");
      return;
    }
    if (myLkBuf) { free(myLkBuf); myLkBuf = nullptr; }
    myLkBuf = (uint8_t*)ps_malloc(n);
    if (!myLkBuf) { myLkReply("E,not enough PSRAM"); return; }
    myLkTotal = n; myLkGot = 0; myLkActive = true;
    myLkSum = (uint32_t)strtoul(line.substring(sp + 1).c_str(), NULL, 10);
    myLkReply("A,0");
    return;
  }
  if (line.startsWith("@h")) {
    if (!myLkActive) { myLkReply("E,no transfer"); return; }
    int c1 = line.indexOf(','), c2 = (c1 > 0) ? line.indexOf(',', c1 + 1) : -1;
    if (c1 < 0 || c2 < 0) { myLkReply("A," + String((unsigned)myLkGot)); return; }
    size_t off = (size_t)line.substring(2, c1).toInt();
    int hexLen = c2 - c1 - 1;
    uint8_t tmp[128];
    bool ok = (off == myLkGot) && hexLen > 0 && (hexLen % 2) == 0 && (hexLen / 2) <= (int)sizeof(tmp);
    uint32_t sum = 0;
    int n = 0;
    if (ok) {
      for (int i = 0; i < hexLen; i += 2) {
        tmp[n] = (uint8_t)((myLkHex(line[c1 + 1 + i]) << 4) | myLkHex(line[c1 + 2 + i]));
        sum += tmp[n]; n++;
      }
      ok = (sum == (uint32_t)strtoul(line.substring(c2 + 1).c_str(), NULL, 10));
    }
    if (!ok) { myLkReply("A," + String((unsigned)myLkGot)); return; }
    myLkAppend(tmp, (size_t)n);
    return;
  }
  if (line.startsWith("@stat ")) { myLkStat(line.substring(6)); return; }
  if (line.startsWith("@ls "))   { myLkList(line.substring(4)); return; }
  if (line.startsWith("@read ")) { myLkRead(line.substring(6)); return; }
  if (line.startsWith("@put "))  { myLkPutBegin(line.substring(5)); return; }
  if (line.startsWith("@w"))     { myLkPutChunk(line); return; }
  if (line == "@snap") { myLkSnapWanted = true; return; }
  if (line == "@info") { myLkInfo(); return; }
  if (line.startsWith("@cfg ")) { myLkWriteConfig(line.substring(5)); return; }
  myLkReply("E,unknown command");
}

void myLkAppend(const uint8_t* d, size_t n) {
  if (!myLkActive || !myLkBuf) { myLkReply("E,no transfer"); return; }
  size_t room = myLkTotal - myLkGot;
  if (n > room) n = room;
  memcpy(myLkBuf + myLkGot, d, n);
  myLkGot += n;
  myLkReply("A," + String((unsigned)myLkGot));
  if (myLkGot == myLkTotal) myLkFinish();
}

void myLkFinish() {
  myLkActive = false;
  uint32_t s = 0;
  for (size_t i = 0; i < myLkTotal; i++) s += myLkBuf[i];
  if (s != myLkSum) { free(myLkBuf); myLkBuf = nullptr; myLkReply("E,checksum mismatch"); return; }
  const float* f = (const float*)myLkBuf;
  memcpy(myConv1_w,  f, CONV1_WEIGHTS  * 4); f += CONV1_WEIGHTS;
  memcpy(myConv1_b,  f, CONV1_FILTERS  * 4); f += CONV1_FILTERS;
  memcpy(myConv2_w,  f, CONV2_WEIGHTS  * 4); f += CONV2_WEIGHTS;
  memcpy(myConv2_b,  f, CONV2_FILTERS  * 4); f += CONV2_FILTERS;
  memcpy(myOutput_w, f, OUTPUT_WEIGHTS * 4); f += OUTPUT_WEIGHTS;
  memcpy(myOutput_b, f, NUM_CLASSES    * 4);
  myWeightsTrained = true;
  if (mySDavailable) {
    if (!SD.exists("/header")) SD.mkdir("/header");
    File o = SD.open("/header/myWeights.bin", FILE_WRITE);
    if (o) { o.write(myLkBuf, myLkTotal); o.close(); }
  }
  free(myLkBuf); myLkBuf = nullptr;
  myLkReply("Y,weights loaded");
}

void myLkServeSnap() {
  if (!myLkSnapWanted) return;
  myLkSnapWanted = false;
  if (!myCameraOk) { myLkReply("E,camera not working"); return; }
  camera_fb_t* old = esp_camera_fb_get();
  if (old) esp_camera_fb_return(old);
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) { myLkReply("E,no camera frame"); return; }
  size_t cap = 40 + ((fb->len + 2) / 3) * 4;
  char* b = (char*)ps_malloc(cap);
  if (!b) { esp_camera_fb_return(fb); myLkReply("E,not enough PSRAM"); return; }
  myLkSnapCount++;
  size_t o = snprintf(b, cap, "@J %lu ", myLkSnapCount);
  o += myB64(b + o, cap - o, fb->buf, fb->len);
  b[o] = 0;
  esp_camera_fb_return(fb);
  myLkEmit(b, o);
  free(b);
}

void myLkInfo() {
  String s = String("Y,info|v005|") + String((int)INPUT_SIZE) + "|" + String((int)CONV1_FILTERS) + "|" + String((int)CONV2_FILTERS) + "|";
  for (int i = 0; i < NUM_CLASSES; i++) { if (i) s += ","; s += myClassLabels[i]; }
  s += mySDavailable ? "|sd=1" : "|sd=0";
  s += myWeightsTrained ? "|w=1" : "|w=0";
  myLkReply(s);
}

void myLkWriteConfig(const String& json) {
  if (!mySDavailable) { myLkReply("E,no SD card"); return; }
  if (!SD.exists("/header")) SD.mkdir("/header");
  File f = SD.open("/header/config.json", FILE_WRITE);
  if (!f) { myLkReply("E,cannot open"); return; }
  f.print(json);
  f.close();
  myLoadConfig();
  myLkReply("Y,config saved");
}

bool myLkPathOk(const String& p) {
  if (p.length() < 2 || p.length() > 120 || p.indexOf("..") >= 0) return false;
  return p == "/images" || p == "/header" || p.startsWith("/images/") || p.startsWith("/header/");
}

void myLkStat(String path) {
  path.trim();
  if (!mySDavailable || !myLkPathOk(path) || !SD.exists(path)) { myLkReply("E,not found"); return; }
  File f = SD.open(path, FILE_READ);
  if (!f) { myLkReply("E,cannot open"); return; }
  size_t n = f.isDirectory() ? 0 : (size_t)f.size();
  f.close();
  myLkReply("Y,stat|" + String((unsigned)n));
}

void myLkList(String path) {
  path.trim();
  if (!mySDavailable || !myLkPathOk(path) || !SD.exists(path)) { myLkReply("E,not found"); return; }
  File d = SD.open(path);
  if (!d || !d.isDirectory()) { if (d) d.close(); myLkReply("E,not a folder"); return; }
  int count = 0;
  while (true) {
    File f = d.openNextFile();
    if (!f) break;
    String nm = String(f.name());
    int sl = nm.lastIndexOf('/');
    if (sl >= 0) nm = nm.substring(sl + 1);
    String line = String("@L ") + (f.isDirectory() ? String(-1) : String((unsigned)f.size())) + " " + nm;
    f.close();
    myLkEmit(line.c_str(), line.length());
    count++;
  }
  d.close();
  myLkReply("Y,ls|" + String(count));
}

void myLkRead(const String& args) {
  int s1 = args.indexOf(' ');
  int s2 = (s1 > 0) ? args.indexOf(' ', s1 + 1) : -1;
  if (s1 < 0 || s2 < 0) { myLkReply("E,bad args"); return; }
  size_t off = (size_t)args.substring(0, s1).toInt();
  int len = args.substring(s1 + 1, s2).toInt();
  String path = args.substring(s2 + 1);
  path.trim();
  if (len < 1 || len > 1536 || !mySDavailable || !myLkPathOk(path) || !SD.exists(path)) {
    myLkReply("E,invalid read");
    return;
  }
  File f = SD.open(path, FILE_READ);
  if (!f) { myLkReply("E,cannot open"); return; }
  static uint8_t tmp[1536];
  static char out[2200];
  int got = 0;
  if (f.seek(off)) got = (int)f.read(tmp, (size_t)len);
  f.close();
  if (got <= 0) { myLkReply("E,read error"); return; }
  uint32_t sum = 0;
  for (int i = 0; i < got; i++) sum += tmp[i];
  size_t o = snprintf(out, sizeof(out), "@D %u %lu ", (unsigned)off, (unsigned long)sum);
  o += myB64(out + o, sizeof(out) - o, tmp, (size_t)got);
  out[o] = 0;
  myLkEmit(out, o);
}

void myLkPutBegin(const String& args) {
  int s1 = args.indexOf(' ');
  int s2 = (s1 > 0) ? args.indexOf(' ', s1 + 1) : -1;
  if (s1 < 0 || s2 < 0) { myLkReply("E,bad args"); return; }
  size_t n = (size_t)args.substring(0, s1).toInt();
  uint32_t sum = (uint32_t)strtoul(args.substring(s1 + 1, s2).c_str(), NULL, 10);
  String path = args.substring(s2 + 1);
  path.trim();
  if (!mySDavailable || !path.startsWith("/images/") || !myLkPathOk(path)) {
    myLkReply("E,invalid path");
    return;
  }
  if (myPutBuf) { free(myPutBuf); myPutBuf = nullptr; }
  myPutBuf = (uint8_t*)ps_malloc(n);
  if (!myPutBuf) { myLkReply("E,not enough PSRAM"); return; }
  myPutPath = path; myPutTotal = n; myPutGot = 0; myPutSum = sum; myPutActive = true;
  myLkReply("A,0");
}

void myLkPutChunk(const String& line) {
  if (!myPutActive || !myPutBuf) { myLkReply("E,no upload"); return; }
  int c1 = line.indexOf(',');
  int c2 = (c1 > 0) ? line.indexOf(',', c1 + 1) : -1;
  if (c1 < 0 || c2 < 0) { myLkReply("A," + String((unsigned)myPutGot)); return; }
  size_t off = (size_t)line.substring(2, c1).toInt();
  uint32_t want = (uint32_t)strtoul(line.substring(c1 + 1, c2).c_str(), NULL, 10);
  if (off != myPutGot) { myLkReply("A," + String((unsigned)myPutGot)); return; }
  static uint8_t tmp[1600];
  size_t olen = 0;
  String b64 = line.substring(c2 + 1);
  int rc = mbedtls_base64_decode(tmp, sizeof(tmp), &olen, (const unsigned char*)b64.c_str(), b64.length());
  uint32_t sum = 0;
  for (size_t i = 0; i < olen; i++) sum += tmp[i];
  if (rc != 0 || olen == 0 || sum != want || myPutGot + olen > myPutTotal) {
    myLkReply("A," + String((unsigned)myPutGot));
    return;
  }
  memcpy(myPutBuf + myPutGot, tmp, olen);
  myPutGot += olen;
  myLkReply("A," + String((unsigned)myPutGot));
  if (myPutGot == myPutTotal) myLkPutFinish();
}

void myLkPutFinish() {
  myPutActive = false;
  uint32_t s = 0;
  for (size_t i = 0; i < myPutTotal; i++) s += myPutBuf[i];
  if (s != myPutSum) { free(myPutBuf); myPutBuf = nullptr; myLkReply("E,checksum mismatch"); return; }
  if (!SD.exists("/images")) SD.mkdir("/images");
  String folder = myPutPath.substring(0, myPutPath.lastIndexOf('/'));
  if (!SD.exists(folder)) SD.mkdir(folder);
  File f = SD.open(myPutPath, FILE_WRITE);
  if (!f) { free(myPutBuf); myPutBuf = nullptr; myLkReply("E,cannot write"); return; }
  f.write(myPutBuf, myPutTotal);
  f.close();
  free(myPutBuf); myPutBuf = nullptr;
  myLkReply("Y,saved " + myPutPath);
}

void myKeyPush(char c) {
  uint8_t next = (uint8_t)((myKeyHead + 1) % 16);
  if (next == myKeyTail) return;
  myKeyQueue[myKeyHead] = c;
  myKeyHead = next;
}

bool myKeyAvailable() {
  return (myKeyHead != myKeyTail) || (Serial.available() > 0);
}

char myKeyRead() {
  bool serialFirst = myLkCapture && (Serial.available() > 0);
  if (myKeyHead != myKeyTail && !serialFirst) {
    char c = myKeyQueue[myKeyTail];
    myKeyTail = (uint8_t)((myKeyTail + 1) % 16);
    return c;
  }
  return (char)Serial.read();
}


// =============================================================================
//  BLUETOOTH (v004)
// =============================================================================
#if MY_USE_BLE
void MyBleServerCb::onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) {
  myBleConnected = true;
  myBleMtu = 23;
  myBleRxHead = 0; myBleRxTail = 0;
}
void MyBleServerCb::onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) {
  myBleConnected = false;
  myDebugStream = false;
  NimBLEDevice::startAdvertising();
}
void MyBleServerCb::onMTUChange(uint16_t mtu, NimBLEConnInfo& connInfo) {
  myBleMtu = mtu;
}
void MyBleRxCb::onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) {
  NimBLEAttValue v = pCharacteristic->getValue();
  const uint8_t* d = v.data();
  size_t n = v.length();
  for (size_t i = 0; i < n; i++) {
    uint16_t next = (uint16_t)((myBleRxHead + 1) % MY_BLE_RING);
    if (next == myBleRxTail) break;
    myBleRxRing[myBleRxHead] = d[i];
    myBleRxHead = next;
  }
}
#endif

void myLkBleBegin() {
#if MY_USE_BLE
  NimBLEDevice::init(myBleName);
  NimBLEDevice::setMTU(247);
  myBleServer = NimBLEDevice::createServer();
  myBleServer->setCallbacks(new MyBleServerCb());
  NimBLEService* svc = myBleServer->createService(MY_BLE_SERVICE_UUID);
  myBleTx = svc->createCharacteristic(MY_BLE_TX_UUID, NIMBLE_PROPERTY::NOTIFY);
  NimBLECharacteristic* rx = svc->createCharacteristic(MY_BLE_RX_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  rx->setCallbacks(new MyBleRxCb());
  svc->start();
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(MY_BLE_SERVICE_UUID);
  adv->enableScanResponse(true);
  adv->start();
#endif
}

void myBleService() {
#if MY_USE_BLE
  while (myBleRxTail != myBleRxHead) {
    char c = (char)myBleRxRing[myBleRxTail];
    myBleRxTail = (uint16_t)((myBleRxTail + 1) % MY_BLE_RING);
    myBleFeed(c);
  }
#endif
}

void myBleFeed(char c) {
#if MY_USE_BLE
  if (c == '\r') return;
  if (c != '\n') { if (myBleRxLine.length() < 2100) myBleRxLine += c; return; }
  String t = myBleRxLine;
  myBleRxLine = "";
  if (t.length() == 0) return;
  if (t[0] == '@') { myLkHandleLine(t); return; }
  for (unsigned i = 0; i < t.length(); i++) {
    char k = t[i];
    if (k == 'D' || k == 'd') myHandleDebugChar(k);
    else if (k > ' ') myKeyPush(k);
  }
#endif
}

void myBleSend(const char* s, size_t n) {
#if MY_USE_BLE
  size_t chunk = (myBleMtu > 23) ? (size_t)(myBleMtu - 3) : 20;
  if (chunk > 240) chunk = 240;
  static const uint8_t myNewline = '\n';
  size_t o = 0;
  while (o <= n) {
    size_t take;
    if (o == n) { myBleTx->setValue(&myNewline, 1); take = 1; }
    else { take = (n - o > chunk) ? chunk : (n - o); myBleTx->setValue((const uint8_t*)s + o, take); }
    int tries = 0;
    while (!myBleTx->notify() && tries++ < 200) delay(3);
    o += take;
    delay(2);
  }
#endif
}

#if MY_USE_WIFI
void myLkWsEvent(uint8_t num, WStype_t type, uint8_t* p, size_t len) {
  if (type == WStype_CONNECTED) {
    myWsClient = num;
    myLkReply("Y,hello");
  } else if (type == WStype_DISCONNECTED) {
    if (myWsClient == (int)num) { myWsClient = -1; myDebugStream = false; }
  } else if (type == WStype_TEXT) {
    if (len == 1 && (p[0] == 'D' || p[0] == 'd')) {
      myHandleDebugChar((char)p[0]);
    } else if (len > 0 && p[0] == '@') {
      String t; t.reserve(len);
      for (size_t i = 0; i < len; i++) t += (char)p[i];
      myLkHandleLine(t);
    } else {
      for (size_t i = 0; i < len; i++) if (p[i] > ' ') myKeyPush((char)p[i]);
    }
  } else if (type == WStype_BIN) {
    myLkAppend(p, len);
  }
}
#endif


// =============================================================================
//  IMAGES
// =============================================================================
void myResizeToInput(const uint8_t* rgb240, float* dst) {
  for (int y = 0; y < INPUT_SIZE; y++) {
    int rowOffset = myResizeLut[y] * 240;
    for (int x = 0; x < INPUT_SIZE; x++) {
      int srcIdx = (rowOffset + myResizeLut[x]) * 3;
      int dstIdx = (y * INPUT_SIZE + x) * 3;
      dst[dstIdx]     = rgb240[srcIdx]     * 0.003921569f;
      dst[dstIdx + 1] = rgb240[srcIdx + 1] * 0.003921569f;
      dst[dstIdx + 2] = rgb240[srcIdx + 2] * 0.003921569f;
    }
  }
}

bool myLoadImageFromFile(const char* path, float* buf) {
  File f = SD.open(path);
  if (!f) return false;

  size_t sz = f.size();
  uint8_t* jpg = (uint8_t*)ps_malloc(sz);
  if (!jpg) { f.close(); return false; }
  f.read(jpg, sz);
  f.close();

  if (!myRgbBuffer) { free(jpg); return false; }
  bool ok = fmt2rgb888(jpg, sz, PIXFORMAT_JPEG, myRgbBuffer);
  free(jpg);
  if (!ok) return false;

  myResizeToInput(myRgbBuffer, buf);
  return true;
}

String myUniqueImagePath(const String& folder) {
  unsigned long n = millis();
  String p = folder + "/img_" + String(n) + ".jpg";
  while (SD.exists(p)) {
    n++;
    p = folder + "/img_" + String(n) + ".jpg";
  }
  return p;
}


// =============================================================================
//  THE CNN (Forward, Backward, and Optimization Passes)
// =============================================================================
void myForwardPass(float* input, float* logits) {
  // Conv1
  for (int f = 0; f < CONV1_FILTERS; f++) {
    int ob = f * CONV1_OUTPUT_SIZE * CONV1_OUTPUT_SIZE;
    for (int y = 0; y < CONV1_OUTPUT_SIZE; y++) {
      for (int x = 0; x < CONV1_OUTPUT_SIZE; x++) {
        float sum = myConv1_b[f];
        int w_idx = f * CONV1_IN_STRIDE;
        for (int ky = 0; ky < 3; ky++) {
          int iy = y + ky;
          int irow = iy * INPUT_SIZE;
          for (int kx = 0; kx < 3; kx++) {
            int ix = x + kx;
            int src = (irow + ix) * 3;
            sum += input[src]     * myConv1_w[w_idx];
            sum += input[src + 1] * myConv1_w[w_idx + 1];
            sum += input[src + 2] * myConv1_w[w_idx + 2];
            w_idx += 3;
          }
        }
        myConv1_output[ob + y * CONV1_OUTPUT_SIZE + x] = leaky_relu(sum);
      }
    }
  }

  // Max Pooling 2x2
  for (int f = 0; f < CONV1_FILTERS; f++) {
    int src_base = f * CONV1_OUTPUT_SIZE * CONV1_OUTPUT_SIZE;
    int dst_base = f * POOL1_OUTPUT_SIZE * POOL1_OUTPUT_SIZE;
    for (int py = 0; py < POOL1_OUTPUT_SIZE; py++) {
      int sy = py * 2;
      for (int px = 0; px < POOL1_OUTPUT_SIZE; px++) {
        int sx = px * 2;
        float mx = myConv1_output[src_base + sy * CONV1_OUTPUT_SIZE + sx];
        float v1 = myConv1_output[src_base + sy * CONV1_OUTPUT_SIZE + sx + 1];
        float v2 = myConv1_output[src_base + (sy + 1) * CONV1_OUTPUT_SIZE + sx];
        float v3 = myConv1_output[src_base + (sy + 1) * CONV1_OUTPUT_SIZE + sx + 1];
        if (v1 > mx) mx = v1;
        if (v2 > mx) mx = v2;
        if (v3 > mx) mx = v3;
        myPool1_output[dst_base + py * POOL1_OUTPUT_SIZE + px] = mx;
      }
    }
  }

  // Conv2
  for (int f = 0; f < CONV2_FILTERS; f++) {
    int ob = f * CONV2_OUTPUT_SIZE * CONV2_OUTPUT_SIZE;
    for (int y = 0; y < CONV2_OUTPUT_SIZE; y++) {
      for (int x = 0; x < CONV2_OUTPUT_SIZE; x++) {
        float sum = myConv2_b[f];
        int w_idx = f * CONV2_IN_STRIDE;
        for (int ky = 0; ky < 3; ky++) {
          int iy = y + ky;
          int irow = iy * POOL1_OUTPUT_SIZE;
          for (int kx = 0; kx < 3; kx++) {
            int ix = x + kx;
            int p_idx = irow + ix;
            for (int pf = 0; pf < CONV1_FILTERS; pf++) {
              sum += myPool1_output[pf * POOL1_OUTPUT_SIZE * POOL1_OUTPUT_SIZE + p_idx] * myConv2_w[w_idx];
              w_idx++;
            }
          }
        }
        myConv2_output[ob + y * CONV2_OUTPUT_SIZE + x] = leaky_relu(sum);
      }
    }
  }

  // Dense Layer / Output logits
  for (int c = 0; c < NUM_CLASSES; c++) {
    float sum = myOutput_b[c];
    int w_base = c * FLATTENED_SIZE;
    for (int i = 0; i < FLATTENED_SIZE; i++) {
      sum += myConv2_output[i] * myOutput_w[w_base + i];
    }
    logits[c] = sum;
  }

  // Softmax
  float max_logit = logits[0];
  for (int c = 1; c < NUM_CLASSES; c++) if (logits[c] > max_logit) max_logit = logits[c];
  float denom = 0;
  for (int c = 0; c < NUM_CLASSES; c++) {
    myDense_output[c] = exp(clip_value(logits[c] - max_logit, -20.0f, 0.0f));
    denom += myDense_output[c];
  }
  if (denom < 1e-9f) denom = 1e-9f;
  for (int c = 0; c < NUM_CLASSES; c++) myDense_output[c] /= denom;
}

void myBackwardDense(int label) {
  for (int c = 0; c < NUM_CLASSES; c++) {
    float err = myDense_output[c] - (c == label ? 1.0f : 0.0f);
    myOutput_b_grad[c] += err;
    int w_base = c * FLATTENED_SIZE;
    for (int i = 0; i < FLATTENED_SIZE; i++) {
      myOutput_w_grad[w_base + i] += err * myConv2_output[i];
      myDense_grad[i] += err * myOutput_w[w_base + i];
    }
  }
}

void myBackwardConv2() {
  memset(myConv2_grad, 0, FLATTENED_SIZE * sizeof(float));
  int hn = CONV2_OUTPUT_SIZE * CONV2_OUTPUT_SIZE;
  for (int f = 0; f < CONV2_FILTERS; f++) {
    int ob = f * CONV2_OUTPUT_SIZE * CONV2_OUTPUT_SIZE;
    int w_idx = f * CONV2_IN_STRIDE;
    for (int y = 0; y < CONV2_OUTPUT_SIZE; y++) {
      for (int x = 0; x < CONV2_OUTPUT_SIZE; x++) {
        int idx = ob + y * CONV2_OUTPUT_SIZE + x;
        float out_val = myConv2_output[idx];
        float d = myDense_grad[idx] * leaky_relu_deriv(out_val);
        myConv2_b_grad[f] += d;
        for (int ky = 0; ky < 3; ky++) {
          int iy = y + ky;
          int irow = iy * POOL1_OUTPUT_SIZE;
          for (int kx = 0; kx < 3; kx++) {
            int ix = x + kx;
            int p_idx = irow + ix;
            for (int pf = 0; pf < CONV1_FILTERS; pf++) {
              float p_val = myPool1_output[pf * hn + p_idx];
              myConv2_w_grad[w_idx] += d * p_val;
              myPool1_grad[pf * hn + p_idx] += d * myConv2_w[w_idx];
              w_idx++;
            }
          }
        }
      }
    }
  }
}

void myBackwardPool1() {
  int hn = POOL1_OUTPUT_SIZE * POOL1_OUTPUT_SIZE;
  int c1_hn = CONV1_OUTPUT_SIZE * CONV1_OUTPUT_SIZE;
  memset(myPool1_grad, 0, POOL1_OUT_FLOATS * sizeof(float));
  for (int f = 0; f < CONV1_FILTERS; f++) {
    int src_base = f * c1_hn;
    int pool_base = f * hn;
    for (int py = 0; py < POOL1_OUTPUT_SIZE; py++) {
      int sy = py * 2;
      for (int px = 0; px < POOL1_OUTPUT_SIZE; px++) {
        int sx = px * 2;
        int p_idx = pool_base + py * POOL1_OUTPUT_SIZE + px;
        float grad = myPool1_grad[p_idx];
        
        int best_idx = src_base + sy * CONV1_OUTPUT_SIZE + sx;
        float mx = myConv1_output[best_idx];
        int v1_idx = src_base + sy * CONV1_OUTPUT_SIZE + sx + 1;
        int v2_idx = src_base + (sy + 1) * CONV1_OUTPUT_SIZE + sx;
        int v3_idx = src_base + (sy + 1) * CONV1_OUTPUT_SIZE + sx + 1;
        
        if (myConv1_output[v1_idx] > mx) { mx = myConv1_output[v1_idx]; best_idx = v1_idx; }
        if (myConv1_output[v2_idx] > mx) { mx = myConv1_output[v2_idx]; best_idx = v2_idx; }
        if (myConv1_output[v3_idx] > mx) { mx = myConv1_output[v3_idx]; best_idx = v3_idx; }
        
        myConv1_grad[best_idx] += grad;
      }
    }
  }
}

void myBackwardConv1() {
  for (int f = 0; f < CONV1_FILTERS; f++) {
    int ob = f * CONV1_OUTPUT_SIZE * CONV1_OUTPUT_SIZE;
    int w_idx = f * CONV1_IN_STRIDE;
    for (int y = 0; y < CONV1_OUTPUT_SIZE; y++) {
      for (int x = 0; x < CONV1_OUTPUT_SIZE; x++) {
        int idx = ob + y * CONV1_OUTPUT_SIZE + x;
        float d = myConv1_grad[idx] * leaky_relu_deriv(myConv1_output[idx]);
        myConv1_b_grad[f] += d;
        for (int ky = 0; ky < 3; ky++) {
          int iy = y + ky;
          int irow = iy * INPUT_SIZE;
          for (int kx = 0; kx < 3; kx++) {
            int ix = x + kx;
            int src = (irow + ix) * 3;
            myConv1_w_grad[w_idx]     += d * myInputBuffer[src];
            myConv1_w_grad[w_idx + 1] += d * myInputBuffer[src + 1];
            myConv1_w_grad[w_idx + 2] += d * myInputBuffer[src + 2];
            w_idx += 3;
          }
        }
      }
    }
  }
}

void myAdamUpdate(float* w, float* g, float* m, float* v, int size, int step) {
  float lr = LEARNING_RATE;
  float b1 = 0.9f;
  float b2 = 0.999f;
  float eps = 1e-8f;
  float bc1 = 1.0f - pow(b1, step);
  float bc2 = 1.0f - pow(b2, step);

  for (int i = 0; i < size; i++) {
    float grad = g[i];
    m[i] = b1 * m[i] + (1.0f - b1) * grad;
    v[i] = b2 * v[i] + (1.0f - b2) * (grad * grad);
    float m_hat = m[i] / bc1;
    float v_hat = v[i] / bc2;
    w[i] -= lr * m_hat / (sqrt(v_hat) + eps);
    g[i] = 0;
  }
}

void myUpdateWeights(int step) {
  myAdamUpdate(myConv1_w, myConv1_w_grad, myConv1_w_m, myConv1_w_v, CONV1_WEIGHTS, step);
  myAdamUpdate(myConv1_b, myConv1_b_grad, myConv1_b_m, myConv1_b_v, CONV1_FILTERS, step);
  myAdamUpdate(myConv2_w, myConv2_w_grad, myConv2_w_m, myConv2_w_v, CONV2_WEIGHTS, step);
  myAdamUpdate(myConv2_b, myConv2_b_grad, myConv2_b_m, myConv2_b_v, CONV2_FILTERS, step);
  myAdamUpdate(myOutput_w, myOutput_w_grad, myOutput_w_m, myOutput_w_v, OUTPUT_WEIGHTS, step);
  myAdamUpdate(myOutput_b, myOutput_b_grad, myOutput_b_m, myOutput_b_v, NUM_CLASSES, step);
}


// =============================================================================
//  MENU ACTIONS (Collect, Train, Infer)
// =============================================================================
void myActionCollect(int classIdx) {
  if (!mySDavailable || !myCameraOk) {
    myShowMessage("SD/Cam Error");
    delay(2000);
    myResetMenuState();
    return;
  }

  Serial.printf("\n>>> Collection mode: %s\n", myClassLabels[classIdx].c_str());
  myResetTouchState();

  String folder = "/images/" + myClassLabels[classIdx];
  if (!SD.exists("/images")) SD.mkdir("/images");
  if (!SD.exists(folder)) SD.mkdir(folder);

  int imageCount = 0;
  File root = SD.open(folder);
  if (root) {
    while (File file = root.openNextFile()) {
      String fn = String(file.name());
      if (!file.isDirectory() && (fn.endsWith(".jpg") || fn.endsWith(".JPG"))) imageCount++;
      file.close();
    }
    root.close();
  }

  unsigned long lastCameraDrain = 0;
  unsigned long lastOLED = 0;
  bool oledNeedsUpdate = false;
  bool shouldCapture = false;

  while (true) {
    myLkPoll();
    unsigned long now = millis();

    if (now - lastCameraDrain > 50) {
      lastCameraDrain = now;
      if (!shouldCapture) {
        camera_fb_t* fb = esp_camera_fb_get();
        if (fb) {
          if (now - lastOLED > 250 && myRgbBuffer) {
            if (fmt2rgb888(fb->buf, fb->len, fb->format, myRgbBuffer)) {
              oledNeedsUpdate = true;
              lastOLED = now;
            }
          }
          esp_camera_fb_return(fb);
        }
      }
    }

    if (oledNeedsUpdate) {
      oledNeedsUpdate = false;
      myRenderRgbToOLED(-1);
    }

    if (myKeyAvailable()) {
      char c = myKeyRead();
      if (myHandleDebugChar(c)) {
        // Handled
      } else if (c == 'l' || c == 'L') {
        myResetMenuState();
        return;
      } else if (c == 't' || c == 'T') {
        shouldCapture = true;
      }
    }

    int touchAction = myCheckTouchInput();
    if (touchAction == 2) {
      myResetMenuState();
      return;
    } else if (touchAction == 1) {
      shouldCapture = true;
    }

    if (shouldCapture) {
      shouldCapture = false;
      camera_fb_t* fb = esp_camera_fb_get();
      if (fb) {
        String fileName = myUniqueImagePath(folder);
        File file = SD.open(fileName, FILE_WRITE);
        if (file) {
          file.write(fb->buf, fb->len);
          file.close();
          imageCount++;
          myDisplayImageOnOLED(fb, imageCount);
          delay(300);
          lastOLED = millis();
        }
        esp_camera_fb_return(fb);
      }
    }
    delay(5);
  }
}

bool myTrainingExitRequested() {
  if (myKeyAvailable()) {
    char c = myKeyRead();
    if (c == 'x' || c == 'X' || c == 'l' || c == 'L') return true;
  }
  if (myPeekTouchAction() == 2) return true;
  return false;
}

void myActionTrain() {
  if (!mySDavailable) {
    myShowMessage("No SD card");
    delay(2000);
    myResetMenuState();
    return;
  }

  if (!myAllocateTrainingBuffers()) {
    myShowMessage("Not enough RAM", "for training");
    delay(3000);
    myResetMenuState();
    return;
  }

  Serial.println("\n>>> Starting Training on Device <<<");
  myResetAdam();

  myTrainingData.clear();
  for (int c = 0; c < NUM_CLASSES; c++) {
    String folder = "/images/" + myClassLabels[c];
    File root = SD.open(folder);
    if (!root) continue;
    while (File file = root.openNextFile()) {
      String fn = String(file.name());
      if (!file.isDirectory() && (fn.endsWith(".jpg") || fn.endsWith(".JPG"))) {
        myTrainingData.push_back({ folder + "/" + fn, c });
      }
      file.close();
    }
    root.close();
  }

  if (myTrainingData.size() < 2) {
    Serial.println("Not enough images to train!");
    myShowMessage("Need more", "images!");
    delay(3000);
    myResetMenuState();
    return;
  }

  Serial.printf("Found %d training images across %d classes.\n", (int)myTrainingData.size(), NUM_CLASSES);

  int step = 0;
  for (int epoch = 0; epoch < TARGET_EPOCHS; epoch++) {
    if (myTrainingExitRequested()) {
      Serial.println("Training aborted by user.");
      break;
    }

    std::random_shuffle(myTrainingData.begin(), myTrainingData.end());
    float totalLoss = 0;
    int correct = 0;

    for (size_t i = 0; i < myTrainingData.size(); i++) {
      if (!myLoadImageFromFile(myTrainingData[i].path.c_str(), myInputBuffer)) continue;
      
      step++;
      float logits[NUM_CLASSES];
      myForwardPass(myInputBuffer, logits);

      int label = myTrainingData[i].label;
      totalLoss -= log(max(myDense_output[label], 1e-7f));

      int pred = 0;
      float max_p = myDense_output[0];
      for (int c = 1; c < NUM_CLASSES; c++) {
        if (myDense_output[c] > max_p) { max_p = myDense_output[c]; pred = c; }
      }
      if (pred == label) correct++;

      myBackwardDense(label);
      myBackwardConv2();
      myBackwardPool1();
      myBackwardConv1();

      if ((i + 1) % BATCH_SIZE == 0 || i == myTrainingData.size() - 1) {
        myUpdateWeights(step);
      }
    }

    float acc = (float)correct / myTrainingData.size();
    float loss = totalLoss / myTrainingData.size();
    Serial.printf("Epoch %d/%d - Loss: %.4f - Accuracy: %.2f%%\n", epoch + 1, TARGET_EPOCHS, loss, acc * 100.0f);
    
    char buf1[32], buf2[32];
    snprintf(buf1, sizeof(buf1), "Epoch %d/%d", epoch + 1, TARGET_EPOCHS);
    snprintf(buf2, sizeof(buf2), "Acc: %.1f%%", acc * 100.0f);
    myShowMessage("Training...", buf1, buf2);
  }

  myWeightsTrained = true;
  mySaveWeights();
  Serial.println("Training complete! Weights saved.");
  myShowMessage("Training", "Complete!");
  delay(2000);
  myResetMenuState();
}

void myActionInfer() {
  if (!myCameraOk) {
    myShowMessage("Camera error");
    delay(2000);
    myResetMenuState();
    return;
  }
  if (!myWeightsTrained) {
    myShowMessage("Train model", "first!");
    delay(2000);
    myResetMenuState();
    return;
  }

  Serial.println("\n>>> Entering Inference Mode <<<");
  myResetTouchState();

  unsigned long lastInferenceTime = 0;
  while (true) {
    myLkPoll();

    if (myKeyAvailable()) {
      char c = myKeyRead();
      if (myHandleDebugChar(c)) {
        // Handled
      } else if (c == 'l' || c == 'L' || c == 'x' || c == 'X') {
        myResetMenuState();
        return;
      }
    }

    if (myCheckTouchInput() > 0) {
      myResetMenuState();
      return;
    }

    unsigned long now = millis();
    if (now - lastInferenceTime > 100) {
      lastInferenceTime = now;
      camera_fb_t* fb = esp_camera_fb_get();
      if (fb) {
        if (fmt2rgb888(fb->buf, fb->len, fb->format, myRgbBuffer)) {
          myResizeToInput(myRgbBuffer, myInputBuffer);
          float logits[NUM_CLASSES];
          myForwardPass(myInputBuffer, logits);

          int pred = 0;
          float max_p = myDense_output[0];
          for (int c = 1; c < NUM_CLASSES; c++) {
            if (myDense_output[c] > max_p) { max_p = myDense_output[c]; pred = c; }
          }

          u8g2.firstPage();
          do {
            u8g2.setFont(u8g2_font_6x10_tf);
            u8g2.setCursor(0, 10);
            u8g2.print(myClassLabels[pred]);
            u8g2.setCursor(0, 25);
            u8g2.print(String((int)(max_p * 100)) + "%");
          } while (u8g2.nextPage());

          myDebugSendFrame('I', 0, fb, pred, logits);
        }
        esp_camera_fb_return(fb);
      }
    }
    delay(5);
  }
}


// =============================================================================
//  MENU NAVIGATION
// =============================================================================
String myMenuLabel(int item) {
  if (item >= 1 && item <= NUM_CLASSES) return myClassLabels[item - 1];
  if (item == NUM_CLASSES + 1) return "Train Model";
  if (item == NUM_CLASSES + 2) return "Run Inference";
  return "";
}

void myResetMenuState() {
  myIsSelected = false;
  myMenuIndex = 1;
  myResetTouchState();
  myDrawMenu();
}

void myDrawMenu() {
  u8g2.firstPage();
  do {
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.setCursor(0, 10);
    u8g2.print("--- MENU ---");
    u8g2.setCursor(0, 24);
    u8g2.print("> " + myMenuLabel(myMenuIndex));
    u8g2.setCursor(0, 38);
    u8g2.print("(" + String(myMenuIndex) + "/" + String(myTotalItems) + ")");
  } while (u8g2.nextPage());
}

void myExecuteMenuItem(int idx) {
  if (idx >= 1 && idx <= NUM_CLASSES) {
    myActionCollect(idx - 1);
  } else if (idx == NUM_CLASSES + 1) {
    myActionTrain();
  } else if (idx == NUM_CLASSES + 2) {
    myActionInfer();
  }
}

void myHandleMenuNavigation() {
  int action = myCheckTouchInput();
  char key = 0;

  if (myKeyAvailable()) {
    key = myKeyRead();
    if (myHandleDebugChar(key)) key = 0;
  }

  if (action == 1 || key == 't' || key == 'T') {
    myMenuIndex++;
    if (myMenuIndex > myTotalItems) myMenuIndex = 1;
    myDrawMenu();
  } else if (action == 2 || key == 'l' || key == 'L') {
    myExecuteMenuItem(myMenuIndex);
  } else if (key >= '1' && key <= '9') {
    int target = key - '0';
    if (target >= 1 && target <= myTotalItems) {
      myMenuIndex = target;
      myDrawMenu();
    }
  }
}