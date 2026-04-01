# Naga X BLE Bridge

Razer Naga Left-Handed Edition (PID 0x008D) USB-to-BLE HID bridge using ESP32-S3.

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
- **Scroll Debounce**: Direction-lock filter suppresses encoder backlash/chatter
  - 80ms direction lock window
  - Requires 2 consecutive reverse events to change direction

### Button Mapping

| Input | Action |
|-------|--------|
| Left click | Left click |
| Right click | Right click |
| Middle click (wheel) | Mission Control (Ctrl+Up) / Normal middle click (toggleable) |
| Back (side) | Back |
| Forward (side) | Forward |
| Side button 1 | Cmd+V (Paste) |
| Side button 3 | Cmd+C (Copy) |
| Side buttons 2, 4-9 | Number keys (passthrough) |
| Side 10 + 11 (0 + -) | Toggle middle button mode |

### Middle Button Modes
- **Mode 0** (default): Mission Control (Ctrl + Up Arrow)
- **Mode 1**: Normal middle click

Toggle by pressing side buttons 10 and 11 simultaneously.

## Hardware

### Required
- ESP32-S3 DevKitC-1
- Razer Naga Left-Handed Edition 2020 (USB)
- USB-C to USB-A Female OTG adapter

### Connections
- **USB-OTG port**: Naga X (via OTG adapter)
- **UART port**: Power supply (5V USB-C) + Serial monitor

## Arduino IDE Settings

| Setting | Value |
|---------|-------|
| Board | ESP32S3 Dev Module |
| USB Mode | Hardware CDC and JTAG |
| USB CDC on Boot | Disabled |
| Partition Scheme | Default 4MB with spiffs |

### Required Libraries
- NimBLE-Arduino (v1.4+)

## Configuration

Edit defines at the top of `s3_usb_ble_bridge.ino`:

```c
#define RAZER_DPI_VALUE          800   // Mouse DPI (100-20000)
#define BLE_MOUSE_INTERVAL_MS    8     // BLE notify rate (~125Hz)
#define KALMAN_Q                 0.5f  // Process noise (higher = more responsive)
#define KALMAN_R                 3.0f  // Measurement noise (higher = smoother)
#define SCROLL_DIR_LOCK_MS       80    // Scroll direction lock window (ms)
#define SCROLL_REVERSE_COUNT     2     // Reverse events needed to change scroll direction
```

## Razer Protocol

The firmware sends three vendor-specific commands on USB connection:

1. **SET_DEVICE_MODE** (0x03) - Enables middle button and extended features
2. **SET_DPI** - Configures mouse sensitivity
3. **SET_POLL_RATE** (1000Hz) - Maximum USB report rate

These use Razer's 90-byte Feature Report protocol with transaction ID 0x1F.

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
