/*
   A decoded value.

   Appliance state is a mix of numbers, flags and enumerated names, and the same
   pipeline has to carry all three: a raw byte becomes a number, a lookup turns
   it into a name. Rather than three parallel paths, everything moves through one
   variant.

   (C) 2026 Jonas Brüstel
   Licensed under the GNU General Public License version 3.0.
*/

#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <variant>

namespace dbus2::decoder {

/* monostate means "no value" -- the frame was too short, a guard suppressed it,
   or a lookup dropped it. Distinct from zero, which is a real reading. */
using Value = std::variant<std::monostate, bool, double, std::string>;

inline bool empty(const Value &v) { return std::holds_alternative<std::monostate>(v); }

inline bool as_bool(const Value &v) {
  if (auto *b = std::get_if<bool>(&v))
    return *b;
  if (auto *d = std::get_if<double>(&v))
    return *d != 0.0;
  if (auto *s = std::get_if<std::string>(&v))
    return !s->empty();
  return false;
}

inline double as_number(const Value &v) {
  if (auto *d = std::get_if<double>(&v))
    return *d;
  if (auto *b = std::get_if<bool>(&v))
    return *b ? 1.0 : 0.0;
  return 0.0;
}

/* Rendered the way the value should appear over MQTT and in the UI. Booleans
   read "On"/"Off" -- text for people, decided here so the page, the broker and
   the discovery payload that declares those words cannot disagree. */
std::string to_string(const Value &v, int decimals = -1);

bool same(const Value &a, const Value &b);

}  // namespace dbus2::decoder
