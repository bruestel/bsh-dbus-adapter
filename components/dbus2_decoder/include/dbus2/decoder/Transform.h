/*
   Turning a raw reading into the value a person wants to see.

   These replace the ESPHome filter chain. `map:` becomes Map, `multiply:`
   becomes Multiply, `calibrate_linear:` becomes Calibrate.

   Lookup is the one that earns its keep. The multi-line lambdas in the upstream
   configurations are almost all if/else-if chains over a byte, often falling
   back to a `globals:` variable holding the previous state. Lookup expresses
   both: rules plus a default of Hold, which keeps whatever was last emitted.
   Without it the coverage of the schema drops sharply and those appliances would
   need hand-written C++.

   (C) 2026 Jonas Brüstel
   Licensed under the GNU General Public License version 3.0.
*/

#pragma once

#include "dbus2/decoder/Value.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace dbus2::decoder {

enum class TOp : uint8_t {
  Multiply, Divide, Offset,
  Invert,     // boolean negation
  Clamp,
  Round,
  Calibrate,  // interpolate, or exact table lookup
  Map,        // number -> name
  Lookup,     // ranges/sets -> value, with Hold or Drop as fallback
  FilterOut,  // suppress specific readings
  Format,     // printf-style rendering to text
};

/* What to do when no rule matches. Hold is what makes a stateful appliance
   readable: many frames only report a change, so the last known value has to
   survive the frames that say nothing. */
enum class Fallback : uint8_t { Value, Hold, Drop };

struct Rule {
  std::vector<double> in;  // matching inputs
  Value out;
};

struct Transform {
  TOp op = TOp::Multiply;
  double value = 1;        // Multiply, Divide, Offset
  double min = 0, max = 0; // Clamp
  int decimals = 0;        // Round
  bool exact = false;      // Calibrate: table lookup rather than interpolation
  std::vector<std::pair<double, double>> points;  // Calibrate
  std::map<double, std::string> table;            // Map
  std::string fallback_text;                      // Map default
  std::vector<Rule> rules;                        // Lookup
  Fallback fallback = Fallback::Value;            // Lookup
  Value fallback_value;                           // Lookup
  std::vector<double> filter;                     // FilterOut
  std::string fmt;                                // Format

  /* What the one conversion in `fmt` expects, decided when the profile is
     parsed. It has to be recorded rather than worked out at use, because the
     value arriving here may be a string on one frame and a number on the next,
     depending on what the transforms before it produced -- and handing printf a
     double where it expects a pointer is a crash, not a wrong reading. */
  enum class FmtArg : uint8_t { None, Int, Double, String };
  FmtArg fmt_arg = FmtArg::None;

  /* `held` carries the entity's last emitted value, so Hold can return it.
     Returns nothing when the value should be dropped entirely. */
  std::optional<Value> apply(const Value &in, const Value &held) const;
};

/* Accepts a format string only if it carries exactly one conversion, and says
   what that conversion expects. Everything else -- a second conversion, %n, a
   width taken from an argument -- is refused.

   This exists because a profile is a file people exchange, and these strings
   reach printf. One conversion too many reads whatever follows on the stack:
   a crash, or somebody's memory published as a reading. */
bool check_format(const std::string &fmt, Transform::FmtArg *expects);

const char *to_string(TOp op);
std::optional<TOp> top_from_string(const char *name);

}  // namespace dbus2::decoder
