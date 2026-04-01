/*
 * ESP32-S3 USB-OTG → BLE HID Bridge — "Vendit-Naga-X"
 *
 * Single-board replacement for the Pico + ESP32-C3 two-board setup.
 * Uses the ESP32-S3 native USB-OTG as USB Host to read HID reports
 * from a Razer Naga X mouse, then re-transmits them over BLE HID.
 *
 * Hardware: ESP32-S3 DevKitC-1 (USB-OTG port → Naga X)
 *
 * BLE HID Report Map:
 *   Report ID 1 — Mouse  (buttons, X, Y, wheel)
 *   Report ID 2 — Keyboard (modifier, reserved, keys[6])
 *
 * Arduino IDE Settings:
 *   Board:     "ESP32S3 Dev Module"
 *   USB Mode:  "Disabled"  ← CRITICAL: frees OTG peripheral for Host mode
 *   PSRAM:     "OPI PSRAM" (if available)
 *   Partition: "Default 4MB with spiffs"
 *
 * Required Libraries:
 *   - NimBLE-Arduino  (v1.4+)
 */

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>
#include "usb/usb_host.h"

// Use UART0 for serial output (external UART chip on /dev/cu.usbserial-*)
// "Hardware CDC and JTAG" mode maps Serial to internal USB CDC,
// so we use Serial0 (UART0) explicitly for the external UART port.
#define LOG Serial0

// ─── Configuration ─────────────────────────────────────────────
#define DEVICE_NAME       "Vendit-Naga-X-2"
#define MAX_HID_REPORT    64
#define MAX_HID_IFACES    4    // Max HID interfaces on a composite device

// Mouse sensitivity: divide raw X/Y by this value.
// Naga X at high DPI sends very large deltas.
// Increase to slow down, decrease to speed up. 1 = raw passthrough.
#define MOUSE_SENSITIVITY_DIV  16

// USB HID class constants
#define USB_CLASS_HID             0x03
#define USB_HID_SUBCLASS_BOOT    0x01
#define USB_HID_PROTOCOL_MOUSE   0x02
#define USB_HID_PROTOCOL_KBD     0x01
#define USB_DESC_TYPE_HID        0x21
#define USB_HID_SET_IDLE         0x0A
#define USB_HID_SET_PROTOCOL     0x0B

// Razer vendor-specific constants
#define RAZER_VID                0x1532
#define RAZER_USB_REPORT_LEN     90     // 0x5A
#define RAZER_TRANSACTION_ID     0x1F   // For Naga Left-Handed 2020
#define RAZER_CMD_SET_DEVICE_MODE_CLASS  0x00
#define RAZER_CMD_SET_DEVICE_MODE_ID     0x04
#define RAZER_DRIVER_MODE        0x03
#define RAZER_CMD_SET_DPI_CLASS  0x04
#define RAZER_CMD_SET_DPI_ID     0x05
#define RAZER_DPI_VALUE          1000     // Change this to your preferred DPI (100-20000)
#define RAZER_CMD_SET_POLL_RATE_CLASS  0x00
#define RAZER_CMD_SET_POLL_RATE_ID     0x05
#define RAZER_POLL_RATE_1000HZ   0x01   // 0x01=1000Hz, 0x02=500Hz, 0x08=125Hz

// Razer LED control
#define RAZER_CMD_LED_CLASS      0x0F
#define RAZER_CMD_LED_ID         0x02
#define RAZER_LED_SCROLL         0x01
#define RAZER_LED_LOGO           0x04
#define RAZER_LED_SIDE           0x10
#define RAZER_LED_EFFECT_STATIC  0x01

// ─── HID Report Map (BLE side) ────────────────────────────────
// Combined Mouse (ID=1) + Keyboard (ID=2)
static const uint8_t hidReportMap[] = {
  // ── Mouse (Report ID 1) ──────────────────────────────────────
  0x05, 0x01,       // Usage Page (Generic Desktop)
  0x09, 0x02,       // Usage (Mouse)
  0xA1, 0x01,       // Collection (Application)
  0x85, 0x01,       //   Report ID (1)
  0x09, 0x01,       //   Usage (Pointer)
  0xA1, 0x00,       //   Collection (Physical)

  // 5 buttons
  0x05, 0x09,       //     Usage Page (Buttons)
  0x19, 0x01,       //     Usage Minimum (1)
  0x29, 0x05,       //     Usage Maximum (5)
  0x15, 0x00,       //     Logical Minimum (0)
  0x25, 0x01,       //     Logical Maximum (1)
  0x95, 0x05,       //     Report Count (5)
  0x75, 0x01,       //     Report Size (1)
  0x81, 0x02,       //     Input (Data, Variable, Absolute)

  // 3-bit padding
  0x95, 0x01,       //     Report Count (1)
  0x75, 0x03,       //     Report Size (3)
  0x81, 0x01,       //     Input (Constant)

  // X, Y movement (16-bit each)
  0x05, 0x01,       //     Usage Page (Generic Desktop)
  0x09, 0x30,       //     Usage (X)
  0x09, 0x31,       //     Usage (Y)
  0x16, 0x00, 0x80, //     Logical Minimum (-32768)
  0x26, 0xFF, 0x7F, //     Logical Maximum (32767)
  0x75, 0x10,       //     Report Size (16)
  0x95, 0x02,       //     Report Count (2)
  0x81, 0x06,       //     Input (Data, Variable, Relative)

  // Scroll wheel (8-bit)
  0x09, 0x38,       //     Usage (Wheel)
  0x15, 0x81,       //     Logical Minimum (-127)
  0x25, 0x7F,       //     Logical Maximum (127)
  0x75, 0x08,       //     Report Size (8)
  0x95, 0x01,       //     Report Count (1)
  0x81, 0x06,       //     Input (Data, Variable, Relative)

  0xC0,             //   End Collection (Physical)
  0xC0,             // End Collection (Application)

  // ── Keyboard (Report ID 2) ───────────────────────────────────
  0x05, 0x01,       // Usage Page (Generic Desktop)
  0x09, 0x06,       // Usage (Keyboard)
  0xA1, 0x01,       // Collection (Application)
  0x85, 0x02,       //   Report ID (2)

  // Modifier keys (8 bits)
  0x05, 0x07,       //   Usage Page (Key Codes)
  0x19, 0xE0,       //   Usage Minimum (Left Control)
  0x29, 0xE7,       //   Usage Maximum (Right GUI)
  0x15, 0x00,       //   Logical Minimum (0)
  0x25, 0x01,       //   Logical Maximum (1)
  0x75, 0x01,       //   Report Size (1)
  0x95, 0x08,       //   Report Count (8)
  0x81, 0x02,       //   Input (Data, Variable, Absolute)

  // Reserved byte
  0x95, 0x01,       //   Report Count (1)
  0x75, 0x08,       //   Report Size (8)
  0x81, 0x01,       //   Input (Constant)

  // 6 key codes
  0x95, 0x06,       //   Report Count (6)
  0x75, 0x08,       //   Report Size (8)
  0x15, 0x00,       //   Logical Minimum (0)
  0x25, 0x65,       //   Logical Maximum (101)
  0x05, 0x07,       //   Usage Page (Key Codes)
  0x19, 0x00,       //   Usage Minimum (0)
  0x29, 0x65,       //   Usage Maximum (101)
  0x81, 0x00,       //   Input (Data, Array)

  0xC0              // End Collection
};

