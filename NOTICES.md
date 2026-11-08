# Third-party notices

This firmware is MIT licensed (see [LICENSE](LICENSE)). It bundles or builds
against the following third-party work, each under its own permissive licence.

## Bundled in this repository

| Component | Where | Licence |
|---|---|---|
| cJSON 1.7.18 (Dave Gamble and contributors) | [src/vendor/cJSON.c](src/vendor/cJSON.c), [cJSON.h](src/vendor/cJSON.h) | MIT, see [src/vendor/LICENSE.cJSON](src/vendor/LICENSE.cJSON) |
| font8x8_basic (Daniel Hepper, after Marcel Sondaar and the IBM VGA fonts) | [src/vendor/font8x8_basic.h](src/vendor/font8x8_basic.h) | Public domain |
| DHCP server (Damien P. George, from MicroPython, via pico-examples) | [src/vendor/dhcpserver.c](src/vendor/dhcpserver.c), [dhcpserver.h](src/vendor/dhcpserver.h) | MIT, in the file header |
| DNS server (Raspberry Pi (Trading) Ltd., from pico-examples) | [src/vendor/dnsserver.c](src/vendor/dnsserver.c), [dnsserver.h](src/vendor/dnsserver.h) | BSD 3-Clause, in the file header |
| QR Code generator library (Project Nayuki) | [src/vendor/qrcodegen.c](src/vendor/qrcodegen.c), [qrcodegen.h](src/vendor/qrcodegen.h) | MIT, in the file header |

## Built against, not bundled

The build pulls these in through PlatformIO; none of their source is copied
into this repository.

| Component | Licence |
|---|---|
| Raspberry Pi Pico SDK | BSD 3-Clause |
| lwIP | BSD 3-Clause |
| cyw43-driver and its CYW43439 firmware blob | Permissive, with a Raspberry Pi Pico-specific clause; see the package's own LICENSE |
| PlatformIO `platform-raspberrypi` (maxgerhardt fork) | Apache 2.0 |

## Derived know-how, no code copied

The panel initialisation register sequences in [src/epd.c](src/epd.c) are the
published command sequences for the AC073TC1A and E673 controllers, taken from
Pimoroni's MIT-licensed drivers:

- [pimoroni/pimoroni-pico](https://github.com/pimoroni/pimoroni-pico) `drivers/inky73` (MIT)
- [pimoroni/inky](https://github.com/pimoroni/inky) `inky/inky_e673.py` (MIT)

The PCF85063A register usage in [src/rtc.c](src/rtc.c) and the PSRAM burst
protocol in [src/psram.c](src/psram.c) follow the same drivers and the parts'
datasheets. The code here is an independent C implementation against the Pico
SDK, not a copy, but the sequences originate with Pimoroni and their MIT
licence is acknowledged here with thanks.

The Tesserae REST device protocol is implemented from its published
specification at <https://docs.tesserae.ink/dev/client-protocol/>. No code from
the AGPL-licensed Tesserae server or its `tesserae-device-*` firmwares is
included in this repository.
