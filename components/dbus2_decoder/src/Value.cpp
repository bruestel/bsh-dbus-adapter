/*
   A decoded value.

   (C) 2026 Jonas Brüstel
   Licensed under the GNU General Public License version 3.0.
*/

#include "dbus2/decoder/Value.h"

#include <cstdio>

namespace dbus2::decoder {

std::string to_string(const Value &v, int decimals) {
  if (auto *s = std::get_if<std::string>(&v))
    return *s;
  /* "On"/"Off" rather than true/false, because these are read by people: the
     page shows them and a broker carries them as text. Not shouted -- the
     capitals came from Home Assistant's binary sensors, which no longer need
     them: the discovery payload declares these exact words as pl_on and pl_off,
     so this is the one place that decides, and the page, the broker and the
     dashboard cannot drift apart. */
  if (auto *b = std::get_if<bool>(&v))
    return *b ? "On" : "Off";
  if (auto *d = std::get_if<double>(&v)) {
    char buf[32];
    if (decimals >= 0)
      snprintf(buf, sizeof(buf), "%.*f", decimals, *d);
    else if (*d == static_cast<long long>(*d))
      snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(*d));
    else
      snprintf(buf, sizeof(buf), "%g", *d);
    return buf;
  }
  return "";
}

bool same(const Value &a, const Value &b) {
  if (a.index() != b.index())
    return false;
  if (auto *x = std::get_if<double>(&a))
    return *x == std::get<double>(b);
  if (auto *x = std::get_if<bool>(&a))
    return *x == std::get<bool>(b);
  if (auto *x = std::get_if<std::string>(&a))
    return *x == std::get<std::string>(b);
  return true;  // both empty
}

}  // namespace dbus2::decoder