// ─── HID Interface Tracking ──────────────────────────────────
typedef struct {
  bool     active;
  uint8_t  iface_num;
  uint8_t  ep_addr;       // Interrupt IN endpoint address
  uint16_t ep_mps;        // Max packet size
  uint8_t  protocol;      // 1=kbd, 2=mouse, 0=other
  usb_transfer_t* xfer;   // Ongoing IN transfer
} hid_iface_t;

static hid_iface_t hidIfaces[MAX_HID_IFACES];
static uint8_t     numHidIfaces = 0;

// ─── BLE Globals ──────────────────────────────────────────────
static NimBLEHIDDevice*      hid          = nullptr;
static NimBLECharacteristic* mouseInput   = nullptr;
static NimBLECharacteristic* keyInput     = nullptr;
static volatile bool         bleConnected = false;

// ─── Mouse Report Accumulator ─────────────────────────────────
// Accumulate X/Y deltas between BLE sends to avoid overwhelming BLE
static volatile int32_t  accumX = 0;
static volatile int32_t  accumY = 0;
static volatile int8_t   accumW = 0;       // Latest wheel value
static volatile uint8_t  accumBtn = 0;     // Latest button state
static volatile bool     mouseDirty = false;
static unsigned long     lastMouseNotify = 0;
#define BLE_MOUSE_INTERVAL_MS  8  // ~125Hz BLE notify rate

// ─── Kalman Filter for Cursor Smoothing ───────────────────────
// 1D Kalman filter per axis — estimates velocity (delta per frame)
// Adapts automatically: steady motion → smooth, quick turns → responsive
// Kalman presets — cycle with DPI buttons on the mouse
typedef struct {
  float q;
  float r;
  const char* name;
  uint8_t ledR, ledG, ledB;
} kalman_preset_t;

static const kalman_preset_t kalmanPresets[] = {
  { 1.0f, 3.0f, "Office",     0x00, 0x60, 0xFF },  // 파랑
  { 2.0f, 2.0f, "Balanced",   0x00, 0xFF, 0x40 },  // 초록
  { 3.0f, 1.0f, "Responsive", 0xFF, 0xC0, 0x00 },  // 노랑
  { 5.0f, 0.5f, "Gaming",     0xFF, 0x20, 0x00 },  // 빨강
  { 10.0f, 0.1f, "Raw",       0x80, 0x00, 0xFF },  // 보라
};
#define NUM_KALMAN_PRESETS  (sizeof(kalmanPresets) / sizeof(kalmanPresets[0]))
static uint8_t currentPreset = 2;  // Default: Responsive

// Active Kalman params (updated on preset change)
static float KALMAN_Q = 3.0f;
static float KALMAN_R = 1.0f;

typedef struct {
  float x;     // State estimate (filtered velocity)
  float p;     // Estimation error covariance
  float rem;   // Sub-pixel remainder for integer output
} kalman1d_t;

static kalman1d_t kfX = { 0, 1.0f, 0 };
static kalman1d_t kfY = { 0, 1.0f, 0 };

// Kalman predict + update, returns filtered value
static float kalmanUpdate(kalman1d_t* kf, float measurement) {
  // Predict
  // x_pred = x (constant velocity model)
  float p_pred = kf->p + KALMAN_Q;

  // Update
  float k = p_pred / (p_pred + KALMAN_R);  // Kalman gain
  kf->x = kf->x + k * (measurement - kf->x);
  kf->p = (1.0f - k) * p_pred;

  return kf->x;
}

// Apply Kalman filter to accumulated delta and return integer output
static int16_t kalmanApply(kalman1d_t* kf, int32_t rawAccum) {
  if (rawAccum == 0 && fabsf(kf->x) < 0.5f) {
    // Mouse stopped — decay to zero
    kf->x *= 0.3f;
    kf->rem = 0;
    return 0;
  }

  float filtered = kalmanUpdate(kf, (float)rawAccum);
  float withRem = filtered + kf->rem;

  // Round to integer, keep remainder
  int16_t out = (int16_t)(withRem + (withRem > 0 ? 0.5f : -0.5f));
  kf->rem = withRem - out;

  // Prevent drift
  if (rawAccum == 0 && abs(out) <= 1) {
    kf->rem = 0;
    out = 0;
  }

  return out;
}

// ─── Middle Button Mode Toggle ────────────────────────────────
// Side buttons 10+11+12 (0, -, =) pressed simultaneously → toggle
// Mode 0: Mission Control (Ctrl+Up)
// Mode 1: Normal middle click (button 3)
static uint8_t middleButtonMode = 0;  // 0=Mission Control, 1=Normal

// ─── Scroll Wheel Filter ──────────────────────────────────────
// Suppress encoder backlash/chatter by locking direction briefly
#define SCROLL_DIR_LOCK_MS    120   // Ignore reverse direction for this long
#define SCROLL_REVERSE_COUNT  2    // Require N consecutive reverse events to change direction
static int8_t  scrollDir = 0;            // Current locked direction: +1, -1, or 0
static unsigned long scrollDirTime = 0;  // When direction was locked
static uint8_t scrollReverseCount = 0;   // Consecutive reverse event counter

