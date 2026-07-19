# Third-Party Notices

This project is licensed under the Apache License 2.0. It builds on third-party
tools, frameworks, and libraries that keep their own licenses.

## Runtime Firmware Dependencies

The firmware is built with ESP-IDF through PlatformIO.

| Component | Version / Source | License |
| --- | --- | --- |
| ESP-IDF | `dependencies.lock`: `idf` 5.5.4, installed as PlatformIO package `framework-espidf` | Apache-2.0 |
| ESP-IDF components | Selected by ESP-IDF build metadata from `framework-espidf/components` | Mostly Apache-2.0; some bundled upstream components use compatible permissive or dual licenses |
| Espressif mDNS component | `dependencies.lock`: `espressif/mdns` 1.11.3 | Apache-2.0 |
| Mbed TLS, via ESP-IDF | ESP-IDF component `mbedtls` | Apache-2.0 OR GPL-2.0-or-later; this project uses it under Apache-2.0 |
| wpa_supplicant, via ESP-IDF | ESP-IDF component `wpa_supplicant` | BSD license option, according to its bundled `COPYING` file |
| lwIP, via ESP-IDF | ESP-IDF component `lwip` | BSD-style license, according to upstream lwIP licensing |
| FreeRTOS, via ESP-IDF | ESP-IDF component `freertos` | MIT-style license, according to upstream FreeRTOS licensing |

Generated dependency folders such as `.pio/` and `managed_components/` are not
tracked in this repository. Developers should install dependencies through
PlatformIO/ESP-IDF and review the corresponding bundled license files in their
local toolchain before redistributing firmware binaries.

## Build And Flash Tooling

| Tool | Role | License |
| --- | --- | --- |
| PlatformIO Core / espressif32 platform | Build orchestration and board/platform metadata | Apache-2.0 for the installed `espressif32` platform package |
| esptool | Flashing utility used through ESP-IDF/PlatformIO | GPL-2.0-or-later in the installed ESP-IDF tooling; used as a separate development tool, not linked into firmware |

## Protocol And Product References

The project references public protocol and product documentation but does not
vendor those documents or vendor product firmware/software.

- Bosch eBike Systems Open Live Data Interface PDF:
  <https://www.bosch-ebike.com/fileadmin/EBC/Service/Downloads/LiveData/20260501_LiveDataInterface_V1_28042026.pdf?_=1777357728>
- Philips Hue developer documentation:
  <https://developers.meethue.com/>

Product and company names are used descriptively. Bosch, Bosch eBike Systems,
Philips Hue, and related marks belong to their respective owners. This project is
not affiliated with, endorsed by, or sponsored by those companies.

## Audit Notes

- No third-party source files are tracked in the repository at the time this file
  was written, except for standard project metadata and generated lock/config
  files.
- The project contains a hand-written decoder for the documented Bosch Live Data
  protobuf fields; it does not vendor generated `.proto` output or Bosch source
  code.
- The Philips Hue integration uses local Hue HTTP APIs and mDNS discovery but
  does not vendor Hue SDK code.
