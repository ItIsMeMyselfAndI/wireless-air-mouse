#include <Arduino.h>
#include <Wire.h>
#include <esp_mac.h>
#define USE_NIMBLE
#include <NimBLEDevice.h>
#include <BleMouse.h>

// ============================================================================
// Board Selection & Pin Mapping (Default: ESP32-C3 Super Mini)
// To switch to ESP32-S3: comment out BOARD_ESP32_C3 and uncomment BOARD_ESP32_S3
// ============================================================================
#define BOARD_ESP32_C3
// #define BOARD_ESP32_S3

#if !defined(BOARD_ESP32_C3) && !defined(BOARD_ESP32_S3)
  #if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(ARDUINO_ESP32S3_DEV)
    #define BOARD_ESP32_S3
  #else
    #define BOARD_ESP32_C3 // Default fallback
  #endif
#endif

#if defined(BOARD_ESP32_S3)
  #define BOARD_NAME     "ESP32-S3 Super Mini"
  #define DEVICE_NAME    "ESP32-S3 Air Mouse"
  #define PIN_SDA        12   // MPU-6050 SDA -> GPIO 12
  #define PIN_SCL        11   // MPU-6050 SCL -> GPIO 11
  #define BTN_SCROLL     5    // Scroll button -> GPIO 5
  #define BTN_LEFT       6    // Left click -> GPIO 6
  #define BTN_RIGHT      7    // Right click -> GPIO 7
  #define PIN_LED_CALIB  1    // Calibrated Green LED -> GPIO 1
  #define PIN_LED_BTN    2    // Button Activity LED -> GPIO 2
  #define LED_PIN        48   // Onboard WS2812 RGB LED -> GPIO 48
  #define HAS_RGB_LED    1
#elif defined(BOARD_ESP32_C3)
  #define BOARD_NAME     "ESP32-C3 Super Mini"
  #define DEVICE_NAME    "ESP32-C3 Air Mouse"
  #define PIN_SDA        3    // MPU-6050 SDA -> GPIO 3
  #define PIN_SCL        2    // MPU-6050 SCL -> GPIO 2
  #define BTN_SCROLL     10   // Scroll button -> GPIO 10
  #define BTN_LEFT       20   // Left click -> GPIO 20
  #define BTN_RIGHT      21   // Right click -> GPIO 21
  #define PIN_LED_CALIB  5    // Calibrated Green LED -> GPIO 5
  #define PIN_LED_BTN    6    // Button Activity LED -> GPIO 6
  #define LED_PIN        8    // Onboard Blue LED (Active-LOW) -> GPIO 8
  #define HAS_RGB_LED    0
#endif

#define MPU_ADDR         0x68

// --- BLE Mouse Instance ---
BleMouse bleMouse(DEVICE_NAME, "Espressif", 100);

// --- Sensor Calibration Offsets ---
float gx_offset = 0.0f;
float gz_offset = 0.0f;
float ax_offset = 0.0f;
float ay_offset = 0.0f;
float az_offset = 0.0f;

// --- Motion & Filter Parameters ---
const float Y_DEADZONE       = 2.0f;  // Deg/s deadband to filter pitch tremor
const float X_DEADZONE       = 1.8f;  // Deg/s deadband to filter yaw tremor
const float ALPHA            = 0.22f; // Low-pass filter smoothing factor
const float SENSITIVITY      = 0.40f; // Cursor sensitivity multiplier
const float SCROLL_THRESHOLD = 8.0f;  // Pitch accumulator threshold per wheel notch

// --- Motion / Scroll State ---
float smoothX = 0.0f;
float smoothY = 0.0f;
float scrollAccumulator = 0.0f;
bool isCalibrated = false;

// --- Button Debounce Structure & Instances ---
const unsigned long DEBOUNCE_DELAY_MS = 30;

struct Button {
  const uint8_t pin;
  int lastReading;
  int stableState;
  unsigned long lastChangeTime;
};

Button btnLeft   = { BTN_LEFT,   HIGH, HIGH, 0 };
Button btnRight  = { BTN_RIGHT,  HIGH, HIGH, 0 };
Button btnScroll = { BTN_SCROLL, HIGH, HIGH, 0 };

// --- Onboard LED Helpers (WS2812 RGB on S3 vs Active-LOW Blue on C3) ---
static inline void setBoardLedOff() {
#if HAS_RGB_LED
  rgbLedWrite(LED_PIN, 0, 0, 0);
#else
  digitalWrite(LED_PIN, HIGH);
#endif
}

