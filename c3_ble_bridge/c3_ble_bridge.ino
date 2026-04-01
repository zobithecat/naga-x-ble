/*
 * ESP32-C3 BLE HID Bridge — "Vendit-Naga-X"
 *
 * Receives UART packets from Pico USB Host and re-transmits
 * as BLE HID Mouse (Inst 0) + Keyboard (Inst 1).
 *
 * UART Protocol (from Pico):
 *   0xAA | Instance(1B) | Length(1B) | Data(N bytes) | 0xBB
 *
 * Pinout:
 *   UART RX: GPIO 20  (from Pico TX GP0)
 *   UART TX: GPIO 21  (to Pico RX GP1)
 *
 * BLE HID Report Map:
 *   Report ID 1 — Mouse  (buttons, X, Y, wheel)
 *   Report ID 2 — Keyboard (modifier, reserved, keys[6])
 */

#include <NimBLEDevice.h>
#include <NimBLEHIDDevice.h>

// ─── Configuration ───────────────────────────────────────────────
#define UART_BAUD       115200
#define PACKET_HEADER   0xAA
#define PACKET_FOOTER   0xBB
#define MAX_HID_REPORT  64
#define DEVICE_NAME     "Vendit-Naga-X"

// UART pins
#define UART_RX_PIN     4
#define UART_TX_PIN     5

// ─── HID Report Map ─────────────────────────────────────────────
// Combined Mouse (ID=1) + Keyboard (ID=2)
static const uint8_t hidReportMap[] = {
  // ── Mouse (Report ID 1) ──────────────────────────────────────
  0x05, 0x01,       // Usage Page (Generic Desktop)
  0x09, 0x02,       // Usage (Mouse)
  0xA1, 0x01,       // Collection (Application)
  0x85, 0x01,       //   Report ID (1)
  0x09, 0x01,       //   Usage (Pointer)
  0xA1, 0x00,       //   Collection (Physical)

  // 5 buttons (Naga X has at least 5 on body)
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

  // X, Y movement (16-bit each for high DPI)
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

// ─── Globals ────────────────────────────────────────────────────
static NimBLEHIDDevice*           hid        = nullptr;
static NimBLECharacteristic*      mouseInput = nullptr;
static NimBLECharacteristic*      keyInput   = nullptr;
static volatile bool              bleConnected = false;

// UART receive state machine
enum RxState { WAIT_HEADER, READ_INSTANCE, READ_LEN, READ_DATA, WAIT_FOOTER };
static RxState   rxState = WAIT_HEADER;
static uint8_t   rxInstance;
static uint8_t   rxLen;
static uint8_t   rxBuf[MAX_HID_REPORT];
static uint8_t   rxIdx;

// ─── BLE Callbacks ──────────────────────────────────────────────
class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
    bleConnected = true;
    Serial.println("[BLE] Client connected");
    // Allow additional connections / update params
    pServer->updateConnParams(connInfo.getConnHandle(), 6, 6, 0, 200);
  }

  void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
    bleConnected = false;
    Serial.printf("[BLE] Client disconnected (reason=%d)\r\n", reason);
    NimBLEDevice::startAdvertising();
  }
};

// ─── Setup ──────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  // UART from Pico
  Serial1.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);

  // ── NimBLE Init ──
  NimBLEDevice::init(DEVICE_NAME);
  NimBLEDevice::setSecurityAuth(true, false, false);  // bonding, no MITM, no SC
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  NimBLEServer* pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  // ── HID Service ──
  hid = new NimBLEHIDDevice(pServer);
  hid->setManufacturer("Vendit");
  hid->setPnp(0x02, 0x1532, 0x0084, 0x0100);  // Razer VID/PID placeholder
  hid->setHidInfo(0x00, 0x01);                  // Country=0, Flags=RemoteWake

  // Set the combined report map
  hid->setReportMap((uint8_t*)hidReportMap, sizeof(hidReportMap));

  // Create input report characteristics
  // Report ID 1 = Mouse, Report ID 2 = Keyboard
  mouseInput = hid->getInputReport(1);
  keyInput   = hid->getInputReport(2);

  // Start HID service
  hid->startServices();

  // ── Advertising ──
  NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
  pAdv->setAppearance(HID_MOUSE);
  pAdv->addServiceUUID(hid->getHidService()->getUUID());
  pAdv->start();

  Serial.println("[BLE] Advertising as " DEVICE_NAME);
}

