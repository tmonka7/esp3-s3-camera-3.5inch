# Home Automation Panel — ESP32-S3-Touch-LCD-3.5-C

A touch control panel for the Waveshare ESP32-S3-Touch-LCD-3.5-C (320×480 ST7796
over SPI, FT6336 touch, OV5640 camera on the DVP port, TF card, 8 MB PSRAM /
16 MB flash). Built against **ESP-IDF v5.3.5**.

Seventeen screens: splash, dashboard, camera, UART console, Modbus master,
power control, ten room switches, temperature, person/object detection, TF
card browser, video playback, settings, a date/time editor, door access, the
credential list, face enrolment and the web stream.

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
  svc_face/              LBPH face recognition -- no model file, no download
  svc_webcam/            MJPEG over HTTP on port 81
  ui/                    17 LVGL screens + shared theme and live-view widget
```

### How it fits together

Services never touch LVGL. They publish small by-value payloads on an
`esp_event` loop; the visible screen subscribes in `on_enter`, unsubscribes in
`on_leave`, and takes the LVGL lock inside its handler. Only one screen is
built and only one is doing work at a time.

The camera runs in **JPEG mode permanently**, and one frame feeds five
consumers without a mode switch: the recorder stores it verbatim, the web
stream sends it verbatim, the detector decodes it 1:8, the live view decodes
it to RGB565, and the face recogniser decodes it to 320×240. Core 1 runs the
frame pump and the face worker (JPEG decode is the heaviest periodic work);
core 0 runs LVGL, the event loop and the pollers.

Consumers that do real work take a copy and hand off to their own task rather
than working inside the pump callback, and each throttles itself — the
recogniser to about 2 Hz — so a slow consumer costs frame rate rather than
blocking the others. The pump only runs at all while something is subscribed,
so a panel with no viewer, no recording and detection off is not capturing.

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
| **Credentials** | Enrolled cards: rename, disable, remove; scan or type a UID |
| **Faces** | Live view with the detected box, five-shot enrolment, enrolled list |
| **Web Stream** | Stream URL, start/stop, port, viewer state, local preview |

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

## Watching the camera from a browser

The panel serves its camera as multipart MJPEG, the same shape the ESP32-CAM
examples use, so a browser, VLC, ffmpeg, Home Assistant and Frigate all take
it without help:

| | |
|---|---|
| `GET http://<panel-ip>:81/stream` | `multipart/x-mixed-replace`, MJPEG |
| `GET http://<panel-ip>:81/jpg` | one JPEG |
| `GET http://<panel-ip>:81/` | a page that just embeds the stream |

Turn it on from **Settings → Camera → Web Stream**, which opens a page showing
the URL, a start/stop button, the port, whether a viewer is attached, and a
local preview so you can tell a black stream from a stopped camera. The port
is configurable; 81 is the default because that is where everyone expects it.

Nothing is re-encoded — the frame the sensor produced is the frame that goes
out, so serving it costs a memcpy and a socket write rather than CPU.

**One viewer at a time.** A streaming handler never returns and
`esp_http_server` runs handlers on a single task, so a second viewer would
block behind the first; the second request is answered `503` instead of being
left to hang. If you need several viewers, point one of them at the panel and
fan out from there.

It is **off by default and has no authentication**. Anything that can reach
the panel on that port can watch the camera. Keep it on a trusted network, or
put a reverse proxy in front of it.

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

## Face recognition, without a model file

There is no neural network in this firmware and nothing is downloaded. Faces
are described with **local binary pattern histograms** — label each pixel by
how its eight neighbours compare to it, histogram those labels over a 4×4
grid, compare two faces by comparing histograms. LBPH learns nothing, so the
reference data is simply the faces you enrolled. No weights, no model
partition, no one-time fetch, and the offline build stays offline.

Finding the face uses the other technique that needs no training data: skin
occupies a narrow, well-documented band of Cb/Cr in YCbCr, and tone mostly
lives in Y rather than in the chrominance, so a threshold there works across
skin tones. Segment on it, take the largest region shaped like a head, grow
the box a little because skin segmentation stops at the hairline, and crop.

Enrol from **Door Access → Cards → Faces**, or **Settings → Access → Faces**.
The page shows the live camera with a box drawn round what it has found, so
you can see what it sees while enrolling. It takes five shots a few hundred
milliseconds apart rather than one, so the stored reference covers a little
natural movement. Up to 8 people, five shots each; templates live in
`/sdcard/faces/db.bin` because they are ~5 KB per person and the NVS
partition is 24 KB.

### How good is it, honestly

Good enough to tell apart a handful of people who stand roughly where they
stood when they enrolled, in light of roughly the same colour. Not good
enough for much else. Specifically, it degrades badly with head angle, with
a large change in lighting, with glasses that were not worn at enrolment, and
against a stranger who happens to resemble someone enrolled. Skin
segmentation also fires on wood, sand and terracotta, which the shape test
mostly — not always — rejects.

Two things take the edge off the worst failure mode. A match must beat the
runner-up by a clear margin, so "two mediocre scores" reads as no match
rather than a coin flip; and the threshold in Settings (default 80%) sets how
close is close enough. Raise it if you get false accepts, lower it if it
never recognises you.

And it still cannot tell a face from a photograph of one. That is the camera,
not the algorithm.

If you later want the accuracy of a CNN, vendoring `esp-dl` replaces exactly
two functions — descriptor extraction and distance. Enrolment, storage,
gating, policy and the UI are all independent of which of the two is behind
them.

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
- Card authentication: UID only, no MIFARE sector auth and no DESFire.
- Door contact and exit button: no GPIOs left for them.
- Web stream authentication: the MJPEG endpoints are open to the network.
- More than one simultaneous stream viewer.