static inline void initBoardLed() {
#if HAS_RGB_LED
  rgbLedWrite(LED_PIN, 0, 0, 0);
#else
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);
#endif
}

static inline void setBoardLedCalibrating() {
#if HAS_RGB_LED
  rgbLedWrite(LED_PIN, 60, 0, 0); // Red
#else
  digitalWrite(LED_PIN, LOW);     // ON (Active-LOW)
#endif
}

static inline void setBoardLedCalibrated() {
#if HAS_RGB_LED
  rgbLedWrite(LED_PIN, 0, 60, 0); // Green flash
  delay(500);
  rgbLedWrite(LED_PIN, 0, 0, 0);
#else
  digitalWrite(LED_PIN, HIGH);    // OFF
#endif
}

static inline void updateBoardLedActivity(bool anyPressed) {
#if HAS_RGB_LED
  if (btnLeft.stableState == LOW) {
    rgbLedWrite(LED_PIN, 0, 0, 50);   // Blue: Left click / drag
  } else if (btnRight.stableState == LOW) {
    rgbLedWrite(LED_PIN, 0, 50, 0);   // Green: Right click
  } else if (btnScroll.stableState == LOW) {
    rgbLedWrite(LED_PIN, 50, 25, 0);  // Amber: Scroll mode
  } else {
    rgbLedWrite(LED_PIN, 0, 0, 0);    // OFF
  }
#else
  digitalWrite(LED_PIN, anyPressed ? LOW : HIGH);
#endif
}

// ============================================================================
// Hardware & Sensor Initialization
// ============================================================================

void calibrateSensors() {
  Serial.println("Calibrating... Keep the sensor stationary and level.");

  setBoardLedCalibrating();
  delay(500); // Wait for hand placement/vibration to settle

  const int samples = 250;
  long rawAx = 0, rawAy = 0, rawAz = 0;
  long rawGx = 0, rawGz = 0;

  for (int i = 0; i < samples; i++) {
    Wire.beginTransmission(MPU_ADDR);
    Wire.write(0x3B);
    Wire.endTransmission(false);
    Wire.requestFrom((uint8_t)MPU_ADDR, (uint8_t)14);

    if (Wire.available() >= 14) {
      rawAx += (int16_t)((Wire.read() << 8) | Wire.read());
      rawAy += (int16_t)((Wire.read() << 8) | Wire.read());
      rawAz += (int16_t)((Wire.read() << 8) | Wire.read());
      Wire.read(); Wire.read(); // Skip temp
      rawGx += (int16_t)((Wire.read() << 8) | Wire.read());
      Wire.read(); Wire.read(); // Skip Gyro Y
      rawGz += (int16_t)((Wire.read() << 8) | Wire.read());
    }
    delay(4);
  }

  // +/- 8g range (4096 LSB/g)
  ax_offset = ((float)rawAx / samples) / 4096.0f;
  ay_offset = ((float)rawAy / samples) / 4096.0f;
  az_offset = (((float)rawAz / samples) - 4096.0f) / 4096.0f; // 1g on Z-axis

  // +/- 500 deg/s range (65.5 LSB/(deg/s))
  gx_offset = ((float)rawGx / samples) / 65.5f;
  gz_offset = ((float)rawGz / samples) / 65.5f;

  Serial.println("[SUCCESS] Calibration complete.");

  // Permanently turn ON Calibrated LED
  digitalWrite(PIN_LED_CALIB, HIGH);

  setBoardLedCalibrated();
}

void sleepMPU() {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x6B);
  Wire.write(0x40); // Set SLEEP bit in PWR_MGMT_1
  Wire.endTransmission();
}

void wakeMPU() {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x6B);
  Wire.write(0x00); // Clear SLEEP bit in PWR_MGMT_1
  Wire.endTransmission();
  delay(30);
}

void initMPU() {
  Wire.begin(PIN_SDA, PIN_SCL);

  // Wake up MPU-6050 to configure registers
  wakeMPU();

  // Configure Accelerometer to +/- 8g (0x1C = 0x10)
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x1C);
  Wire.write(0x10);
  Wire.endTransmission();

  // Configure Gyroscope to +/- 500 deg/s (0x1B = 0x08)
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x1B);
  Wire.write(0x08);
  Wire.endTransmission();

  // Put sensor to sleep until a host connects
  sleepMPU();
}

