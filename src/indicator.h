/*
   LED signalling.

   Behind an appliance these two LEDs are the only feedback channel: no cable,
   no browser, no logs. So they have to separate the cases you would otherwise
   be blind to -- above all "WiFi is gone" versus "WiFi is fine but the bus is
   silent", and "nothing arrives" versus "bytes arrive but are garbage".

   Board wiring (verified against the schematic, all active low -- anode at
   3.3V, cathode through 510R to the GPIO):

     D4 red    hardwired to GND, power indicator, not software controlled
     D5 green  net STATUS   (GPIO6)  -- system and network state
     D6 blue   net ACTIVITY (GPIO7)  -- bus state

   (C) 2026 Jonas Brüstel
   Licensed under the GNU General Public License version 3.0.
*/

#pragma once

#include <cstdint>

namespace indicator {

enum class System : uint8_t {
  Booting,       // solid on until the first state is known
  ApMode,        // 5 Hz     -- waiting to be configured
  Disconnected,  // 2 flashes, pause -- WiFi configured but not associated
  Connected,     // heartbeat -- online, but no bus frame seen yet
  Nominal,       // inverted heartbeat -- online and frames flowing
  Fatal,         // 10 Hz
  Ota,           // both LEDs in antiphase -- do not remove power
  Wiping,        // both LEDs solid -- keep holding to erase the WiFi settings
};

enum class Bus : uint8_t {
  Silent,   // off
  Healthy,  // one blip per valid frame
  Garbage,  // steady 4 Hz: bytes arrive but fail CRC (wrong baud or wiring)
};

void begin();

void set_system(System s);
void set_bus(Bus b);

/* Triggers one blip in Healthy mode. Cheap; safe to call per frame. */
void note_frame();

/* Drives the patterns. Call regularly from the main loop. */
void tick(int64_t now_us);

const char *to_string(System s);
const char *to_string(Bus b);

}  // namespace indicator
