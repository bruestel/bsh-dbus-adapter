/*
   An appliance profile: what the frames on this machine mean.

   One profile describes one appliance model. It is data, not code, so adding a
   machine is a file rather than a firmware build -- which is the whole reason
   the decoding was lifted out of the ESPHome lambdas.

   (C) 2026 Jonas Brüstel
   Licensed under the GNU General Public License version 3.0.
*/

#pragma once

#include "dbus2/decoder/Extract.h"
#include "dbus2/decoder/Transform.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace dbus2::decoder {

enum class Kind : uint8_t { Sensor, BinarySensor, TextSensor, Event, Hidden };

const char *to_string(Kind k);

/* What Home Assistant needs to present the value. Carried verbatim from the
   upstream configurations, which already got these right. */
struct HaMeta {
  std::string device_class;
  std::string unit;
  std::string state_class;
  std::string icon;
  std::string entity_category;
  int decimals = -1;
};

struct Behavior {
  bool on_change = true;      // suppress repeats; the bus repeats constantly
  uint32_t throttle_ms = 0;
  uint32_t debounce_ms = 0;
  uint32_t expire_after_s = 0;  // publish "unavailable" if nothing arrives
  uint32_t momentary_ms = 0;    // auto-clear, for button-press style events
  bool retain = true;

  /* Filling the silence between two real readings.

     Some appliances report a remaining time only now and then -- this dryer
     managed one update in twelve minutes of running -- which leaves a display
     that is correct but visibly stuck. With this set, the engine carries the
     value forward on its own: every countdown_every_s it subtracts
     countdown_by, never past countdown_min, and any frame from the bus resets
     both the value and the cycle.

     The gate is not optional decoration. A remaining time only counts down
     while the appliance is actually running: at rest the same frame carries the
     *expected* duration of the selected programme, and a paused machine does
     not advance at all -- it re-estimates on resume. Counting in either state
     would produce a confident, wrong number. So the countdown runs only while
     the named entity reads the given text; without a gate it does not run. */
  uint32_t countdown_every_s = 0;   // 0 = no countdown
  double countdown_by = 1;
  double countdown_min = 0;
  std::string countdown_gate_id;    // entity whose value permits counting
  std::string countdown_gate_value; // ... when it reads exactly this
};

/* Optional short-circuit before the main extraction, for the conditional
   arithmetic that appears as `if (x[4] == 1) return 0;` in the lambdas. */
struct Guard {
  Extract when;
  double eq = 0;
  Value emit;
};

struct Entity {
  std::string id;
  std::string name;
  Kind kind = Kind::Sensor;

  uint8_t dest = 0;
  uint16_t cmd = 0;
  uint8_t min_len = 0;

  /* Optional payload matcher, and only for Kind::Hidden: leading data bytes
     (after the command) that must equal these for the marker to apply. Empty
     means the whole command is marked regardless of payload.

     Consulted by the monitor, never by the decoder, which matches on dest and
     cmd alone. That is why the parser refuses it on any other kind: a matcher
     nothing enforces would let a profile promise a narrower match than it
     gets. */
  std::vector<uint8_t> match_payload;

  std::vector<Guard> guards;
  Extract extract;
  std::vector<Transform> transforms;
  Behavior behavior;
  HaMeta ha;
};

struct Meta {
  std::string id;
  std::string manufacturer;
  std::string model;
  /* What the appliance is called in a shop, as opposed to what it is called on
     its type plate. Optional; a profile written by hand rarely has one. */
  std::string product;
  std::string appliance;
  std::string source;
  std::vector<std::string> credits;
};

struct Profile {
  Meta meta;
  uint32_t baud = 0;  // 0 = leave the bus setting alone
  uint32_t availability_timeout_s = 60;
  std::vector<Entity> entities;

  /* (dest << 16) | cmd -> indices into entities. Built once on load; with well
     under a hundred distinct pairs even on the largest configuration, lookup
     cost is irrelevant next to getting it right. */
  std::unordered_multimap<uint32_t, uint16_t> index;

  static uint32_t key(uint8_t dest, uint16_t cmd) { return (static_cast<uint32_t>(dest) << 16) | cmd; }

  void reindex();

  /* The dest/cmd pairs this profile expects, used to recognise which appliance
     is on the bus without asking it -- asking would mean transmitting. */
  std::vector<uint32_t> signature() const;
};

}  // namespace dbus2::decoder