void updateLEDStatus() {
  bool anyButtonPressed = (btnLeft.stableState == LOW) ||
                          (btnRight.stableState == LOW) ||
                          (btnScroll.stableState == LOW);

  // External button indicator LED
  digitalWrite(PIN_LED_BTN, anyButtonPressed ? HIGH : LOW);

  // Onboard status LED (WS2812 RGB on S3 vs Active-LOW Blue on C3)
  updateBoardLedActivity(anyButtonPressed);
}

// ============================================================================
// Handlers
// ============================================================================

bool readMotion(float &pitchRate, float &yawRate) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3B);
  Wire.endTransmission(false);
  Wire.requestFrom((uint8_t)MPU_ADDR, (uint8_t)14);

  if (Wire.available() < 14) return false;

  int16_t rawAx = (Wire.read() << 8) | Wire.read();
  int16_t rawAy = (Wire.read() << 8) | Wire.read();
  int16_t rawAz = (Wire.read() << 8) | Wire.read();
  Wire.read(); Wire.read(); // Skip temp
  int16_t rawGx = (Wire.read() << 8) | Wire.read();
  Wire.read(); Wire.read(); // Skip Gyro Y
  int16_t rawGz = (Wire.read() << 8) | Wire.read();

  // Convert raw gyro to deg/s and remove calibrated offset
  pitchRate = ((float)rawGx / 65.5f) - gx_offset; // Vertical (Pitch)
  yawRate   = ((float)rawGz / 65.5f) - gz_offset; // Horizontal (Yaw)

  // Deadzone suppression
  if (fabs(pitchRate) < Y_DEADZONE) pitchRate = 0.0f;
  if (fabs(yawRate)   < X_DEADZONE) yawRate   = 0.0f;

  // EMA smoothing
  smoothX = (smoothX * (1.0f - ALPHA)) + (yawRate * ALPHA);
  smoothY = (smoothY * (1.0f - ALPHA)) + (pitchRate * ALPHA);

  return true;
}

void handleScrollMode() {
  scrollAccumulator += smoothY;

  if (scrollAccumulator >= SCROLL_THRESHOLD) {
    bleMouse.move(0, 0, 1); // Scroll Up
    Serial.println("[SCROLL] Up");
    scrollAccumulator -= SCROLL_THRESHOLD;
  } else if (scrollAccumulator <= -SCROLL_THRESHOLD) {
    bleMouse.move(0, 0, -1); // Scroll Down
    Serial.println("[SCROLL] Down");
    scrollAccumulator += SCROLL_THRESHOLD;
  }
}

void handleCursorMovement() {
  scrollAccumulator = 0.0f;

  int moveX = (int)(-smoothX * SENSITIVITY);
  int moveY = (int)(-smoothY * SENSITIVITY);

  if (moveX != 0 || moveY != 0) {
    bleMouse.move(moveX, moveY);
  }
}

void handleMotion(bool isScrollMode) {
  float pitch = 0.0f, yaw = 0.0f;
  if (!readMotion(pitch, yaw)) return;

  if (isScrollMode) {
    handleScrollMode();
  } else {
    handleCursorMovement();
  }
}

