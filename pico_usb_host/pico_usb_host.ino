/*
 * Pico USB Host — Naga X BLE Bridge
 *
 * USB Host via PIO on GP4(D+)/GP5(D-)
 * UART TX via Serial1 on GP12(TX)/GP13(RX)
 *
 * Protocol: 0xAA | Instance | Len | Data... | 0xBB
 */

#include "pio_usb.h"
#include "Adafruit_TinyUSB.h"

Adafruit_USBH_Host USBHost;

// USB Host PIO pins
#define PIN_USB_HOST_DP  4

// UART packet protocol
#define PACKET_HEADER    0xAA
#define PACKET_FOOTER    0xBB
#define MAX_REPORT       64

// Volatile buffer for ISR-safe transfer
static volatile bool     reportReady = false;
static volatile uint8_t  reportInstance;
static volatile uint8_t  reportLen;
static volatile uint8_t  reportBuf[MAX_REPORT];

// ─── USB HID Callbacks ─────────────────────────────────────
extern "C" {

void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance,
                      uint8_t const* desc_report, uint16_t desc_len) {
  Serial.printf("[USB] Connected! dev=%d inst=%d\n", dev_addr, instance);
  tuh_hid_receive_report(dev_addr, instance);
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance) {
  Serial.printf("[USB] Disconnected! dev=%d inst=%d\n", dev_addr, instance);
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance,
                                uint8_t const* report, uint16_t len) {
  if (!reportReady && len > 0 && len <= MAX_REPORT) {
    for (uint16_t i = 0; i < len; i++) {
      reportBuf[i] = report[i];
    }
    reportLen = len;
    reportInstance = instance;
    reportReady = true;
  }
  tuh_hid_receive_report(dev_addr, instance);
}

} // extern "C"

// ─── Setup ─────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);

  Serial.println("\n=== Naga X BLE Bridge - Pico USB Host ===");

  // USB Host on PIO (GP4/GP5)
  pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
  pio_cfg.pin_dp = PIN_USB_HOST_DP;
  USBHost.configure_pio_usb(1, &pio_cfg);
  USBHost.begin(1);
  Serial.println("[USB] Host on GP4/GP5 ready");

  // UART0 on GP12(TX)/GP13(RX)
  Serial1.setTX(12);
  Serial1.setRX(13);
  Serial1.begin(115200);
  Serial.println("[UART] TX=GP12, RX=GP13 ready");

  Serial.println("Plug in Naga X!");
}

// ─── Loop ──────────────────────────────────────────────────
void loop() {
  USBHost.task();

  if (reportReady) {
    uint8_t inst = reportInstance;
    uint8_t len  = reportLen;
    uint8_t buf[MAX_REPORT];
    for (uint8_t i = 0; i < len; i++) buf[i] = reportBuf[i];
    reportReady = false;

    // Send UART packet: AA | inst | len | data | BB
    Serial1.write(PACKET_HEADER);
    Serial1.write(inst);
    Serial1.write(len);
    Serial1.write(buf, len);
    Serial1.write(PACKET_FOOTER);

    // Debug
    Serial.printf("[TX] inst=%d len=%d: ", inst, len);
    for (uint8_t i = 0; i < len; i++) Serial.printf("%02X ", buf[i]);
    Serial.println();
  }
}