static int8_t filterScroll(int8_t raw) {
  if (raw == 0) return 0;

  unsigned long now = millis();
  int8_t dir = (raw > 0) ? 1 : -1;

  if (scrollDir == 0) {
    // No direction locked — accept and lock
    scrollDir = dir;
    scrollDirTime = now;
    scrollReverseCount = 0;
    return raw;
  }

  if (dir == scrollDir) {
    // Same direction — accept, refresh lock
    scrollDirTime = now;
    scrollReverseCount = 0;
    return raw;
  }

  // Opposite direction
  if (now - scrollDirTime < SCROLL_DIR_LOCK_MS) {
    // Within lock window — count reverse events
    scrollReverseCount++;
    if (scrollReverseCount >= SCROLL_REVERSE_COUNT) {
      // Enough consecutive reverses — accept direction change
      scrollDir = dir;
      scrollDirTime = now;
      scrollReverseCount = 0;
      return raw;
    }
    return 0;  // Suppress backlash
  }

  // Lock expired — accept new direction
  scrollDir = dir;
  scrollDirTime = now;
  scrollReverseCount = 0;
  return raw;
}

// ─── USB Host Globals ─────────────────────────────────────────
static usb_host_client_handle_t clientHandle = NULL;
static usb_device_handle_t      devHandle    = NULL;
static uint8_t                  usbDevAddr   = 0;
static volatile bool            usbDeviceConnected = false;

// Queue removed — dispatch directly from USB callback for minimum latency

// ─── BLE Callbacks ────────────────────────────────────────────
class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
    bleConnected = true;
    LOG.println("[BLE] Client connected");
    // Min connection interval = 6 (7.5ms), latency=0, timeout=400
    pServer->updateConnParams(connInfo.getConnHandle(), 6, 6, 0, 400);
  }

  void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
    bleConnected = false;
    LOG.printf("[BLE] Client disconnected (reason=%d)\r\n", reason);
    NimBLEDevice::startAdvertising();
  }
};

// ─── BLE Setup ────────────────────────────────────────────────
static void setupBLE() {
  NimBLEDevice::init(DEVICE_NAME);
  NimBLEDevice::setSecurityAuth(true, false, false);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  NimBLEServer* pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  hid = new NimBLEHIDDevice(pServer);
  hid->setManufacturer("Vendit");
  hid->setPnp(0x02, 0x1532, 0x0084, 0x0100);
  hid->setHidInfo(0x00, 0x01);
  hid->setReportMap((uint8_t*)hidReportMap, sizeof(hidReportMap));

  mouseInput = hid->getInputReport(1);
  keyInput   = hid->getInputReport(2);

  hid->startServices();

  NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
  pAdv->setAppearance(HID_MOUSE);
  pAdv->addServiceUUID(hid->getHidService()->getUUID());
  pAdv->start();

  LOG.println("[BLE] Advertising as " DEVICE_NAME);
}

