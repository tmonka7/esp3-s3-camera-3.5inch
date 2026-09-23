# Home Automation Panel — ESP32-S3-Touch-LCD-3.5-C

A touch control panel for the Waveshare ESP32-S3-Touch-LCD-3.5-C (320×480 ST7796
over SPI, FT6336 touch, OV5640 camera on the DVP port, TF card, 8 MB PSRAM /
16 MB flash). Built against **ESP-IDF v5.3.5**.

Fifteen screens: splash, dashboard, camera, UART console, Modbus master,
power control, ten room switches, temperature, person/object detection, TF
card browser, video playback, settings, a date/time editor, door access and
the credential list.

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

### There are two spare GPIOs, and access control uses both

The DVP camera takes 14 pins. After the LCD, touch, I²C, TF card and the two
UARTs, what is left on an ESP32-S3-N16R8 — excluding 26–32 (flash), 33–37
(octal PSRAM) and 22–25 (which do not exist) — is **GPIO 6 and GPIO 11**,
plus GPIO 0 (the BOOT strap) and 19/20 if you give up USB.

Access control spends exactly those two: **RC522 chip select on 6, door strike
on 11**. There is no third pin for a door contact or an exit button. If you
need one, the TCA9554 expander has spare bits, or a Modbus relay board has
inputs.

All three UARTs are also taken (console, RS485/Modbus, UART page), so a
serial reader would cost you one of those pages.

Two wiring points that are easy to get wrong:

- **The RC522 needs MISO.** The LCD never reads, so a board may not break that
  pin out even though it is assigned. Check before you solder.
- **Fit a pull-down on the relay input.** GPIO 11 floats from reset until the
  firmware configures it. Without a pull-down (or a pull-up, for an
  active-low relay module, with `BSP_LOCK_ACTIVE_HIGH` unticked) the door can
  unlock on every reboot. Firmware cannot close this window; only a resistor
  can.

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
  svc_access/            door policy: credentials, unlock decision, strike
  ui/                    15 LVGL screens + shared theme and live-view widget
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
| **Settings** | System, network, camera, detection, access, Modbus, about |
| **Date & Time** | Six rollers, live clock readout, writes through to the RTC |
| **Door Access** | Lock state, manual unlock, relock countdown, recent decisions |
| **Credentials** | Enrolled cards: rename, disable, remove |

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

## Door access

An MFRC522 on the LCD's SPI bus reads a card's UID; `svc_access` decides what
that UID is worth and drives the strike for a configurable time before
relocking itself. Credentials live in their own NVS namespace, deliberately
separate from the settings blob, so bumping `APP_SETTINGS_VERSION` can never
erase the door keys.

Enrolment is from **Door Access → Add card**, which opens a 30-second window,
closes on the first card read, and closes again if you leave the page.

The **Credentials** page (Door Access → Cards, or Settings → Access →
Credentials) has both routes: **Scan card** does the same thing, and **Enter
UID** types one in as hex — accepting `04A2B31C`, `04 A2 B3 1C` and
`04:A2:B3:1C` alike. Typing it is how you commission the door before the
RC522 is wired, and the only route at all if the reader never answers.

Every
decision is appended to `/sdcard/access/YYYY-MM-DD.csv` and, by default,
photographed — the photo is a fresh grab, so it shows whoever is standing
there rather than the detector's last frame.

Repeated denials trigger a lockout (five failures, 30 seconds, both
configurable) so the reader cannot be worked through in a loop.

### What this lock does not do

It is configured so that **either** a card **or** a face opens the door, which
makes the door as strong as the weaker of the two:

- **A card UID is not a secret.** It is broadcast unauthenticated to anything
  that asks, and a cloner copies it in seconds. The firmware reads the UID and
  stops there — no MIFARE sector authentication, no DESFire. Anyone who can
  hold a reader near a resident's card can make a working copy.
- **Face recognition cannot tell a face from a photograph of one.** Nothing in
  an RGB camera distinguishes them, and this board has no IR or depth sensor
  to help. That is a hardware property, not a threshold to tune.
- **The strike is driven from the panel's own GPIO.** If the panel is mounted
  outside, prying it off the wall exposes the two wires that open the door,
  and the electronics stop being relevant.

Each of those has a fix if it matters later: DESFire cards with AES mutual
authentication, requiring card *and* face together rather than either, and
moving the relay to the secure side of the door as a Modbus coil (already
supported by `svc_switch`). As configured, treat this as a convenience lock
on an interior or low-risk door, not as security.

The audit trail is the part that holds up regardless — it records what was
presented and when, with a photo, whether or not the credential was genuine.

### Face recognition is not built yet

`svc_access_submit_face()` and the policy around it are in place — threshold,
enable switch, credential kind, audit path — but nothing calls it. The
recogniser needs `esp-dl`, which is not vendored, so wiring it up means
fetching esp-dl and its models once on a networked machine and committing
them alongside LVGL and esp32-camera. `svc_detect`'s existing backend hook is
where the model gets gated on motion so it is not run on every frame. Until
then the Face Entry row reads "unavailable" and cannot be switched on.

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
- Face recognition: the policy and audit paths exist, the recogniser does not
  (see "Face recognition is not built yet" above).
- Card authentication: UID only, no MIFARE sector auth and no DESFire.
- Door contact and exit button: no GPIOs left for them.
