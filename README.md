# Home Automation Panel — ESP32-S3-Touch-LCD-3.5-C

A touch control panel for the Waveshare ESP32-S3-Touch-LCD-3.5-C (320×480 ST7796
over SPI, FT6336 touch, OV5640 camera on the DVP port, TF card, 8 MB PSRAM /
16 MB flash). Built against **ESP-IDF v5.3.5**.

Twelve screens: splash, dashboard, camera, UART console, Modbus master, power
control, ten room switches, temperature, person/object detection, TF card
browser, video playback and settings.

---

## Before you flash: check the pins

The camera DVP pins are taken from Waveshare's wiki and are known good. **The
LCD, I²C, TF-card and RS485 pins are defaults, not verified facts** — Waveshare
publishes those only as symbolic names (`LCD_CS`, `SPI_MOSI`, `SD_MMC_CLK`…)
rather than GPIO numbers.

Check them against `ESP32-S3-Touch-LCD-3.5-Schematic.pdf` before the first
flash. Everything lives in one Kconfig menu, so no code changes are needed:

```
idf.py menuconfig  →  Board Support (ESP32-S3-Touch-LCD-3.5-C)
```

The firmware prints a loud warning at every boot until you tick
`BSP_PINS_VERIFIED`. If the screen stays dark or touch is dead, this is the
first thing to look at.

Confirmed camera pins: XCLK 38, PCLK 41, VSYNC 17, HREF 18, D0–D7 =
45/47/48/46/42/40/39/21, SIOD 8, SIOC 7.

## Build

```bash
idf.py set-target esp32s3
idf.py menuconfig        # verify the Board Support pins
idf.py build
idf.py -p COMx flash monitor
```

Managed components are pulled automatically: `esp_lcd_st7796`,
`esp_lcd_touch_ft5x06`, `esp_lvgl_port` 1.4, `lvgl` 8.3, `esp32-camera` 2.0,
`esp-modbus` 1.0.

Partitions (16 MB): 5 MB app, 2 MB `assets` SPIFFS, ~8.6 MB free for a later
OTA layout.

## Layout

```
main/                    boot sequence, auto-sleep
components/
  bsp/                   pins, I²C, TCA9554, ST7796+FT6336+LVGL, camera, TF card
  app_core/              event bus, NVS settings, RTC/SNTP clock, Wi-Fi
  svc_uart/              serial console + scrollback + log export
  svc_modbus/            RTU/TCP master, one serialised request path
  svc_switch/            10 switches: GPIO / expander / Modbus coil bindings
  svc_power/             meter polling, four rails, 24 h usage history
  svc_temp/              SHT3x or Modbus climate, setpoint write-back
  svc_media/             frame pump, snapshots, MJPEG-AVI record/play, browser
  svc_detect/            motion + blob detection, pluggable classifier
  svc_notify/            buzzer, CSV event log, HTTP webhook
  ui/                    12 LVGL screens + shared theme and live-view widget
```

### How it fits together

Services never touch LVGL. They publish small by-value payloads on an
`esp_event` loop; the visible screen subscribes in `on_enter`, unsubscribes in
`on_leave`, and takes the LVGL lock inside its handler. Only one screen is
built and only one is doing work at a time.

The camera runs in **JPEG mode permanently**, and one frame feeds three
consumers without a mode switch: the recorder stores it verbatim, the detector
decodes it 1:8, the live view decodes it to RGB565. Core 1 runs the frame pump
(JPEG decode is the heaviest periodic work); core 0 runs LVGL, the event loop
and the pollers.

## What the pages do

| Page | Notes |
|---|---|
| **Camera** | Live view, snapshot to `/sdcard/image`, AVI recording, flip |
| **UART** | Configurable baud/format, ASCII or hex, send dialog, log to card |
| **Modbus** | RTU ⇄ TCP switch, arbitrary read/write, hex + decimal results |
| **Power** | Voltage/current/power/energy from a meter, four rail coils, usage chart |
| **Switches** | Ten switches, each bound to a GPIO, expander bit or Modbus coil |
| **Temperature** | Arc gauge, humidity, outdoor, setpoint write-back, room tabs |
| **Detection** | Live box overlay, event list, detection/notify toggles |
| **Storage** | Recordings grouped by day, capacity bar |
| **Playback** | AVI player with scrub, frame step, pause |
| **Settings** | System, network, camera, detection, Modbus, about |

Recordings are MJPEG in AVI, which plays in VLC and every desktop player
without a codec pack, and whose `idx1` index makes on-device seeking cheap. A
recording cut short by a power loss never got its index written; the reader
rebuilds one by scanning, so those files still play.

## Three things to know

**Detection labels are a heuristic, not a model.** The pipeline compares each
frame against an adapting background, groups foreground cells into a blob, and
labels it from geometry and motion: tall and slow → person, wide and fast →
vehicle, otherwise object. "Something is there and it is moving" is dependable.
The label is not — an umbrella will read as a person often enough that you
should not build a security policy on it. For real classification, register an
esp-dl backend with `svc_detect_set_backend()`; the motion pipeline then just
gates which frames the model runs on.

**Modbus RTU and TCP are not simultaneous.** esp-modbus 1.x hosts one master
instance at a time, so the Modbus page switches between them rather than
running both. Switching tears the stack down and back up (a few hundred ms).
esp-modbus 2.x has a handle-based API that would allow both at once, at the
cost of an untested API migration.

**The power and climate register map is a guess.** The defaults assume a
common 40001-based energy meter. The Modbus page lets an installer read and
write arbitrary registers to find the real map, and Settings makes the
addresses editable — but the shipped defaults will not match your meter.

## Not built

- On-device Modbus *slave* mode (master only).
- Audio: the board's ES8311 codec, mic and speaker are unused.
- The IMU (QMI8658) is probed and reported but not otherwise used.
- OTA update; the partition table leaves room but no OTA path exists.
- Screen lock: `pin_code` is stored in settings but nothing enforces it.