// ─── HID Report Dispatch (USB → BLE) ─────────────────────────
static void dispatchHidReport(uint8_t ifaceIdx,
                              const uint8_t* data, uint16_t len) {
  if (len == 0) return;

  uint8_t protocol = hidIfaces[ifaceIdx].protocol;
  uint8_t iface_num = hidIfaces[ifaceIdx].iface_num;

  if (!bleConnected) {
    // Log once per second when BLE not connected
    static unsigned long lastLog = 0;
    if (millis() - lastLog > 1000) {
      LOG.printf("[WAIT] iface=%d data but BLE not connected\n", iface_num);
      lastLog = millis();
    }
    return;
  }

  // ── Interface 0: Mouse (proto=2) ──
  // Razer Driver Mode report: [buttons, X(int8), Y(int8), wheel(int8), ...]
  // Button bits from Razer:  0=Left, 1=Right, 2=Middle, 5=Back, 6=Forward
  // Standard 5-btn HID:      0=Left, 1=Right, 2=Middle, 3=Back, 4=Forward
  if (iface_num == 0 && protocol == USB_HID_PROTOCOL_MOUSE) {
    uint8_t mouseReport[6] = {0};

    if (len >= 8) {
      // Razer Driver Mode 8-byte report:
      //   [0]=buttons [1]=X(8bit) [2]=Y(8bit) [3]=wheel
      //   [4-5]=X(16bit LE) [6-7]=Y(16bit LE)
      // Use the 16-bit values for full DPI resolution
      int16_t outX = (int16_t)(data[4] | (data[5] << 8));
      int16_t outY = (int16_t)(data[6] | (data[7] << 8));

      // Remap Razer button bits → standard 5-button layout
      // Razer:   bit0=Left, bit1=Right, bit2=Middle, bit5=Back, bit6=Forward
      // BLE HID: bit0=Left, bit1=Right, bit2=Middle, bit3=Back, bit4=Forward
      uint8_t rawBtn = data[0];

      // Middle button handling (mode-dependent)
      {
        static bool middleWasPressed = false;
        bool middleNow = (rawBtn & 0x44) != 0;  // 0x04 or 0x40

        if (middleButtonMode == 0) {
          // Mode 0: Mission Control (Ctrl+Up)
          if (middleNow && !middleWasPressed) {
            uint8_t keyReport[8] = {0};
            keyReport[0] = 0x01;  // Left Control
            keyReport[2] = 0x52;  // Up Arrow
            keyInput->setValue(keyReport, sizeof(keyReport));
            keyInput->notify();
          } else if (!middleNow && middleWasPressed) {
            uint8_t keyReport[8] = {0};
            keyInput->setValue(keyReport, sizeof(keyReport));
            keyInput->notify();
          }
          rawBtn &= ~0x44;  // Strip from mouse report
        } else {
          // Mode 1: Normal middle click — remap 0x40→0x04
          if (rawBtn & 0x40) {
            rawBtn = (rawBtn & ~0x40) | 0x04;  // Move bit6 to bit2
          }
          // Keep bit2 (0x04) in rawBtn for mouse report
        }
        middleWasPressed = middleNow;
      }

      uint8_t btn = (rawBtn & 0x07);            // bits 0-2: Left, Right, Middle
      if (rawBtn & 0x20) btn |= 0x08;           // bit 5 → bit 3: Back

      // Accumulate deltas
      accumX += outX;
      accumY += outY;
      int8_t filteredW = filterScroll((int8_t)data[3]);
      if (filteredW != 0) accumW = filteredW;
      accumBtn = btn;
      mouseDirty = true;

      // Send immediately if button changed, or if interval elapsed
      static uint8_t prevBtn = 0;
      unsigned long now = millis();
      bool btnChanged = (btn != prevBtn);
      prevBtn = btn;

      if (btnChanged || (now - lastMouseNotify >= BLE_MOUSE_INTERVAL_MS)) {
        // Apply Kalman filter
        int16_t sendX = kalmanApply(&kfX, accumX);
        int16_t sendY = kalmanApply(&kfY, accumY);

        mouseReport[0] = accumBtn;
        mouseReport[1] = (uint8_t)(sendX & 0xFF);
        mouseReport[2] = (uint8_t)(sendX >> 8);
        mouseReport[3] = (uint8_t)(sendY & 0xFF);
        mouseReport[4] = (uint8_t)(sendY >> 8);
        mouseReport[5] = accumW;

        mouseInput->setValue(mouseReport, sizeof(mouseReport));
        mouseInput->notify();

        accumX = 0;
        accumY = 0;
        accumW = 0;
        mouseDirty = (fabsf(kfX.x) > 0.5f || fabsf(kfY.x) > 0.5f);
        lastMouseNotify = now;
      }
    }

  // ── Interface 1: Razer proprietary — intercept DPI buttons only ──
  } else if (iface_num == 1) {
    // Report ID 4 with keycode 0x20=DPI Up, 0x21=DPI Down
    if (len >= 2 && data[0] == 0x04) {
      static bool dpiTriggered = false;
      if (data[1] == 0x20 || data[1] == 0x21) {
        if (!dpiTriggered) {
          if (data[1] == 0x20 && currentPreset < NUM_KALMAN_PRESETS - 1) {
            currentPreset++;
          } else if (data[1] == 0x21 && currentPreset > 0) {
            currentPreset--;
          }
          KALMAN_Q = kalmanPresets[currentPreset].q;
          KALMAN_R = kalmanPresets[currentPreset].r;
          kfX.p = 1.0f; kfY.p = 1.0f;
          // LED feedback
          razerSetAllLeds(kalmanPresets[currentPreset].ledR,
                          kalmanPresets[currentPreset].ledG,
                          kalmanPresets[currentPreset].ledB);
          LOG.printf("[KALMAN] Preset: %s (Q=%.1f R=%.1f)\n",
                     kalmanPresets[currentPreset].name, KALMAN_Q, KALMAN_R);
          dpiTriggered = true;
        }
      } else if (data[1] == 0x00) {
        dpiTriggered = false;  // Button released
      }
    }

  // ── Interface 2: Boot Keyboard (side buttons, proto=1, mps=8) ──
  } else if (iface_num == 2 && protocol == USB_HID_PROTOCOL_KBD) {
    uint8_t keyReport[8] = {0};
    uint8_t copyLen = (len > 8) ? 8 : len;
    memcpy(keyReport, data, copyLen);

    // ── Combo: 10+11 (0 + -) → toggle middle button mode ──
    {
      static bool comboTriggered = false;
      bool has27 = false, has2D = false;
      for (int i = 2; i < 8; i++) {
        if (keyReport[i] == 0x27) has27 = true;  // Button 10 (0)
        if (keyReport[i] == 0x2D) has2D = true;  // Button 11 (-)
      }
      if (has27 && has2D) {
        if (!comboTriggered) {
          middleButtonMode = (middleButtonMode == 0) ? 1 : 0;
          LOG.printf("[MODE] Middle button → %s\n",
                     middleButtonMode == 0 ? "Mission Control" : "Normal Click");
          comboTriggered = true;
        }
        return;
      } else {
        comboTriggered = false;
      }
    }

    // ── Combo: 11+12 (- + =) → arrow key mode for 2,4,5,6 ──
    {
      static bool arrowMode = false;
      static bool arrowComboTriggered = false;
      bool has2D = false, has2E = false;
      for (int i = 2; i < 8; i++) {
        if (keyReport[i] == 0x2D) has2D = true;  // Button 11 (-)
        if (keyReport[i] == 0x2E) has2E = true;  // Button 12 (=)
      }
      if (has2D && has2E) {
        if (!arrowComboTriggered) {
          arrowMode = !arrowMode;
          LOG.printf("[MODE] Arrow keys → %s\n", arrowMode ? "ON" : "OFF");
          arrowComboTriggered = true;
        }
        return;
      } else {
        arrowComboTriggered = false;
      }

      // Remap side buttons
      for (int i = 2; i < 8; i++) {
        if (arrowMode) {
          // Arrow mode: 2=↑, 4=←, 5=↓, 6=→
          if      (keyReport[i] == 0x1F) keyReport[i] = 0x52;  // 2 → Up
          else if (keyReport[i] == 0x21) keyReport[i] = 0x50;  // 4 → Left
          else if (keyReport[i] == 0x22) keyReport[i] = 0x51;  // 5 → Down
          else if (keyReport[i] == 0x23) keyReport[i] = 0x4F;  // 6 → Right
        } else {
          // Normal mode: 2=F5
          if (keyReport[i] == 0x1F) {                           // 2 → Cmd+R (Reload)
            keyReport[0] |= 0x08;                               // Left GUI (Cmd)
            keyReport[i] = 0x15;                                 // 'R'
          }
        }

        // Always active remaps (both modes)
        if (keyReport[i] == 0x1E) {           // Side button 1 → Cmd+V
          keyReport[0] |= 0x08;
          keyReport[i] = 0x19;
        } else if (keyReport[i] == 0x20) {    // Side button 3 → Cmd+C
          keyReport[0] |= 0x08;
          keyReport[i] = 0x06;
        }
      }
    }

    keyInput->setValue(keyReport, sizeof(keyReport));
    keyInput->notify();
  }
  // Interface 1: ignored — Razer proprietary (sends config data as fake keyboard)
  // Interface 3: ignored — likely consumer control / battery
}

