# Naga X BLE Bridge

Razer Naga Left-Handed Edition (RZ01-0341, PID 0x008D) USB-to-BLE HID bridge using ESP32-S3.

> **한국어**: Razer Naga 왼손잡이 에디션을 ESP32-S3 하나로 BLE 무선 마우스로 변환하는 펌웨어입니다.
> macOS에서 Synapse 미지원, Karabiner 매핑 불가 문제를 해결합니다.

## Overview

```
Razer Naga X ──USB──> ESP32-S3 (USB-OTG Host) ──BLE──> Mac / iPad / PC
```

Single ESP32-S3 DevKitC-1 board replaces the original two-board setup (Raspberry Pi Pico + ESP32-C3).

## Features

### USB Host
- ESP-IDF native `usb_host` API (no TinyUSB dependency)
- 4 HID interfaces claimed (mouse, keyboard, Razer proprietary, consumer)
- Razer Driver Mode activation (enables wheel click via vendor protocol)
- Configurable DPI (default 800, range 100-20000)
- 1000Hz USB polling rate
- **Hot-plug support**: USB disconnect/reconnect without reboot

### BLE HID
- NimBLE-Arduino, 7.5ms connection interval
- Mouse: 5 buttons (left, right, middle, back, forward) + 16-bit X/Y + scroll wheel
- Keyboard: side button passthrough (12 buttons)
- Combined HID Report Map (Report ID 1 = Mouse, Report ID 2 = Keyboard)

### Signal Processing
- **Decimation**: USB 1000Hz -> BLE ~125Hz with X/Y delta accumulation
- **Kalman Filter**: 1D Kalman filter per axis for adaptive cursor smoothing
  - Steady motion: smooth trajectory with fast convergence
  - Direction change: high responsiveness via adaptive Kalman gain
  - Sub-pixel remainder tracking for precise slow movement
  - 5 presets switchable via DPI buttons with LED color feedback
  - macOS pointer acceleration OFF recommended for best results
- **Scroll Filter**: Leaky integrator absorbs encoder bounce/chatter
  - Per-event decay (0.85) + threshold (3.5)
  - Biased signal passes, unbiased noise self-cancels

### Button Mapping

#### Physical Button Map (물리 버튼 맵)

| Physical Button | HID Bit | Mapped Action | 설명 |
|----------------|---------|---------------|------|
| Left click | 0x01 | Left click | 좌클릭 |
| Right click | 0x02 | Right click | 우클릭 |
| Wheel left button | 0x40 | Mission Control (Ctrl+Up) | 미션 컨트롤 |
| Wheel right button | 0x20 | Back | 뒤로가기 |
| Wheel both buttons | 0x04 | Middle click | 중간클릭 (Onshape orbit) |
| Scroll wheel | byte[3] | Scroll (filtered) | 스크롤 (leaky integrator) |
| Side button 1 | 0x1E | Cmd+V (Paste) | 붙여넣기 |
| Side button 2 | 0x1F | Cmd+R (Reload) | 새로고침 |
| Side button 3 | 0x20 | Cmd+C (Copy) | 복사 |
| Side buttons 4-9 | 0x21-0x26 | Number keys | 숫자키 패스스루 |
| Side 10, 11, 12 | 0x27,0x2D,0x2E | 0, -, = keys | 숫자키 패스스루 |
| DPI Up button | iface1: 0x04/0x20 | Kalman preset up | 칼만 프리셋 ▲ |
| DPI Down button | iface1: 0x04/0x21 | Kalman preset down | 칼만 프리셋 ▼ |

#### Arrow Mode (방향키 모드 — 터미널/FPS용)

Side 11+12 동시 누르기로 토글. 한 손으로 마우스 + 방향키 조작 가능.

| Input | Action | 설명 |
|-------|--------|------|
| Side button 2 | Up Arrow | 위 |
| Side button 4 | Left Arrow | 왼쪽 |
| Side button 5 | Down Arrow | 아래 |
| Side button 6 | Right Arrow | 오른쪽 |

#### Mode Toggles (모드 전환)

| Combo | Action | 설명 |
|-------|--------|------|
| Side 11 + 12 (- + =) | Toggle arrow key mode | 사이드 2,4,5,6을 방향키로 전환 |
| DPI Up / Down | Cycle Kalman preset | 칼만 프리셋 순환 + LED 색상 변경 |

#### Kalman Presets (DPI 버튼으로 전환, LED 색상 피드백)

| Preset | Q | R | LED | Feel |
|--------|---|---|-----|------|
| Office | 1.0 | 3.0 | Blue | 부드러움 |
| Balanced | 2.0 | 2.0 | Green | 균형 |
| Responsive | 3.0 | 1.0 | Yellow | 반응형 |
| Gaming | 5.0 | 0.5 | Red | 거의 raw |
| Raw | 10.0 | 0.1 | Purple | 무보정 |

