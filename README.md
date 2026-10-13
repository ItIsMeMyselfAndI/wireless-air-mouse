# ESP32-C3 Wireless BLE Air Mouse

An inertial Bluetooth Low Energy (BLE) air mouse built with the ESP32-C3 Super Mini microcontroller and an MPU-6050 6-axis IMU. It emulates a standard BLE HID mouse compatible with Windows, Linux, macOS, Android, and iOS.

---

## Features

- **Inertial Air Tracking**: Uses pitch (vertical) and yaw (horizontal) angular velocity from the gyroscope.
- **Drift & Tremor Suppression**: Exponential moving average (EMA) low-pass filter with deadzone filtering.
- **Scroll Wheel Mode**: Hold the dedicated scroll button (Pin 7) and tilt up/down to scroll.
- **Left Click & Drag**: Debounced press and release support on Pin 3.
- **Right Click**: Debounced click support on Pin 2.
- **Status LED**: Visual feedback for active button clicks and scrolling on onboard LED (Pin 8).
- **Serial Debugging**: 115200 baud output for clicks, scrolling events, and sensor calibration.

---

## Hardware Pinout (ESP32-C3 Super Mini)

| Component | Pin / GPIO | Description |
| :--- | :--- | :--- |
| **MPU-6050 SDA** | `GPIO 5` | I2C Data |
| **MPU-6050 SCL** | `GPIO 6` | I2C Clock |
| **MPU-6050 VCC** | `3.3V` / `5V` | Power supply (matches module voltage regulator) |
| **MPU-6050 GND** | `GND` | Ground |
| **Left Button** | `GPIO 3` | Push button to `GND` (Internal Pull-up) |
| **Right Button** | `GPIO 2` | Push button to `GND` (Internal Pull-up) |
| **Scroll Button** | `GPIO 7` | Push button to `GND` (Internal Pull-up) |
| **User LED** | `GPIO 8` | Onboard blue LED (Active-LOW) |

> **Note**: Buttons are wired between the designated GPIO pin and **GND**. Internal pull-ups are enabled in firmware.

---

## Required Libraries

Install the following libraries via the Arduino IDE Library Manager or `arduino-cli`:

1. **ESP32-BLE-Mouse** (by T-Party / wakwak-koba):
   - Library: `ESP32-BLE-Mouse`
2. **NimBLE-Arduino** (by h2zero):
   - Library: `NimBLE-Arduino` (Required for `#define USE_NIMBLE` to minimize RAM and flash footprint)
3. **Wire** (Standard Arduino I2C library, built into ESP32 core)

---

## Build & Upload Instructions

### Option A: Using Arduino IDE (Windows & Linux)

1. **Install ESP32 Board Support**:
   - Open **File > Preferences**.
   - In *Additional Boards Manager URLs*, add:
     ```text
     https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
     ```
   - Open **Tools > Board > Boards Manager...**, search for `esp32`, and install **esp32 by Espressif Systems**.

2. **Select Board and Configuration**:
   - Board: **ESP32C3 Dev Module** (or your specific ESP32-C3 board).
   - USB CDC On Boot: **Enabled** (Important for Serial output on ESP32-C3 USB port).
   - Flash Size: **4MB**.
   - Partition Scheme: **Default 4MB with spiffs** (or *Minimal SPIFFS*).

3. **Install Libraries**:
   - Go to **Sketch > Include Library > Manage Libraries...**.
   - Search and install `NimBLE-Arduino`.
   - Search and install `ESP32 BLE Mouse`.

4. **Flash the Sketch**:
   - Connect the ESP32-C3 via USB.
   - Select the COM port (`COMx` on Windows, `/dev/ttyACM0` or `/dev/ttyUSB0` on Linux).
   - Click **Upload**.

---

### Option B: Using Command Line / arduino-cli

#### Linux

1. **Dialout Permissions**:
   Ensure your user has access to serial ports:
   ```bash
   sudo usermod -aG dialout $USER
   # Log out and log back in for changes to take effect
   ```

2. **Install Board Core and Libraries**:
   ```bash
   arduino-cli core update-index --additional-urls https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   arduino-cli core install esp32:esp32 --additional-urls https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   arduino-cli lib install "NimBLE-Arduino"
   arduino-cli lib install "ESP32 BLE Mouse"
   ```

3. **Compile and Upload**:
   ```bash
   # Compile with USB CDC enabled for ESP32-C3
   arduino-cli compile --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc WirelessAirMouse/

   # Upload (replace /dev/ttyACM0 with your port)
   arduino-cli upload -p /dev/ttyACM0 --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc WirelessAirMouse/
   ```

#### Windows

1. **Install via PowerShell**:
   ```powershell
   arduino-cli core update-index --additional-urls https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   arduino-cli core install esp32:esp32 --additional-urls https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   arduino-cli lib install "NimBLE-Arduino"
   arduino-cli lib install "ESP32 BLE Mouse"
   ```

2. **Compile and Upload**:
   ```powershell
   # Compile
   arduino-cli compile --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc .\WirelessAirMouse\

   # Upload (replace COM3 with your actual COM port from Device Manager)
   arduino-cli upload -p COM3 --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc .\WirelessAirMouse\
   ```

---

## Operating Instructions

### 1. Calibration on Startup
- Keep the board **stationary and level** for ~2-3 seconds immediately after power-up/reset.
- The firmware collects 250 samples to compute zero-rate gyro and accel offsets.

### 2. Pairing with Your Computer

#### Windows 10 / 11
1. Open **Settings > Bluetooth & devices**.
2. Turn Bluetooth **On** and click **Add device**.
3. Select **Bluetooth**.
4. Click on **ESP32-C3 Air Mouse**.
5. Once paired, the mouse is ready to use immediately.

#### Linux (GNOME / KDE / bluez)
- **GUI**: Open **Settings > Bluetooth**, wait for **ESP32-C3 Air Mouse** to appear, and click **Connect**.
- **CLI (`bluetoothctl`)**:
  ```bash
  bluetoothctl
  scan on
  # Look for "ESP32-C3 Air Mouse" and copy its MAC address (e.g., XX:XX:XX:XX:XX:XX)
  pair XX:XX:XX:XX:XX:XX
  trust XX:XX:XX:XX:XX:XX
  connect XX:XX:XX:XX:XX:XX
  ```

---

## Controls & Usage

- **Cursor Tracking**: Point and tilt the controller. Pitch controls vertical movement; yaw controls horizontal movement.
- **Left Click**: Press button on `GPIO 3`. Can be held down for drag-and-drop operations.
- **Right Click**: Press button on `GPIO 2`.
- **Scroll Wheel**: Press and hold the scroll button on `GPIO 7`, then tilt the controller up or down to scroll. Cursor tracking is temporarily locked while scrolling.