// ─── USB Interrupt Transfer Callback ─────────────────────────
static void usbTransferCallback(usb_transfer_t* transfer) {
  if (transfer->status == USB_TRANSFER_STATUS_COMPLETED && transfer->actual_num_bytes > 0) {
    // Dispatch directly — no queue, minimum latency
    for (int i = 0; i < numHidIfaces; i++) {
      if (hidIfaces[i].xfer == transfer) {
        dispatchHidReport(i, transfer->data_buffer, transfer->actual_num_bytes);
        break;
      }
    }
  }

  // Re-submit immediately
  if (transfer->status != USB_TRANSFER_STATUS_CANCELED) {
    usb_host_transfer_submit(transfer);
  }
}

// ─── Razer LED Color Set ──────────────────────────────────────
static void razerSetLedColor(uint8_t ledId, uint8_t r, uint8_t g, uint8_t b) {
  if (!devHandle) return;

  usb_transfer_t* xfer = NULL;
  esp_err_t err = usb_host_transfer_alloc(RAZER_USB_REPORT_LEN + 8, 0, &xfer);
  if (err != ESP_OK || !xfer) return;

  uint8_t* setup = xfer->data_buffer;
  uint8_t* report = setup + 8;
  memset(report, 0, RAZER_USB_REPORT_LEN);

  report[0] = 0x00;                    // status
  report[1] = RAZER_TRANSACTION_ID;    // 0x1F
  report[5] = 0x09;                    // data_size
  report[6] = RAZER_CMD_LED_CLASS;     // 0x0F
  report[7] = RAZER_CMD_LED_ID;        // 0x02
  report[8]  = 0x01;                   // VARSTORE
  report[9]  = ledId;                  // LED region
  report[10] = RAZER_LED_EFFECT_STATIC; // 0x01
  report[11] = 0x00;
  report[12] = 0x00;
  report[13] = 0x01;                   // param (always 1 for static)
  report[14] = r;
  report[15] = g;
  report[16] = b;

  uint8_t crc = 0;
  for (int i = 2; i < 88; i++) crc ^= report[i];
  report[88] = crc;

  setup[0] = 0x21; setup[1] = 0x09;
  setup[2] = 0x00; setup[3] = 0x03;
  setup[4] = 0x00; setup[5] = 0x00;
  setup[6] = (uint8_t)(RAZER_USB_REPORT_LEN & 0xFF);
  setup[7] = (uint8_t)(RAZER_USB_REPORT_LEN >> 8);

  xfer->num_bytes = 8 + RAZER_USB_REPORT_LEN;
  xfer->device_handle = devHandle;
  xfer->bEndpointAddress = 0;
  xfer->callback = [](usb_transfer_t* t) { usb_host_transfer_free(t); };

  usb_host_transfer_submit_control(clientHandle, xfer);
}

// Set all LED zones to the same color
static void razerSetAllLeds(uint8_t r, uint8_t g, uint8_t b) {
  razerSetLedColor(RAZER_LED_SCROLL, r, g, b);
  vTaskDelay(pdMS_TO_TICKS(10));
  razerSetLedColor(RAZER_LED_LOGO, r, g, b);
  vTaskDelay(pdMS_TO_TICKS(10));
  razerSetLedColor(RAZER_LED_SIDE, r, g, b);
}