> Tip: macOS에서 시스템 설정 > 포인터 가속 끄기를 권장합니다.

---

## Razer Naga Left-Handed Edition Protocol Reference

### Device Identity

| Field | Value |
|-------|-------|
| VID | 0x1532 (Razer Inc.) |
| PID | 0x008D |
| Model | RZ01-0341 |
| Name | Naga Left-Handed Edition 2020 |
| Sensor | Razer Focus+ Optical, max 20000 DPI |
| Buttons | Left, Right, Wheel Left, Wheel Right, Wheel Both, 12 side, 2 DPI |

### USB HID Interfaces

The mouse enumerates 4 HID interfaces:

| Interface | Subclass | Protocol | EP | MPS | Purpose |
|-----------|----------|----------|----|-----|---------|
| 0 | 1 (Boot) | 2 (Mouse) | 0x81 IN | 8 | Mouse movement + buttons |
| 1 | 0 | 1 (Keyboard) | 0x82 IN | 16 | Razer proprietary (DPI events, config) |
| 2 | 1 (Boot) | 1 (Keyboard) | 0x83 IN | 8 | Side buttons (keyboard keycodes) |
| 3 | 0 | 2 (Mouse) | 0x84 IN | 2 | Unknown (possibly consumer control) |

### Interface 0 — Mouse Report (8 bytes)

```
Byte:  [0]      [1]    [2]    [3]     [4]    [5]    [6]    [7]
Field: buttons  X(8b)  Y(8b)  wheel   X_lo   X_hi   Y_lo   Y_hi
                                       ← 16-bit LE →  ← 16-bit LE →
```

- Bytes 1-2: 8-bit signed X/Y (boot protocol compatibility)
- Bytes 4-7: 16-bit signed X/Y little-endian (full DPI resolution)
- Byte 3: Scroll wheel, signed 8-bit (+1=down, -1=up, multi-tick possible)

#### Button Bit Map (byte 0)

| Bit | Hex | Physical Button |
|-----|-----|----------------|
| 0 | 0x01 | Left click |
| 1 | 0x02 | Right click |
| 2 | 0x04 | Wheel both buttons (HW combo) |
| 5 | 0x20 | Wheel right button |
| 6 | 0x40 | Wheel left button |

> **Critical**: Bit 2 (0x04) is sent by hardware when both wheel buttons are pressed simultaneously. Bits 5 and 6 are independent wheel side buttons. This is NOT standard HID — it's a Razer-specific button layout.

> **Driver Mode Required**: Without SET_DEVICE_MODE(0x03), the mouse does NOT send 0x04 (wheel click) at all. Only 0x01 and 0x02 are sent in Device Mode.

### Interface 1 — Razer Proprietary (16 bytes)

Report format: `[ReportID] [Keycode] [padding...]`

| Report ID | Keycode | Meaning |
|-----------|---------|---------|
| 0x04 | 0x20 | DPI Up button pressed |
| 0x04 | 0x21 | DPI Down button pressed |
| 0x04 | 0x00 | DPI button released |
| 0x05 | 0x02 XX XX ... | DPI status (XX XX = current DPI big-endian) |

DPI status example: `05 02 03 20 03 20` = X:800 Y:800 (0x0320 = 800)

### Interface 2 — Side Buttons (8 bytes, keyboard format)

Standard HID keyboard report: `[mod] [reserved] [key1] [key2] ... [key6]`

| Side Button | HID Keycode | Key |
|-------------|-------------|-----|
| 1 | 0x1E | 1 |
| 2 | 0x1F | 2 |
| 3 | 0x20 | 3 |
| 4 | 0x21 | 4 |
| 5 | 0x22 | 5 |
| 6 | 0x23 | 6 |
| 7 | 0x24 | 7 |
| 8 | 0x25 | 8 |
| 9 | 0x26 | 9 |
| 10 | 0x27 | 0 |
| 11 | 0x2D | - |
| 12 | 0x2E | = |

### Interface 3 — Unknown (2 bytes)

MPS=2, protocol=mouse. No meaningful data observed. Possibly battery/consumer control.

### Razer Vendor Command Protocol

90-byte Feature Report sent via USB Control Transfer.

#### Transport

| Parameter | Value |
|-----------|-------|
| bmRequestType | 0x21 (Class, Interface, OUT) |
| bRequest | 0x09 (SET_REPORT) |
| wValue | 0x0300 (Feature Report, ID 0) |
| wIndex | 0x0000 (Interface 0) |
| wLength | 90 (0x5A) |

#### Packet Structure

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 1 | status | 0x00 = new command |
| 1 | 1 | transaction_id | **0x1F** for this device |
| 2-3 | 2 | remaining_packets | 0x0000 |
| 4 | 1 | protocol_type | 0x00 |
| 5 | 1 | data_size | Payload byte count |
| 6 | 1 | command_class | Command category |
| 7 | 1 | command_id | Specific command |
| 8-87 | 80 | arguments | Command-specific payload |
| 88 | 1 | crc | XOR of bytes 2..87 |
| 89 | 1 | reserved | 0x00 |

