#include <Arduino.h>
#include <Wire.h>
#define USE_NIMBLE
#include <BleMouse.h>

// --- Pin Assignments (ESP32-C3 Super Mini) ---
#define PIN_SDA    5
#define PIN_SCL    6
#define MPU_ADDR   0x68

#define BTN_LEFT   3
#define BTN_RIGHT  2
#define BTN_SCROLL 7   // Button 2 (Scroll modifier)
#define LED_PIN    8   // Onboard user LED (Active-LOW)

// --- BLE Mouse Instance ---
BleMouse bleMouse("ESP32-C3 Air Mouse", "Espressif", 100);

// --- Sensor Calibration Offsets ---
float gx_offset = 0.0f;
float gz_offset = 0.0f;
float ax_offset = 0.0f;
float ay_offset = 0.0f;
float az_offset = 0.0f;

// --- Motion & Filter Parameters ---
const float Y_DEADZONE       = 1.8f;  // Deg/s deadband to filter pitch tremor
const float X_DEADZONE       = 1.4f;  // Deg/s deadband to filter yaw tremor
const float ALPHA            = 0.22f; // Low-pass filter smoothing factor
const float SENSITIVITY      = 0.40f; // Cursor sensitivity multiplier
const float SCROLL_THRESHOLD = 8.0f;  // Pitch accumulator threshold per wheel notch

// --- Motion / Scroll State ---
float smoothX = 0.0f;
float smoothY = 0.0f;
float scrollAccumulator = 0.0f;

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

// ============================================================================
// Hardware & Sensor Initialization
// ============================================================================

void calibrateSensors() {
  Serial.println("Calibrating... Keep the sensor stationary and level.");
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
}

void initMPU() {
  Wire.begin(PIN_SDA, PIN_SCL);

  // Wake up MPU-6050: clear PWR_MGMT_1 (0x6B)
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x6B);
  Wire.write(0x00);
  Wire.endTransmission();
  delay(50);

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

  calibrateSensors();
}

void updateLEDStatus() {
  // LED is Active-LOW: ON if any button is active/pressed
  bool anyButtonPressed = (btnLeft.stableState == LOW) ||
                          (btnRight.stableState == LOW) ||
                          (btnScroll.stableState == LOW);

  digitalWrite(LED_PIN, anyButtonPressed ? LOW : HIGH);
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

  // --- Left Button (Click & Drag) ---
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

  // --- Right Button (Click) ---
  int readingRight = digitalRead(btnRight.pin);
  if (readingRight != btnRight.lastReading) {
    btnRight.lastChangeTime = now;
  }
  if ((now - btnRight.lastChangeTime) > DEBOUNCE_DELAY_MS) {
    if (readingRight != btnRight.stableState) {
      btnRight.stableState = readingRight;
      if (btnRight.stableState == LOW) {
        bleMouse.click(MOUSE_RIGHT);
        Serial.println("[CLICK] Right Click");
      } else {
        Serial.println("[RELEASE] Right Button Released");
      }
      updateLEDStatus();
    }
  }
  btnRight.lastReading = readingRight;

  // --- Scroll Button / Button 2 (Pin 7) ---
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

  // Status LED (Active-LOW: HIGH is OFF)
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);

  // Buttons with internal pull-ups
  pinMode(btnLeft.pin, INPUT_PULLUP);
  pinMode(btnRight.pin, INPUT_PULLUP);
  pinMode(btnScroll.pin, INPUT_PULLUP);

  btnLeft.lastReading   = btnLeft.stableState   = digitalRead(btnLeft.pin);
  btnRight.lastReading  = btnRight.stableState  = digitalRead(btnRight.pin);
  btnScroll.lastReading = btnScroll.stableState = digitalRead(btnScroll.pin);

  initMPU();

  bleMouse.begin();
  Serial.println("BLE Mouse ready. Advertising as 'ESP32-C3 Air Mouse'...");
}

void loop() {
  if (bleMouse.isConnected()) {
    handleButtons();

    bool isScrollMode = (btnScroll.stableState == LOW);
    handleMotion(isScrollMode);
  }

  delay(10); // 100 Hz sampling loop
}