// ─── Parse USB Descriptors & Claim HID Interfaces ────────────
static void openHidDevice(uint8_t dev_addr) {
  // Use global devHandle for cleanup on disconnect
  esp_err_t err;

  err = usb_host_device_open(clientHandle, dev_addr, &devHandle);
  if (err != ESP_OK) {
    LOG.printf("[USB] Device open failed: 0x%x\n", err);
    return;
  }

  // Get device descriptor for VID/PID
  const usb_device_desc_t* dev_desc;
  usb_host_get_device_descriptor(devHandle, &dev_desc);
  LOG.printf("[USB] Device VID=0x%04X PID=0x%04X\n",
                dev_desc->idVendor, dev_desc->idProduct);

  // ── Razer Driver Mode: enable middle button ──
  // Razer mice default to "Device Mode" (0x00) where wheel click
  // is NOT sent via HID. Must switch to "Driver Mode" (0x03).
  if (dev_desc->idVendor == RAZER_VID) {
    LOG.println("[RAZER] Sending SET_DEVICE_MODE(Driver=0x03)...");

    usb_transfer_t* razer_xfer = NULL;
    err = usb_host_transfer_alloc(RAZER_USB_REPORT_LEN + 8, 0, &razer_xfer);
    if (err == ESP_OK && razer_xfer) {
      // Build 90-byte Razer command packet at data_buffer+8
      // (first 8 bytes = USB SETUP packet for control transfer)
      uint8_t* setup = razer_xfer->data_buffer;
      uint8_t* report = setup + 8;  // Razer report starts after SETUP

      // Zero the entire report
      memset(report, 0, RAZER_USB_REPORT_LEN);

      // Fill Razer command fields
      report[0] = 0x00;                    // status: new
      report[1] = RAZER_TRANSACTION_ID;    // 0x1F
      // report[2-3] = 0x0000              // remaining_packets
      // report[4] = 0x00                  // protocol_type
      report[5] = 0x02;                    // data_size: 2 bytes
      report[6] = RAZER_CMD_SET_DEVICE_MODE_CLASS;  // 0x00
      report[7] = RAZER_CMD_SET_DEVICE_MODE_ID;     // 0x04
      report[8] = RAZER_DRIVER_MODE;       // 0x03 (driver mode)
      report[9] = 0x00;                    // param (forced 0)

      // Calculate CRC: XOR bytes 2..87
      uint8_t crc = 0;
      for (int i = 2; i < 88; i++) crc ^= report[i];
      report[88] = crc;

      // USB SETUP packet: SET_REPORT, Feature Report, Interface 0
      setup[0] = 0x21;   // bmRequestType: Class, Interface, OUT
      setup[1] = 0x09;   // bRequest: SET_REPORT
      setup[2] = 0x00;   // wValue low: Report ID 0
      setup[3] = 0x03;   // wValue high: Feature (0x03)
      setup[4] = 0x00;   // wIndex low: Interface 0
      setup[5] = 0x00;   // wIndex high
      setup[6] = (uint8_t)(RAZER_USB_REPORT_LEN & 0xFF);  // wLength low
      setup[7] = (uint8_t)(RAZER_USB_REPORT_LEN >> 8);    // wLength high

      razer_xfer->num_bytes = 8 + RAZER_USB_REPORT_LEN;
      razer_xfer->device_handle = devHandle;
      razer_xfer->bEndpointAddress = 0;
      razer_xfer->callback = [](usb_transfer_t* t) {
        LOG.printf("[RAZER] SET_DEVICE_MODE response: status=%d\n", t->status);
        usb_host_transfer_free(t);
      };

      err = usb_host_transfer_submit_control(clientHandle, razer_xfer);
      if (err != ESP_OK) {
        LOG.printf("[RAZER] Control transfer failed: 0x%x\n", err);
        usb_host_transfer_free(razer_xfer);
      }

      vTaskDelay(pdMS_TO_TICKS(100));  // Give device time to switch mode
    }

    // ── Set DPI ──
    {
      LOG.printf("[RAZER] Setting DPI to %d...\n", RAZER_DPI_VALUE);

      usb_transfer_t* dpi_xfer = NULL;
      err = usb_host_transfer_alloc(RAZER_USB_REPORT_LEN + 8, 0, &dpi_xfer);
      if (err == ESP_OK && dpi_xfer) {
        uint8_t* setup = dpi_xfer->data_buffer;
        uint8_t* report = setup + 8;
        memset(report, 0, RAZER_USB_REPORT_LEN);

        report[0] = 0x00;                     // status
        report[1] = RAZER_TRANSACTION_ID;      // 0x1F
        report[5] = 0x07;                      // data_size: 7
        report[6] = RAZER_CMD_SET_DPI_CLASS;   // 0x04
        report[7] = RAZER_CMD_SET_DPI_ID;      // 0x05
        report[8] = 0x01;                      // VARSTORE (persistent)
        report[9]  = (RAZER_DPI_VALUE >> 8) & 0xFF;  // X DPI high
        report[10] = RAZER_DPI_VALUE & 0xFF;          // X DPI low
        report[11] = (RAZER_DPI_VALUE >> 8) & 0xFF;   // Y DPI high
        report[12] = RAZER_DPI_VALUE & 0xFF;           // Y DPI low
        report[13] = 0x00;
        report[14] = 0x00;

        // CRC: XOR bytes 2..87
        uint8_t crc = 0;
        for (int i = 2; i < 88; i++) crc ^= report[i];
        report[88] = crc;

        // USB SETUP: SET_REPORT Feature, Interface 0
        setup[0] = 0x21;
        setup[1] = 0x09;
        setup[2] = 0x00;
        setup[3] = 0x03;
        setup[4] = 0x00;
        setup[5] = 0x00;
        setup[6] = (uint8_t)(RAZER_USB_REPORT_LEN & 0xFF);
        setup[7] = (uint8_t)(RAZER_USB_REPORT_LEN >> 8);

        dpi_xfer->num_bytes = 8 + RAZER_USB_REPORT_LEN;
        dpi_xfer->device_handle = devHandle;
        dpi_xfer->bEndpointAddress = 0;
        dpi_xfer->callback = [](usb_transfer_t* t) {
          LOG.printf("[RAZER] SET_DPI response: status=%d\n", t->status);
          usb_host_transfer_free(t);
        };

        err = usb_host_transfer_submit_control(clientHandle, dpi_xfer);
        if (err != ESP_OK) {
          LOG.printf("[RAZER] DPI transfer failed: 0x%x\n", err);
          usb_host_transfer_free(dpi_xfer);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
      }
    }

    // ── Set Poll Rate to 1000Hz ──
    {
      LOG.println("[RAZER] Setting poll rate to 1000Hz...");

      usb_transfer_t* poll_xfer = NULL;
      err = usb_host_transfer_alloc(RAZER_USB_REPORT_LEN + 8, 0, &poll_xfer);
      if (err == ESP_OK && poll_xfer) {
        uint8_t* setup = poll_xfer->data_buffer;
        uint8_t* report = setup + 8;
        memset(report, 0, RAZER_USB_REPORT_LEN);

        report[0] = 0x00;
        report[1] = RAZER_TRANSACTION_ID;
        report[5] = 0x01;                          // data_size: 1
        report[6] = RAZER_CMD_SET_POLL_RATE_CLASS;  // 0x00
        report[7] = RAZER_CMD_SET_POLL_RATE_ID;     // 0x05
        report[8] = RAZER_POLL_RATE_1000HZ;         // 0x01 = 1000Hz

        uint8_t crc = 0;
        for (int i = 2; i < 88; i++) crc ^= report[i];
        report[88] = crc;

        setup[0] = 0x21; setup[1] = 0x09;
        setup[2] = 0x00; setup[3] = 0x03;
        setup[4] = 0x00; setup[5] = 0x00;
        setup[6] = (uint8_t)(RAZER_USB_REPORT_LEN & 0xFF);
        setup[7] = (uint8_t)(RAZER_USB_REPORT_LEN >> 8);

        poll_xfer->num_bytes = 8 + RAZER_USB_REPORT_LEN;
        poll_xfer->device_handle = devHandle;
        poll_xfer->bEndpointAddress = 0;
        poll_xfer->callback = [](usb_transfer_t* t) {
          LOG.printf("[RAZER] SET_POLL_RATE response: status=%d\n", t->status);
          usb_host_transfer_free(t);
        };

        err = usb_host_transfer_submit_control(clientHandle, poll_xfer);
        if (err != ESP_OK) {
          LOG.printf("[RAZER] Poll rate transfer failed: 0x%x\n", err);
          usb_host_transfer_free(poll_xfer);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
      }
    }

    // ── Set initial LED color from current preset ──
    vTaskDelay(pdMS_TO_TICKS(50));
    razerSetAllLeds(kalmanPresets[currentPreset].ledR,
                    kalmanPresets[currentPreset].ledG,
                    kalmanPresets[currentPreset].ledB);
    LOG.printf("[RAZER] LED set to %s color\n", kalmanPresets[currentPreset].name);
  }

  // Get active configuration descriptor
  const usb_config_desc_t* config_desc;
  err = usb_host_get_active_config_descriptor(devHandle, &config_desc);
  if (err != ESP_OK) {
    LOG.printf("[USB] Get config desc failed: 0x%x\n", err);
    usb_host_device_close(clientHandle, devHandle);
    return;
  }

  // Walk through all interface descriptors looking for HID class
  numHidIfaces = 0;
  int offset = 0;
  const uint8_t* p = (const uint8_t*)config_desc;
  int total_len = config_desc->wTotalLength;

  while (offset < total_len && numHidIfaces < MAX_HID_IFACES) {
    const usb_intf_desc_t* intf = (const usb_intf_desc_t*)(p + offset);

    // Check if this is an interface descriptor with HID class
    if (intf->bLength >= sizeof(usb_intf_desc_t) &&
        intf->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE &&
        intf->bInterfaceClass == USB_CLASS_HID) {

      uint8_t iface_num  = intf->bInterfaceNumber;
      uint8_t proto      = intf->bInterfaceProtocol;  // 1=kbd, 2=mouse
      uint8_t subclass   = intf->bInterfaceSubClass;

      LOG.printf("[USB] Found HID interface %d: subclass=%d protocol=%d endpoints=%d\n",
                    iface_num, subclass, proto, intf->bNumEndpoints);

      // Claim the interface
      err = usb_host_interface_claim(clientHandle, devHandle, iface_num, 0);
      if (err != ESP_OK) {
        LOG.printf("[USB] Claim interface %d failed: 0x%x\n", iface_num, err);
        offset += intf->bLength;
        continue;
      }

      // Find interrupt IN endpoint in the following descriptors
      int ep_offset = offset + intf->bLength;
      uint8_t ep_addr = 0;
      uint16_t ep_mps = 0;

      while (ep_offset < total_len) {
        const usb_ep_desc_t* ep = (const usb_ep_desc_t*)(p + ep_offset);

        if (ep->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
          break;  // Hit next interface — stop
        }

        if (ep->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT &&
            (ep->bEndpointAddress & 0x80) &&  // IN endpoint
            (ep->bmAttributes & 0x03) == USB_TRANSFER_TYPE_INTR) {
          ep_addr = ep->bEndpointAddress;
          ep_mps  = ep->wMaxPacketSize;
          LOG.printf("[USB]   IN EP 0x%02X mps=%d\n", ep_addr, ep_mps);
          break;
        }

        ep_offset += ep->bLength;
      }

      if (ep_addr != 0) {
        // Set idle (suppress repeated reports when nothing changes)
        usb_transfer_t* ctrl_xfer = NULL;
        usb_host_transfer_alloc(64, 0, &ctrl_xfer);
        if (ctrl_xfer) {
          ctrl_xfer->num_bytes = 8;
          ctrl_xfer->data_buffer[0] = USB_BM_REQUEST_TYPE_DIR_OUT |
                                      USB_BM_REQUEST_TYPE_TYPE_CLASS |
                                      USB_BM_REQUEST_TYPE_RECIP_INTERFACE;
          ctrl_xfer->data_buffer[1] = USB_HID_SET_IDLE;
          ctrl_xfer->data_buffer[2] = 0; // Report ID
          ctrl_xfer->data_buffer[3] = 0; // Duration (0 = infinite)
          ctrl_xfer->data_buffer[4] = iface_num;
          ctrl_xfer->data_buffer[5] = 0;
          ctrl_xfer->data_buffer[6] = 0; // wLength
          ctrl_xfer->data_buffer[7] = 0;
          ctrl_xfer->device_handle = devHandle;
          ctrl_xfer->bEndpointAddress = 0;
          ctrl_xfer->callback = [](usb_transfer_t* t) {
            usb_host_transfer_free(t);
          };
          usb_host_transfer_submit_control(clientHandle, ctrl_xfer);
        }

        // SET_PROTOCOL(1) = Report Protocol (not boot)
        // Boot protocol doesn't send middle button on Naga X
        {
          usb_transfer_t* proto_xfer = NULL;
          usb_host_transfer_alloc(64, 0, &proto_xfer);
          if (proto_xfer) {
            proto_xfer->num_bytes = 8;
            proto_xfer->data_buffer[0] = USB_BM_REQUEST_TYPE_DIR_OUT |
                                         USB_BM_REQUEST_TYPE_TYPE_CLASS |
                                         USB_BM_REQUEST_TYPE_RECIP_INTERFACE;
            proto_xfer->data_buffer[1] = USB_HID_SET_PROTOCOL;  // 0x0B
            proto_xfer->data_buffer[2] = 1;    // wValue low = 1 (Report Protocol)
            proto_xfer->data_buffer[3] = 0;    // wValue high
            proto_xfer->data_buffer[4] = iface_num;  // wIndex = interface
            proto_xfer->data_buffer[5] = 0;
            proto_xfer->data_buffer[6] = 0;    // wLength = 0
            proto_xfer->data_buffer[7] = 0;
            proto_xfer->device_handle = devHandle;
            proto_xfer->bEndpointAddress = 0;
            proto_xfer->callback = [](usb_transfer_t* t) {
              LOG.printf("[USB] SET_PROTOCOL response: status=%d\n", t->status);
              usb_host_transfer_free(t);
            };
            usb_host_transfer_submit_control(clientHandle, proto_xfer);
            LOG.printf("[USB] SET_PROTOCOL(Report) sent to iface %d\n", iface_num);
          }
        }

        // Allocate and submit interrupt IN transfer for polling
        usb_transfer_t* in_xfer = NULL;
        err = usb_host_transfer_alloc(ep_mps, 0, &in_xfer);
        if (err == ESP_OK && in_xfer) {
          in_xfer->num_bytes = ep_mps;
          in_xfer->device_handle = devHandle;
          in_xfer->bEndpointAddress = ep_addr;
          in_xfer->callback = usbTransferCallback;
          in_xfer->timeout_ms = 0;  // No timeout for interrupt

          hidIfaces[numHidIfaces].active    = true;
          hidIfaces[numHidIfaces].iface_num = iface_num;
          hidIfaces[numHidIfaces].ep_addr   = ep_addr;
          hidIfaces[numHidIfaces].ep_mps    = ep_mps;
          hidIfaces[numHidIfaces].protocol  = proto;
          hidIfaces[numHidIfaces].xfer      = in_xfer;

          err = usb_host_transfer_submit(in_xfer);
          if (err == ESP_OK) {
            LOG.printf("[USB] Polling interface %d (proto=%d) on EP 0x%02X\n",
                          iface_num, proto, ep_addr);
            numHidIfaces++;
          } else {
            LOG.printf("[USB] Transfer submit failed: 0x%x\n", err);
            usb_host_transfer_free(in_xfer);
          }
        }
      }
    }

    offset += intf->bLength;
    if (intf->bLength == 0) break;  // Safety
  }

  if (numHidIfaces > 0) {
    usbDeviceConnected = true;
    LOG.printf("[USB] Claimed %d HID interface(s)\n", numHidIfaces);
  } else {
    LOG.println("[USB] No HID interfaces found");
    usb_host_device_close(clientHandle, devHandle);
  }
}

// ─── USB Host Client Event Callback ─────────────────────────
static void usbClientEventCallback(const usb_host_client_event_msg_t* msg,
                                    void* arg) {
  switch (msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
      LOG.printf("[USB] New device addr=%d\n", msg->new_dev.address);
      usbDevAddr = msg->new_dev.address;
      break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
      LOG.println("[USB] Device disconnected — cleaning up");
      // Free all interrupt transfers
      for (int i = 0; i < numHidIfaces; i++) {
        if (hidIfaces[i].xfer) {
          usb_host_transfer_free(hidIfaces[i].xfer);
          hidIfaces[i].xfer = NULL;
        }
        if (hidIfaces[i].active && devHandle) {
          usb_host_interface_release(clientHandle, devHandle, hidIfaces[i].iface_num);
        }
        hidIfaces[i].active = false;
      }
      // Close device
      if (devHandle) {
        usb_host_device_close(clientHandle, devHandle);
        devHandle = NULL;
      }
      usbDeviceConnected = false;
      numHidIfaces = 0;
      usbDevAddr = 0;
      memset(hidIfaces, 0, sizeof(hidIfaces));
      // Reset accumulators
      accumX = 0; accumY = 0; accumW = 0; accumBtn = 0;
      mouseDirty = false;
      LOG.println("[USB] Ready for reconnection");
      break;
    default:
      break;
  }
}

// ─── USB Host Task (runs on Core 0) ──────────────────────────
static void usbHostLibTask(void* pvParameters) {
  // USB Host Library event processing
  while (true) {
    uint32_t event_flags;
    usb_host_lib_handle_events(portMAX_DELAY, &event_flags);

    if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
      usb_host_device_free_all();
    }
  }
}

static void usbClientTask(void* pvParameters) {
  LOG.println("[USB] Host task started");

  // Install USB Host Library
  // NOTE: Arduino USB Mode must be set to "Disabled" so TinyUSB
  //       doesn't claim the OTG peripheral before we can use it.
  const usb_host_config_t host_config = {
    .skip_phy_setup = false,
    .intr_flags = ESP_INTR_FLAG_LEVEL1,
  };
  esp_err_t err = usb_host_install(&host_config);
  if (err != ESP_OK) {
    LOG.printf("[USB] usb_host_install FAILED: 0x%x\n", err);
    LOG.println("[USB] >>> Check: Tools > USB Mode > \"Hardware CDC and JTAG\" <<<");
    vTaskDelete(NULL);
    return;
  }
  LOG.println("[USB] usb_host_install OK");

  // Start lib event task
  xTaskCreatePinnedToCore(usbHostLibTask, "usb_lib", 4096, NULL, 6, NULL, 0);
  LOG.println("[USB] lib event task started");

  // Register USB Host client
  const usb_host_client_config_t client_config = {
    .is_synchronous = false,
    .max_num_event_msg = 5,
    .async = {
      .client_event_callback = usbClientEventCallback,
      .callback_arg = NULL,
    }
  };
  err = usb_host_client_register(&client_config, &clientHandle);
  if (err != ESP_OK) {
    LOG.printf("[USB] client_register FAILED: 0x%x\n", err);
    vTaskDelete(NULL);
    return;
  }

  LOG.println("[USB] Client registered — plug in Naga X to the USB-OTG port!");
  LOG.println("[USB] (NOT the UART/serial port — use the other USB-C connector)");

  // Client event loop
  while (true) {
    // Use short timeout so we can print periodic heartbeat
    esp_err_t evt_err = usb_host_client_handle_events(clientHandle, pdMS_TO_TICKS(1000));

    // Open device when newly detected
    if (usbDevAddr != 0 && !usbDeviceConnected) {
      LOG.printf("[USB] Opening device at addr=%d...\n", usbDevAddr);
      vTaskDelay(pdMS_TO_TICKS(200));  // Brief settle time
      openHidDevice(usbDevAddr);
    }
  }
}

// ─── Setup ────────────────────────────────────────────────────
void setup() {
  LOG.begin(115200);
  delay(500);
  LOG.println("\n=== Vendit Naga X — ESP32-S3 USB-OTG BLE Bridge ===");

  // Init interface tracking
  memset(hidIfaces, 0, sizeof(hidIfaces));

  // Create report queue
  // Start BLE on default core (Core 1)
  setupBLE();

  // Start USB Host on Core 0
  xTaskCreatePinnedToCore(usbClientTask, "usb_client", 8192, NULL, 5, NULL, 0);

  LOG.println("[INIT] Ready — Plug in Naga X to USB-OTG port");
}

// ─── Main Loop (Core 1) ─────────────────────────────────────
// All work happens in USB callback → dispatchHidReport → BLE notify
// loop() is idle.
void loop() {
  // Flush any remaining accumulated mouse data
  if (mouseDirty && bleConnected) {
    unsigned long now = millis();
    if (now - lastMouseNotify >= BLE_MOUSE_INTERVAL_MS) {
      int16_t sendX = kalmanApply(&kfX, accumX);
      int16_t sendY = kalmanApply(&kfY, accumY);

      uint8_t mouseReport[6] = {0};
      mouseReport[0] = accumBtn;
      mouseReport[1] = (uint8_t)(sendX & 0xFF);
      mouseReport[2] = (uint8_t)(sendX >> 8);
      mouseReport[3] = (uint8_t)(sendY & 0xFF);
      mouseReport[4] = (uint8_t)(sendY >> 8);
      mouseReport[5] = accumW;

      mouseInput->setValue(mouseReport, sizeof(mouseReport));
      mouseInput->notify();

      accumX = 0;
      accumY = 0;
      accumW = 0;
      mouseDirty = (fabsf(kfX.x) > 0.5f || fabsf(kfY.x) > 0.5f);
      lastMouseNotify = now;
    }
  }
  vTaskDelay(pdMS_TO_TICKS(1));
}