void handleButtons() {
  unsigned long now = millis();

  // --- Left Button (Click & Drag / Pin 6) ---
  int readingLeft = digitalRead(btnLeft.pin);
  if (readingLeft != btnLeft.lastReading) {
    btnLeft.lastChangeTime = now;
  }
  if ((now - btnLeft.lastChangeTime) > DEBOUNCE_DELAY_MS) {
    if (readingLeft != btnLeft.stableState) {
      btnLeft.stableState = readingLeft;
      if (btnLeft.stableState == LOW) {
        bleMouse.press(MOUSE_LEFT);
        Serial.println("[CLICK] Left Button Pressed");
      } else {
        bleMouse.release(MOUSE_LEFT);
        Serial.println("[RELEASE] Left Button Released");
      }
      updateLEDStatus();
    }
  }
  btnLeft.lastReading = readingLeft;

  // --- Right Button (Click / Pin 7) ---
  int readingRight = digitalRead(btnRight.pin);
  if (readingRight != btnRight.lastReading) {
    btnRight.lastChangeTime = now;
  }
  if ((now - btnRight.lastChangeTime) > DEBOUNCE_DELAY_MS) {
    if (readingRight != btnRight.stableState) {
      btnRight.stableState = readingRight;
      if (btnRight.stableState == LOW) {
        bleMouse.press(MOUSE_RIGHT);
        Serial.println("[CLICK] Right Button Pressed");
      } else {
        bleMouse.release(MOUSE_RIGHT);
        Serial.println("[RELEASE] Right Button Released");
      }
      updateLEDStatus();
    }
  }
  btnRight.lastReading = readingRight;

  // --- Scroll Button / Button 2 (Pin 5) ---
  int readingScroll = digitalRead(btnScroll.pin);
  if (readingScroll != btnScroll.lastReading) {
    btnScroll.lastChangeTime = now;
  }
  if ((now - btnScroll.lastChangeTime) > DEBOUNCE_DELAY_MS) {
    if (readingScroll != btnScroll.stableState) {
      btnScroll.stableState = readingScroll;
      if (btnScroll.stableState == LOW) {
        Serial.println("[CLICK] Scroll Button (Button 2) Pressed");
      } else {
        Serial.println("[RELEASE] Scroll Button (Button 2) Released");
      }
      updateLEDStatus();
    }
  }
  btnScroll.lastReading = readingScroll;
}

// ============================================================================
// Setup & Loop
// ============================================================================

void setup() {
  Serial.begin(115200);

  // Initialize Onboard LED
  initBoardLed();

  // External LEDs
  pinMode(PIN_LED_CALIB, OUTPUT);
  digitalWrite(PIN_LED_CALIB, LOW);
  pinMode(PIN_LED_BTN, OUTPUT);
  digitalWrite(PIN_LED_BTN, LOW);

  // Buttons with internal pull-ups
  pinMode(btnLeft.pin, INPUT_PULLUP);
  pinMode(btnRight.pin, INPUT_PULLUP);
  pinMode(btnScroll.pin, INPUT_PULLUP);

  btnLeft.lastReading   = btnLeft.stableState   = digitalRead(btnLeft.pin);
  btnRight.lastReading  = btnRight.stableState  = digitalRead(btnRight.pin);
  btnScroll.lastReading = btnScroll.stableState = digitalRead(btnScroll.pin);

  initMPU();

  bleMouse.begin();

  // Explicitly configure advertising payload for Windows 11 & Android discovery
  NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
  pAdv->stop();
  pAdv->setName(DEVICE_NAME);
  pAdv->setAppearance(0x03C2); // HID Mouse appearance
  pAdv->addServiceUUID(NimBLEUUID((uint16_t)0x1812)); // HID Service
  pAdv->addServiceUUID(NimBLEUUID((uint16_t)0x180F)); // Battery Service
  pAdv->enableScanResponse(true);
  pAdv->start();

  bleMouse.onConnect([]() {
    Serial.println("[BLE] Host connected!");
  });

  bleMouse.onDisconnect([]() {
    NimBLEDevice::startAdvertising();
    Serial.println("[BLE] Disconnected. Restarted advertising...");
  });

  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_BT);
  Serial.printf("BLE MAC Address: %02X:%02X:%02X:%02X:%02X:%02X\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  Serial.print("Target Board: ");
  Serial.println(BOARD_NAME);
  Serial.print("BLE Mouse ready. Advertising as '");
  Serial.print(DEVICE_NAME);
  Serial.println("'...");
}

void loop() {
  if (bleMouse.isConnected()) {
    if (!isCalibrated) {
      Serial.println("[BLE] Host connected. Waking sensor and calibrating...");
      wakeMPU();
      calibrateSensors();
      smoothX = 0.0f;
      smoothY = 0.0f;
      scrollAccumulator = 0.0f;
      isCalibrated = true;
    }

    handleButtons();

    bool isScrollMode = (btnScroll.stableState == LOW);
    handleMotion(isScrollMode);
    delay(10); // 100 Hz active sampling loop
  } else {
    if (isCalibrated) {
      // Host disconnected: put sensor to sleep and turn off indicators
      isCalibrated = false;
      digitalWrite(PIN_LED_CALIB, LOW);
      digitalWrite(PIN_LED_BTN, LOW);
      setBoardLedOff();
      sleepMPU();
      Serial.println("[POWER] Host disconnected. Sensor in sleep mode.");
    }
    delay(50); // Low-power idle delay while awaiting connection
  }
}
