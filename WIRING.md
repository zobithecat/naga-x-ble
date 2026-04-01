# Naga X BLE Bridge — Wiring Guide

---

## Option A: ESP32-S3 Single Board (New — Recommended)

### Overview
```
  Naga X ──USB──▶ ESP32-S3 DevKitC-1 ──BLE──▶ PC / Tablet
                  (USB-OTG Host port)
```

단일 보드로 USB Host + BLE HID 브릿지 구현. UART 브릿지 불필요.

### USB-OTG Connection (Naga X → ESP32-S3)

| ESP32-S3 USB-OTG Port | Signal  | Naga X USB |
|------------------------|---------|------------|
| D+                     | USB D+  | Green wire |
| D-                     | USB D-  | White wire |
| VBUS (5V)              | USB 5V  | Red wire   |
| GND                    | USB GND | Black wire |

> **주의**: USB-OTG 포트에 OTG 어댑터(USB-A Female → Micro-USB/USB-C Male)를
> 사용하여 Naga X를 연결합니다. 또는 USB-A Female 브레이크아웃 보드 사용 가능.

### Power Supply

1. **ESP32-S3**: USB-C (UART 포트) 또는 5V/GND 핀으로 전원 공급
2. **Naga X**: USB-OTG 포트의 VBUS에서 5V 공급 (500mA 이상 보장 필요)
3. 두 USB 포트 동시 사용 시: UART 포트로 전원+시리얼, OTG 포트로 Naga X Host

### Arduino IDE Settings (ESP32-S3)

- Board: "ESP32S3 Dev Module"
- USB Mode: "USB-OTG (TinyUSB)"
- PSRAM: "OPI PSRAM" (보드에 있으면)
- Partition Scheme: "Default 4MB with spiffs"
- Library: NimBLE-Arduino v1.4+

### Firmware

→ `s3_usb_ble_bridge/s3_usb_ble_bridge.ino`

---

## Option B: Pico + ESP32-C3 Two-Board (Legacy)

### Board Connections

```
  RPi Pico                    ESP32-C3
  ┌──────────┐                ┌──────────┐
  │ GP0 (TX) │───────────────▶│ GPIO20(RX)│
  │ GP1 (RX) │◀───────────────│ GPIO21(TX)│
  │   GND    │────────┬───────│   GND    │
  └──────────┘        │       └──────────┘
                      │
                   Common GND
```

## UART (Serial1) — 115200 bps, 8N1

| Pico Pin | Direction | ESP32-C3 Pin | Signal |
|----------|-----------|-------------|--------|
| GP0 (TX) | ──▶       | GPIO 20 (RX)| UART Data (Pico → ESP) |
| GP1 (RX) | ◀──       | GPIO 21 (TX)| UART Data (ESP → Pico) |
| GND      | ───       | GND         | Common Ground (필수!)  |

## USB Host (PIO USB) — Pico Side

| Pico Pin | Signal   | Naga X USB |
|----------|----------|------------|
| GP2      | USB D+   | Green wire |
| GP3      | USB D-   | White wire |
| VBUS(5V) | USB 5V   | Red wire   |
| GND      | USB GND  | Black wire |

> **주의**: Naga X는 전력 소모가 크므로 Pico의 VBUS(5V)에서
> 직접 공급하거나, 외부 5V 전원을 USB 커넥터에 공급해야 합니다.
> Pico 3.3V 출력으로는 부족합니다.

## Power Supply Notes

1. **Pico**: USB-C 또는 Micro-USB로 5V 공급 (PC 또는 충전기)
2. **ESP32-C3**: 별도 USB-C로 5V 공급 (또는 Pico의 VSYS에서 공급 가능)
3. **Naga X**: Pico의 VBUS 핀(5V)에서 공급 — 500mA 이상 보장 필요
4. **GND 공유**: 세 보드(Pico, ESP32-C3, Naga X USB) 모두 GND를 공유해야 함

## UART Protocol

```
Packet: [0xAA] [Instance] [Length] [Data...] [0xBB]

- 0xAA     : Header byte
- Instance : 0 = Mouse, 1 = Keyboard (side buttons)
- Length   : HID report data length (1~64)
- Data     : Raw HID report bytes
- 0xBB     : Footer byte
```

### Arduino IDE Settings (Legacy)

#### Pico (USB Host)
- Board: "Raspberry Pi Pico"
- Core: Earle Philhower (arduino-pico)
- USB Stack: "Adafruit TinyUSB"
- CPU Speed: 120MHz+ 권장

#### ESP32-C3 (BLE Bridge)
- Board: "ESP32C3 Dev Module"
- Partition Scheme: "Default 4MB with spiffs"
- Library: NimBLE-Arduino v1.4+