#### Commands Used

**SET_DEVICE_MODE** — Enable Driver Mode (required for wheel click)

| Field | Value |
|-------|-------|
| data_size | 0x02 |
| command_class | 0x00 |
| command_id | 0x04 |
| args[0] | 0x03 (Driver Mode) |
| args[1] | 0x00 |

> Device Mode (0x00) = default, no wheel click HID events
> Driver Mode (0x03) = full button reporting including 0x04/0x20/0x40

**SET_DPI**

| Field | Value |
|-------|-------|
| data_size | 0x07 |
| command_class | 0x04 |
| command_id | 0x05 |
| args[0] | 0x01 (VARSTORE, persistent) |
| args[1-2] | X DPI (big-endian uint16) |
| args[3-4] | Y DPI (big-endian uint16) |
| args[5-6] | 0x00 |

DPI range: 100 — 20000. Example: 800 DPI = `0x03 0x20`

**SET_POLL_RATE**

| Field | Value |
|-------|-------|
| data_size | 0x01 |
| command_class | 0x00 |
| command_id | 0x05 |
| args[0] | Rate code |

| Code | Rate |
|------|------|
| 0x01 | 1000Hz |
| 0x02 | 500Hz |
| 0x08 | 125Hz |

**SET_LED_STATIC** — Set LED color

| Field | Value |
|-------|-------|
| data_size | 0x09 |
| command_class | 0x0F |
| command_id | 0x02 |
| args[0] | 0x01 (VARSTORE) |
| args[1] | LED ID |
| args[2] | 0x01 (Static effect) |
| args[3-4] | 0x00 |
| args[5] | 0x01 |
| args[6] | Red (0-255) |
| args[7] | Green (0-255) |
| args[8] | Blue (0-255) |

| LED ID | Zone |
|--------|------|
| 0x01 | Scroll wheel |
| 0x04 | Logo |
| 0x10 | Side (thumb grid) |

---

## Hardware

### Required
- ESP32-S3 DevKitC-1
- Razer Naga Left-Handed Edition 2020 (USB)
- USB-C to USB-A Female OTG adapter

### Connections
- **USB-OTG port**: Naga X (via OTG adapter)
- **UART port**: Power supply (5V USB-C) + Serial monitor

### Future: Battery Build (배터리 내장)
- Seeed XIAO ESP32S3 (21x17.5mm, LiPo charging built-in)
- 301230 LiPo battery (80mAh, ~1.5h) or larger
- ESP32-S3 Light Sleep (~2mA) on idle, USB interrupt wakeup

## Arduino IDE Settings

| Setting | Value |
|---------|-------|
| Board | ESP32S3 Dev Module |
| USB Mode | Hardware CDC and JTAG |
| USB CDC on Boot | Disabled |
| Partition Scheme | Default 4MB with spiffs |

### Required Libraries
- NimBLE-Arduino (v1.4+)

## Configuration (설정)

Edit defines at the top of `s3_usb_ble_bridge.ino`:

```c
// DPI & Polling
#define RAZER_DPI_VALUE          800   // Mouse DPI (100-20000)

// BLE
#define BLE_MOUSE_INTERVAL_MS    8     // BLE notify rate (~125Hz)

// Kalman Filter
#define KALMAN_Q                 3.0f  // Process noise (higher = responsive)
#define KALMAN_R                 1.0f  // Measurement noise (higher = smooth)

// Scroll Filter (Leaky Integrator)
#define SCROLL_DECAY             0.85f // Per-event decay
#define SCROLL_THRESH            3.5f  // Emit threshold (raise for broken encoders)
```

## Project Structure

```
naga-x-ble/
+-- s3_usb_ble_bridge/       <- Current firmware (ESP32-S3, single board)
|   +-- s3_usb_ble_bridge.ino
+-- c3_ble_bridge/            <- Legacy (ESP32-C3 BLE, two-board setup)
|   +-- c3_ble_bridge.ino
+-- pico_usb_host/            <- Legacy (Pico USB Host, two-board setup)
|   +-- pico_usb_host.ino
+-- WIRING.md                 <- Wiring guide for both setups
+-- README.md
```

## Future Ideas

- Battery-powered build with XIAO ESP32S3 (LiPo charging built-in)
- Light sleep on idle with USB interrupt wakeup
- USB-to-USB bridge mode via RP2040 for gaming latency (~1ms)
- Per-application profiles (auto-detect active app)
- NVS-stored button remapping configuration

## Acknowledgments

- [openrazer](https://github.com/openrazer/openrazer) — Razer vendor protocol reverse engineering
- [razer-macos](https://github.com/1kc/razer-macos) — macOS Razer driver reference

## License

MIT