// ─── UART Packet Parser (State Machine) ─────────────────────────
static void processUartByte(uint8_t b) {
  switch (rxState) {
    case WAIT_HEADER:
      if (b == PACKET_HEADER) rxState = READ_INSTANCE;
      break;

    case READ_INSTANCE:
      rxInstance = b;
      rxState = READ_LEN;
      break;

    case READ_LEN:
      rxLen = b;
      if (rxLen == 0 || rxLen > MAX_HID_REPORT) {
        rxState = WAIT_HEADER;  // Invalid length, reset
      } else {
        rxIdx = 0;
        rxState = READ_DATA;
      }
      break;

    case READ_DATA:
      rxBuf[rxIdx++] = b;
      if (rxIdx >= rxLen) rxState = WAIT_FOOTER;
      break;

    case WAIT_FOOTER:
      if (b == PACKET_FOOTER) {
        // Valid packet received — dispatch
        dispatchHidReport(rxInstance, rxBuf, rxLen);
      }
      // Either way, go back to waiting for next packet
      rxState = WAIT_HEADER;
      break;
  }
}

// ─── HID Report Dispatch ────────────────────────────────────────
static void dispatchHidReport(uint8_t instance, const uint8_t* data, uint8_t len) {
  if (!bleConnected) return;

  if (instance == 0) {
    // ── Mouse Report (Inst 0) ──
    // Naga X standard mouse report is typically:
    //   [0] buttons, [1-2] X (16-bit LE), [3-4] Y (16-bit LE), [5] wheel
    // We re-pack into our Report ID 1 format:
    //   buttons(1) | X(2) | Y(2) | wheel(1) = 6 bytes
    uint8_t mouseReport[6] = {0};

    if (len >= 6) {
      mouseReport[0] = data[0];        // Buttons
      mouseReport[1] = data[1];        // X low
      mouseReport[2] = data[2];        // X high
      mouseReport[3] = data[3];        // Y low
      mouseReport[4] = data[4];        // Y high
      mouseReport[5] = data[5];        // Wheel
    } else if (len >= 4) {
      // Fallback: some reports may be shorter (8-bit X/Y)
      mouseReport[0] = data[0];        // Buttons
      mouseReport[1] = data[1];        // X low
      mouseReport[2] = (data[1] & 0x80) ? 0xFF : 0x00;  // Sign extend
      mouseReport[3] = data[2];        // Y low
      mouseReport[4] = (data[2] & 0x80) ? 0xFF : 0x00;  // Sign extend
      if (len >= 5) mouseReport[5] = data[3]; // Wheel
    }

    mouseInput->setValue(mouseReport, sizeof(mouseReport));
    mouseInput->notify();

  } else if (instance == 1) {
    // ── Keyboard Report (Inst 1) ──
    // Naga X side buttons come as keyboard HID:
    //   [0] modifier, [1] reserved, [2..7] keycodes
    // Our Report ID 2 format is identical: 8 bytes
    uint8_t keyReport[8] = {0};
    uint8_t copyLen = (len > 8) ? 8 : len;
    memcpy(keyReport, data, copyLen);

    keyInput->setValue(keyReport, sizeof(keyReport));
    keyInput->notify();
  }
  // Other instances are silently ignored
}

// ─── Main Loop ──────────────────────────────────────────────────
static unsigned long lastCheck = 0;

void loop() {
  // Drain UART RX buffer through state machine
  while (Serial1.available()) {
    uint8_t b = (uint8_t)Serial1.read();
    Serial.printf("[RX] 0x%02X\n", b);  // 디버그
    processUartByte(b);
  }

  // 5초마다 UART 상태 체크
  if (millis() - lastCheck > 5000) {
    lastCheck = millis();
    Serial.println("[DBG] Waiting for UART data...");
  }

  delay(1);
}
