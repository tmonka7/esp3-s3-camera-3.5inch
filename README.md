# Home Automation Panel — ESP32-S3-Touch-LCD-3.5-C

A touch control panel for the Waveshare ESP32-S3-Touch-LCD-3.5-C (320×480 ST7796
over SPI, FT6336 touch, OV5640 camera on the DVP port, TF card, 8 MB PSRAM /
16 MB flash). Built against **ESP-IDF v5.3.5**.

Thirteen screens: splash, dashboard, camera, UART console, Modbus master,
power control, ten room switches, temperature, person/object detection, TF
card browser, video playback, settings and a date/time editor.

**This project builds with no network access.** There are no
`idf_component.yml` manifests, so the IDF component manager never runs and
nothing is fetched from `components.espressif.com`.

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

Nothing but ESP-IDF v5.3.5 is required — no internet, no `idf.py add-dependency`.

```bash
idf.py set-target esp32s3
idf.py menuconfig        # verify the Board Support pins
idf.py build
idf.py -p COMx flash monitor
```

Partitions (16 MB): 5 MB app, 2 MB `assets` SPIFFS, ~8.6 MB free for a later
OTA layout.

## Third-party code

Two libraries are too large to reimplement and are vendored under
`third_party/`, pinned and pruned:

| Library | Version | Why it is vendored |
|---|---|---|
| `lvgl` | v8.3.11 | The whole UI toolkit |
| `esp32-camera` | v2.0.15 | DVP capture, SCCB, JPEG decode |

`third_party/lvgl/demos/` and `third_party/lvgl/examples/` are deliberately
empty: LVGL's `env_support/cmake/esp.cmake` lists them in `INCLUDE_DIRS`
unconditionally, so the directories must exist, but their ~68 MB of contents is
only compiled when `CONFIG_LV_BUILD_EXAMPLES` or a `CONFIG_LV_USE_DEMO_*`
option is set, and this firmware sets none of them.

Everything else that would normally come from the component registry is
written in-tree instead, which also removes the version coupling those
components impose:

| Replaces | In-tree | Size |
|---|---|---|
| `esp_lcd_st7796` | `components/bsp/src/lcd_st7796.c` | ~290 lines |
| `esp_lcd_touch` + `esp_lcd_touch_ft5x06` | `components/bsp/src/touch_ft6336.c` | ~130 lines |
| `esp_lvgl_port` | `components/bsp/src/lvgl_port.c` | ~210 lines |
| `esp-modbus` | `components/svc_modbus/src/mb_*.c` | ~500 lines |

The ST7796 driver implements the standard `esp_lcd_panel_t` vtable, so the
rest of the firmware uses ordinary `esp_lcd_panel_*` calls and nothing knows
the difference.

## Layout

```
main/                    boot sequence, auto-sleep
third_party/             vendored lvgl + esp32-camera
components/
  bsp/                   pins, I²C, TCA9554, ST7796, FT6336, LVGL port,
                         camera, TF card
  app_core/              event bus, NVS settings, manual RTC clock, Wi-Fi
  svc_uart/              serial console + scrollback + log export
  svc_modbus/            RTU + TCP master (PDU codec, both transports live)
  svc_switch/            10 switches: GPIO / expander / Modbus coil bindings
  svc_power/             meter polling, four rails, 24 h usage history
  svc_temp/              SHT3x or Modbus climate, setpoint write-back
  svc_media/             frame pump, snapshots, MJPEG-AVI record/play, browser
  svc_detect/            motion + blob detection, pluggable classifier
  svc_notify/            buzzer, CSV event log, HTTP webhook
  ui/                    13 LVGL screens + shared theme and live-view widget
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
| **Modbus** | RTU and TCP both live, arbitrary read/write, hex + decimal results |
| **Power** | Voltage/current/power/energy from a meter, four rail coils, usage chart |
| **Switches** | Ten switches, each bound to a GPIO, expander bit or Modbus coil |
| **Temperature** | Arc gauge, humidity, outdoor, setpoint write-back, room tabs |
| **Detection** | Live box overlay, event list, detection/notify toggles |
| **Storage** | Recordings grouped by day, capacity bar |
| **Playback** | AVI player with scrub, frame step, pause |
| **Settings** | System, network, camera, detection, Modbus, about |
| **Date & Time** | Six rollers, live clock readout, writes through to the RTC |

Recordings are MJPEG in AVI, which plays in VLC and every desktop player
without a codec pack, and whose `idx1` index makes on-device seeking cheap. A
recording cut short by a power loss never got its index written; the reader
rebuilds one by scanning, so those files still play.

The Modbus master implements function codes 1, 2, 3, 4, 5, 6, 15 and 16, with
CRC-16 framing on RS485 and MBAP framing over TCP. RTU and TCP have separate
locks and run at the same time, so a slow RS485 poll never blocks a TCP
request; the device list picks which one an ordinary request goes out on.
The TCP socket reconnects once per transaction, because gateways drop idle
connections and that should not surface as a user-visible failure.

## The clock is set by hand

There is no SNTP. The panel restores the time from the on-board PCF85063 at
boot and is otherwise set from **Settings -> System -> Date & Time**, which
opens its own page: six rollers for year/month/day/hour/minute/second, a live
readout of the current clock, and a button to snap the rollers back to it. The
day roller only offers days the selected month actually has, so 31 February
cannot be entered.

Everything the user sees and types is **local** time; the RTC stores **UTC**,
and the POSIX timezone string in Settings converts between them. Change the
timezone and the displayed clock shifts without the RTC being rewritten, which
is the behaviour you want when a unit is commissioned in one region and
installed in another.

If no PCF85063 answers on the I2C bus the page says "no RTC" and the time has
to be re-entered after every power cut.

## Two things to know

**Detection labels are a heuristic, not a model.** The pipeline compares each
frame against an adapting background, groups foreground cells into a blob, and
labels it from geometry and motion: tall and slow → person, wide and fast →
vehicle, otherwise object. "Something is there and it is moving" is dependable.
The label is not — an umbrella will read as a person often enough that you
should not build a security policy on it. For real classification, register an
esp-dl backend with `svc_detect_set_backend()`; the motion pipeline then just
gates which frames the model runs on. (esp-dl is not vendored, so adding it
means giving up the offline build or vendoring it too.)

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
