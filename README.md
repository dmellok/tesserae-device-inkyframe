# Tesserae client for the Pimoroni Inky Frame 7.3"

Firmware that turns a **Pimoroni Inky Frame 7.3"** (800 × 480, 7-colour or
Spectra 6, Raspberry Pi Pico W / Pico 2 W aboard) into a battery-powered
[Tesserae](https://docs.tesserae.ink/) dashboard panel over the v1 REST device
API. Written in C against the Raspberry Pi Pico SDK.

This is the Tesserae-maintained fork of
[partridgeworks/tesserae.inkyframe](https://github.com/partridgeworks/tesserae.inkyframe)
by Carl Partridge, who wrote the board support, panel drivers and REST client.
The fork adds a build for the Pico 2 W Frame, setup without a rebuild, and the
features the other Tesserae firmware has.

The panel is a thin client. Tesserae renders, dithers and packs every frame
server-side into the panel's own 4-bpp palette order, so the firmware streams
the downloaded bytes straight to the glass with no decoding, no quantising and
no rotation on device.

- Setup from a phone: the frame opens a `Tesserae-Setup-XXXX` network with a
  setup page for Wi-Fi and the server, so one build serves every frame.
- Zero-touch pairing: the panel announces itself, you click **Register**.
- ETag-conditional fetch, so an unchanged dashboard costs one small request and
  no 30-second e-ink refresh.
- True power-off between refreshes on battery: the PCF85063A RTC alarm wakes
  the board, which draws about 20 µA in between. On USB it stays resident and
  can hold the `always_on` fast-poll mode instead.
- The five front buttons send Tesserae button events, each with its letter
  (`a`-`e`) so the server's button map can bind every button on its own, with
  the server's post-press linger window and their LEDs as progress feedback.
- Battery voltage and RSSI telemetry, escalating backoff when the network is
  down, re-pairing when the token is revoked, `/log` forwarding of paint errors.

Design notes and the bring-up story: [IMPLEMENTATION_PLAN.md](IMPLEMENTATION_PLAN.md).

## Which panel do you have?

The 7.3" Inky Frame shipped with two different panels on identical pins:

| Revision | Colours | Build environment | Tesserae kind |
|---|---|---|---|
| Original **7-colour ACeP** (AC073TC1A), sold before September 2025 | black, white, green, blue, red, yellow, orange | `inkyframe` (default) | `pimoroni_inky_frame_73_acep` |
| **Spectra 6** (E673) with a **Pico 2 W**, shipped from September 2025 | black, white, yellow, red, blue, green | `inkyframe2_spectra` | `pimoroni_inky_frame_73` |
| Spectra 6 with an original Pico W | black, white, yellow, red, blue, green | `inkyframe_spectra` | `pimoroni_inky_frame_73` |

The Pico on the back says which: a Pico 2 W (RP2350) needs the
`inkyframe2_spectra` build, as an RP2040 image does not run on it. The
`inkyframe2_spectra` build is confirmed on hardware.

A Spectra 6 panel has a black border along the top edge where the ribbon cable
attaches. If you are unsure, flash the default build: the palette test pattern
paints eight bars, and the third bar is green on ACeP and red on Spectra 6.

## Build and flash

Requires [PlatformIO](https://platformio.org/) (the VS Code extension, or
`~/.platformio/penv/bin/pio`). It downloads the ARM toolchain, the Pico SDK and
picotool on the first build; nothing else needs installing.

1. Connect the frame over USB. For the **first** flash, put the Pico into
   bootloader mode: hold **BOOTSEL** on the Pico and tap **Reset** on the Inky
   Frame board; an `RPI-RP2` (or `RP2350`) drive appears. Later flashes do not
   need this, the firmware honours the 1200-baud reset.
2. Build and upload the environment for your frame (see the table above):

```
pio run -e inkyframe2_spectra -t upload
pio device monitor          # 115200 baud
```

No Wi-Fi or server goes into the build: the frame asks for them on first boot
(see [Set up](#set-up)). For development, `include/secrets.h` (copy
`include/secrets.example.h`, git-ignored) seeds them so the portal is skipped.

Flashing replaces whatever was on the Pico, MicroPython included. The SD card
is not touched.

## Install the device kind on the server

Tesserae picks the renderer from the device *kind*, and the kind carries the
panel size. Current Tesserae servers ship both Inky Frame entries, so there is
nothing to install. An older server needs the catalog entry for your revision
before the device registers, otherwise the stock `pico_bin_client` kind's
1600 × 1200 default produces frames the firmware refuses.

On such a server, copy the matching file from [server/hardware/pimoroni/](server/hardware/pimoroni/)
into the Tesserae install as `hardware/pimoroni/<name>.json`:

- Bare install or LXC: copy it into `<tesserae>/hardware/pimoroni/`.
- Docker: bind-mount the file, for example in `docker-compose.yml`

```yaml
      - ./hardware/pimoroni/inky_frame_73_acep.json:/app/hardware/pimoroni/inky_frame_73_acep.json:ro
```

then `docker compose up -d`. Confirm with
`docker logs tesserae | grep "hardware-catalog kind pimoroni_inky"`.

The firmware probes for its kind on every pairing attempt and, once registered,
on every wake while it is on the fallback. Install the file later and the next
wake moves the device onto the proper kind by itself.

## Set up

1. Power the frame. A fresh frame paints **Set up this frame** with a QR code
   and opens a Wi-Fi network named `Tesserae-Setup-XXXX` (password
   `tesserae`).
2. Scan the QR code, or join the network by hand. The setup page opens by
   itself; if not, browse to `http://192.168.4.1/`.
3. Enter your Wi-Fi, the Tesserae server URL (for example
   `http://tesserae.local:8765`) and, only for admin-gated pairing, a pairing
   code. **Save & restart**.

If the frame cannot join the Wi-Fi or reach the server with what was entered,
it opens the setup page again and says why. Once the settings have worked, an
outage only retries (offering the setup page once per outage), so a router
reboot never strands the frame in setup.

### The Refresh button (C) as the frame wakes

| Hold | Does |
|---|---|
| under 5 s | refresh |
| 5 s (the C light comes on fully) | opens the setup page |
| 20 s | factory reset: Wi-Fi, server and pairing are erased, then setup |

## Pair

1. After setup the panel joins Wi-Fi, announces itself, and paints a
   **Register me** page showing its device id, MAC and the server URL.
2. In Tesserae open **Settings → Devices → Discovered** and click **Register**.
3. Within 30 seconds the panel takes its token and shows *Waiting for a
   dashboard* until you bind one to it and press **Send**.

The firmware registers with `rotation: 0` (landscape, on its legs). On the
Spectra 6 Frame the firmware turns the buffer 180 degrees while streaming it,
because the E673 is mounted the other way up from the Pi Impression its
command sequence comes from. For any other turn, change **Rotation** on the
device card in Settings → Devices.

## Configuration

Build-time settings live in [include/config.h](include/config.h):

| Setting | Default | Meaning |
|---|---|---|
| `DEVICE_ID` / `DEVICE_NAME` | `inky_frame_73` / Inky Frame 7.3 | announced identity |
| `PANEL_VARIANT` | `PANEL_ACEP` | selects the panel sequence, kind and gamut |
| `PANEL_ROTATION` | 0 | the turn from the 800 × 480 buffer to the dashboard |
| `BUTTON_A_NAME` … `BUTTON_E_NAME` | left, –, refresh, –, right | Tesserae button actions; a button with `""` is sent by its letter only. Every press also carries its letter as `button_key`, which the server's button map checks first (Tesserae 0.505+): `{"a": "page:Calendar", "d": "page:Weather"}` |
| `SLEEP_MIN_S` / `SLEEP_MAX_S` / `SLEEP_DEFAULT_S` | 30 / 27 days / 900 | bounds applied to server sleep values |
| `BACKOFF_STEPS_S` | 60, 120, 300, 900 | retry ladder when Wi-Fi or the server is down |

Runtime settings, the refresh cadence, the button linger window and the
always-on polling mode, come from the device card in Tesserae and are applied on
every wake.

## Troubleshooting: the colours are swapped

If green paints as yellow, blue as red and red as blue, the panel and this
firmware are fine: the server sent the wrong palette indices. Tesserae's
palette-profile feature emits its colours in the Spectra 6 slot order even for
the Inky 7-colour gamut, which swaps two pairs of primaries. Clear the palette
profile on the device's Calibration tab and the colours come back, using the
same measured calibration values in the right order.

Clearing the profile does not stick, because the device card rewrites an empty
slug back to the family default. Use the ready-made workaround profile in
[server/palette_profiles/inky7-slot-order-fix.json](server/palette_profiles/inky7-slot-order-fix.json)
instead: copy it into `data/palette_profiles/` and select it in the Calibration
tab.

Full diagnosis, evidence and the upstream fix:
[docs/palette-order-bug.md](docs/palette-order-bug.md).

Push [tools/palette_test.png](tools/palette_test.png) (generate it with
`python3 tools/make_palette_png.py tools/`) as a dashboard to check: every bar
should show the colour its label names.

## Self-test build

`pio run -e inkyframe_selftest -t upload` builds a variant that skips Tesserae
entirely and paints a synthetic frame from `SELFTEST_BASE_URL` (set it in
[platformio.ini](platformio.ini)). Generate the frame with
`python3 tools/make_test_frames.py tools/` and serve it with
`python3 -m http.server 8000` from `tools/`. It is the quickest way to check the
download and paint path, and to confirm which way up the panel is mounted.

## Wake cycle

```
wake (RTC alarm / button A-E / external trigger / USB / reset)
  -> hold VSYS, read the wake reason from the shift register
  -> Wi-Fi -> resolve the server (mDNS or the fallback IP)
  -> no token?  POST /discover until Register is clicked
  -> GET /frame (If-None-Match: <etag>, ?button=... on a press)
       200: stream 192,000 bytes into PSRAM
       304: nothing to paint       204: "Waiting for a dashboard" page, once
       401/403: forget the token and re-pair on the next wake
  -> POST /status (battery, RSSI, IP, pending button) -> next_poll_s, config
  -> radio off -> paint (about 30 s) -> commit the ETag -> save state to flash
  -> arm the RTC alarm and drop VSYS (battery), or stay resident (USB)
```

Serial output at 115200 baud shows every step with timings.

## Repository layout

```
platformio.ini            envs: inkyframe (ACeP), inkyframe_spectra, inkyframe2_spectra (Pico 2 W), inkyframe_selftest
include/config.h          identity, kind/gamut, pins, button map, setup portal, flash layout
include/secrets.example.h optional development defaults, as include/secrets.h (git-ignored)
include/lwipopts.h        lwIP configuration for the CYW43 stack
src/board.c               VSYS hold, wake-reason shift register, buttons, LEDs
src/epd.c                 panel driver (AC073TC1A / E673) and the test pattern
src/psram.c               8 MB SPI PSRAM, the frame store
src/rtc.c                 PCF85063A clock, alarm and countdown timer
src/power.c               VSYS sense and the two sleep regimes
src/wifi.c                CYW43 join, identity, mDNS resolution
src/http.c                HTTP/1.1 client on lwIP raw TCP, with a streaming sink
src/tesserae.c            discover / register / frame / status / log
src/frame.c               download into PSRAM, length validation, paint
src/storage.c             log-structured flash state (pairing, ETag, server config)
src/settings.c            Wi-Fi, server and pairing code from the setup portal
src/portal.c              setup access point, DHCP, DNS hijack and the web form
src/screen.c              status pages and the setup page (with its join QR)
server/hardware/pimoroni/ Tesserae hardware-catalog entries for both revisions
tools/make_test_frames.py synthetic self-test frame generator
```

## Licence

MIT, see [LICENSE](LICENSE). Third-party components and the origin of the panel
command sequences are credited in [NOTICES.md](NOTICES.md).
