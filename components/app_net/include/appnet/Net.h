/*
   WiFi with an access-point fallback.

   The device ends up behind an appliance where no cable may be attached, so it
   must always remain reachable somehow. If a station connection is not
   configured or cannot be established, it opens its own network and serves a
   captive portal, which is the only way back in.

   (C) 2026 Jonas Brüstel
   Licensed under the GNU General Public License version 3.0.
*/

#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace appnet {

enum class State : uint8_t {
  Idle,
  Connecting,
  Connected,
  ApMode,  // own network, captive portal, setup allowed without a password
};

const char *to_string(State s);

using StateCallback = std::function<void(State)>;

bool begin(StateCallback on_state_change);

State state();
std::string ip();
std::string ssid();
int8_t rssi();

/* True while the device is only reachable through its own access point. Setup
   endpoints must stay open in that case -- requiring a password here would lock
   the user out of a device they cannot reach any other way. */
bool provisioning();


/* Wall-clock time, once SNTP has managed to set it.

   The device keeps UTC and carries no timezone. Frames stay stamped with the
   monotonic uptime clock, because that is what keeps the spacing between them
   exact -- an NTP correction steps the wall clock, and must never make two
   frames look as though they arrived out of order. `boot_epoch_ms` is the
   bridge: add a frame's uptime stamp to it and the result is the absolute time
   it arrived. It is derived on every call, so a later correction applies to
   frames already captured rather than only to new ones.

   Consumers localise for themselves. A browser knows the user's timezone and a
   broker wants UTC anyway, so keeping one here would only be a setting to get
   wrong. */
struct Time {
  bool synced = false;
  int64_t epoch_ms = 0;       // now, milliseconds since the Unix epoch, UTC
  int64_t boot_epoch_ms = 0;  // epoch time at which the uptime clock read zero
};
Time time_info();

}  // namespace appnet
