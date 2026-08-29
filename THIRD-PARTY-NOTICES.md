# Third-party notices

This project is licensed under the GNU General Public License version 3.0; see
[LICENSE](LICENSE). The source in this repository is entirely under that
licence.

The **firmware images** are not only this source. Building links in the ESP-IDF
framework and the components listed below, so a released `.bin` carries other
people's code, under their licences. This file records what and whose, which is
what those licences ask for in return.

Nothing here is vendored into the repository. The components are fetched at
build time by the IDF component manager, pinned by `dependencies.lock`, and the
framework comes from the PlatformIO platform pinned in `platformio.ini`.

## What the images contain

| Component | Version | Licence | Source |
| :--- | :--- | :--- | :--- |
| ESP-IDF | 6.0.1 | Apache-2.0 | [espressif/esp-idf](https://github.com/espressif/esp-idf) |
| FreeRTOS kernel | 10.5.1 | MIT | bundled with ESP-IDF |
| lwIP | 2.2.0 | BSD-3-Clause | bundled with ESP-IDF |
| Mbed TLS | 4.0.0 | Apache-2.0 (see below) | bundled with ESP-IDF |
| Wi-Fi and PHY libraries | with ESP-IDF 6.0.1 | Apache-2.0 (see below) | [espressif/esp32-wifi-lib](https://github.com/espressif/esp32-wifi-lib) |
| cJSON | 1.7.19~2 | MIT | [espressif/cjson](https://components.espressif.com/components/espressif/cjson) |
| mDNS | 1.11.3 | Apache-2.0 | [espressif/mdns](https://components.espressif.com/components/espressif/mdns) |
| esp-mqtt | 1.1.0 | Apache-2.0 | [espressif/mqtt](https://components.espressif.com/components/espressif/mqtt) |

All of these are one-way compatible with the GPL version 3: their code may be
combined into a GPLv3 work, and the combined work is then covered by the GPLv3.

**Mbed TLS** is offered under Apache-2.0 *or* GPL-2.0-or-later. The Apache-2.0
option is the one that applies here, because GPL-2.0 and GPL-3.0 cannot be
combined.

**The Wi-Fi and PHY libraries** are shipped by Espressif as pre-compiled
binaries under Apache-2.0. The licence is permissive, but their source is not
published, so they cannot be rebuilt from source the way everything else here
can.

## What the web installer page uses

The installer at `web/index.html` loads
[ESP Web Tools](https://github.com/esphome/esp-web-tools) 10 (Apache-2.0), by
Nabu Casa and the ESPHome project, from a CDN at run time. It is referenced, not
bundled, and none of it ends up in the firmware.

## Where this project came from

This firmware is a derivative work of
[hn/bsh-home-appliances](https://github.com/hn/bsh-home-appliances) by Hajo
Noerenberg, GPL-3.0: specifically its `open-dbus2` protocol stack and its
reverse-engineering documentation of the bus. Files carrying code ported from it
keep his copyright line alongside the later one.

The appliance profiles in `profiles/` were converted from the ESPHome
configurations in that same project, which were contributed by many people. Each
profile carries a `credits` field naming the authors of the configuration it
came from, and that field is meant to survive further conversion.
