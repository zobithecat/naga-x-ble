# Naga X BLE Bridge

Razer Naga Left-Handed Edition (PID 0x008D) USB-to-BLE HID bridge using ESP32-S3.

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
- Razer Driver Mode activation (enables middle button via vendor protocol)
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
  - Tunable: Q (process noise / responsiveness), R (measurement noise / smoothness)
  - macOS pointer acceleration OFF recommended for best results
- **Scroll Debounce**: Direction-lock filter suppresses encoder backlash/chatter
  - 80ms direction lock window
  - Requires 2 consecutive reverse events to change direction

### Button Mapping

#### Default Mode (일반 모드)

| Input | Action | 설명 |
|-------|--------|------|
| Left click | Left click | 좌클릭 |
| Right click | Right click | 우클릭 |
| Middle click (wheel) | Mission Control (Ctrl+Up) | 미션 컨트롤 |
| Back (side) | Back | 뒤로가기 |
| Forward (side) | Forward | 앞으로가기 |
| Side button 1 | Cmd+V (Paste) | 붙여넣기 |
| Side button 2 | Cmd+R (Reload) | 새로고침 |
| Side button 3 | Cmd+C (Copy) | 복사 |
| Side buttons 4-9 | Number keys | 숫자키 패스스루 |
| Side 10, 11, 12 | 0, -, = keys | 숫자키 패스스루 |

#### Arrow Mode (방향키 모드 — 터미널/FPS용)

11+12 동시 누르기로 토글. 한 손으로 마우스 + 방향키 조작 가능.

| Input | Action | 설명 |
|-------|--------|------|
| Side button 2 | Up Arrow | 위 |
| Side button 4 | Left Arrow | 왼쪽 |
| Side button 5 | Down Arrow | 아래 |
| Side button 6 | Right Arrow | 오른쪽 |

#### Mode Toggles (모드 전환)

| Combo | Action | 설명 |
|-------|--------|------|
| Side 10 + 11 (0 + -) | Toggle middle button mode | 중간버튼: Mission Control <-> 일반 클릭 |
| Side 11 + 12 (- + =) | Toggle arrow key mode | 사이드 2,4,5,6을 방향키로 전환 |

### Middle Button Modes (중간버튼 모드)
- **Mode 0** (default): Mission Control (Ctrl + Up Arrow) — macOS Expose
- **Mode 1**: Normal middle click — Onshape/CAD 3D orbit

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
- Deep sleep on idle for extended battery life

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

// Kalman Filter (커서 스무딩)
#define KALMAN_Q                 3.0f  // Process noise (higher = responsive, lower = smooth)
#define KALMAN_R                 1.0f  // Measurement noise (higher = smooth, lower = raw)

// Scroll Debounce (스크롤 디바운스)
#define SCROLL_DIR_LOCK_MS       80    // Direction lock window (ms)
#define SCROLL_REVERSE_COUNT     2     // Reverse events to change direction
```

### Kalman Tuning Guide (칼만 필터 튜닝)

| Use Case | Q | R | Feel |
|----------|---|---|------|
| Office (사무용) | 1.0 | 3.0 | Smooth, slight lag |
| Balanced (균형) | 2.0 | 2.0 | Good all-round |
| Responsive (반응형) | 3.0 | 1.0 | Near-raw, slight smoothing |
| Gaming (게이밍) | 5.0 | 0.5 | Almost raw input |
| Raw (무보정) | 10.0 | 0.1 | No filtering |

> Tip: macOS에서 시스템 설정 > 포인터 가속 끄기를 권장합니다. 칼만 필터와 OS 가속이 이중으로 적용되면 둥실둥실한 느낌이 납니다.

## Razer Protocol

The firmware sends three vendor-specific commands on USB connection:

1. **SET_DEVICE_MODE** (0x03) - Switches to Driver Mode, enables middle button
2. **SET_DPI** - Configures mouse sensitivity (stored persistently on device)
3. **SET_POLL_RATE** (1000Hz) - Maximum USB report rate

These use Razer's 90-byte Feature Report protocol:
- Transaction ID: 0x1F (Naga Left-Handed 2020)
- USB Control Transfer: bmRequestType=0x21, bRequest=0x09, wValue=0x0300

> Without Driver Mode, the Naga does not send middle button (wheel click) via HID at all.
> This was discovered through reverse-engineering the openrazer project.

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
- Deep sleep on idle for battery life
- USB-to-USB bridge mode via RP2040 for gaming latency (~1ms)
- Profile switching (gaming vs office) via side button combo
- Per-application profiles (auto-detect active app)

## License

MIT
