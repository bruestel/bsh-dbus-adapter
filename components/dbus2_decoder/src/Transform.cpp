/*
   Turning a raw reading into the value a person wants to see.

   (C) 2026 Jonas Brüstel
   Licensed under the GNU General Public License version 3.0.
*/

#include "dbus2/decoder/Transform.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace dbus2::decoder {
namespace {

struct Name {
  TOp op;
  const char *name;
};

constexpr Name kNames[] = {
    {TOp::Multiply, "multiply"}, {TOp::Divide, "divide"},   {TOp::Offset, "offset"},
    {TOp::Invert, "invert"},     {TOp::Clamp, "clamp"},     {TOp::Round, "round"},
    {TOp::Calibrate, "calibrate_linear"}, {TOp::Map, "map"}, {TOp::Lookup, "lookup"},
    {TOp::FilterOut, "filter_out"}, {TOp::Format, "format"},
};

/* Piecewise-linear interpolation between the given points, extended with the
   slope of the outermost segment beyond the ends. Exact mode instead demands a
   listed input, which suits the code-to-temperature tables in the upstream
   configurations -- there, a value between two codes is meaningless. */
std::optional<double> calibrate(const std::vector<std::pair<double, double>> &pts, double x, bool exact) {
  if (pts.empty())
    return x;

  if (exact) {
    for (const auto &p : pts)
      if (p.first == x)
        return p.second;
    return std::nullopt;
  }

  if (pts.size() == 1)
    return pts[0].second;
  if (x <= pts.front().first || pts.size() == 1) {
    const auto &a = pts[0];
    const auto &b = pts[1];
    if (b.first == a.first)
      return a.second;
    return a.second + (x - a.first) * (b.second - a.second) / (b.first - a.first);
  }
  for (size_t i = 1; i < pts.size(); i++) {
    if (x <= pts[i].first) {
      const auto &a = pts[i - 1];
      const auto &b = pts[i];
      if (b.first == a.first)
        return b.second;
      return a.second + (x - a.first) * (b.second - a.second) / (b.first - a.first);
    }
  }
  const auto &a = pts[pts.size() - 2];
  const auto &b = pts.back();
  if (b.first == a.first)
    return b.second;
  return a.second + (x - a.first) * (b.second - a.second) / (b.first - a.first);
}

}  // namespace

const char *to_string(TOp op) {
  for (const auto &n : kNames)
    if (n.op == op)
      return n.name;
  return "?";
}

std::optional<TOp> top_from_string(const char *name) {
  if (!name)
    return std::nullopt;
  for (const auto &n : kNames)
    if (std::strcmp(n.name, name) == 0)
      return n.op;
  return std::nullopt;
}

bool check_format(const std::string &fmt, Transform::FmtArg *expects) {
  using A = Transform::FmtArg;
  A found = A::None;

  for (size_t i = 0; i < fmt.size(); i++) {
    if (fmt[i] != '%')
      continue;
    if (i + 1 < fmt.size() && fmt[i + 1] == '%') {  // an escaped per-cent sign
      i++;
      continue;
    }
    if (found != A::None)
      return false;  // a second conversion has no argument to consume

    size_t j = i + 1;
    /* Flags, then width and precision -- but only as digits. A '*' takes them
       from an argument that is not there. */
    while (j < fmt.size() && std::strchr("-+ #0", fmt[j]))
      j++;
    while (j < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[j])))
      j++;
    if (j < fmt.size() && fmt[j] == '.') {
      j++;
      while (j < fmt.size() && std::isdigit(static_cast<unsigned char>(fmt[j])))
        j++;
    }
    if (j >= fmt.size())
      return false;

    /* No length modifiers: %ld would read a long where an int was passed, and
       on this chip that happens to be the same width, which is exactly the kind
       of accident that stops being true on the next one. */
    switch (fmt[j]) {
      case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': found = A::Int; break;
      case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': found = A::Double; break;
      case 's': found = A::String; break;
      default: return false;  // includes %n, %p and any length modifier
    }
    i = j;
  }

  if (expects)
    *expects = found;
  return true;
}

std::optional<Value> Transform::apply(const Value &in, const Value &held) const {
  switch (op) {
    case TOp::Multiply: return Value{as_number(in) * value};
    case TOp::Divide:   return value == 0 ? std::optional<Value>{} : Value{as_number(in) / value};
    case TOp::Offset:   return Value{as_number(in) + value};
    case TOp::Invert:   return Value{!as_bool(in)};
    case TOp::Clamp:    return Value{std::clamp(as_number(in), min, max)};

    case TOp::Round: {
      const double f = std::pow(10.0, decimals);
      return Value{std::round(as_number(in) * f) / f};
    }

    case TOp::Calibrate: {
      auto r = calibrate(points, as_number(in), exact);
      if (!r)
        return std::nullopt;
      return Value{*r};
    }

    case TOp::Map: {
      auto it = table.find(as_number(in));
      if (it != table.end())
        return Value{it->second};
      if (fallback_text.empty())
        return std::nullopt;
      /* A default may carry the raw code, which is what makes an unmapped
         reading actionable rather than merely "unknown". */
      char buf[64];
      /* The parser has already refused anything but a single integer
         conversion here, so this argument matches whatever the string asks
         for. */
      snprintf(buf, sizeof(buf), fallback_text.c_str(), static_cast<int>(as_number(in)));
      return Value{std::string(buf)};
    }

    case TOp::Lookup: {
      const double x = as_number(in);
      for (const auto &r : rules)
        for (double candidate : r.in)
          if (candidate == x)
            return r.out;
      switch (fallback) {
        case Fallback::Value: return fallback_value;
        case Fallback::Hold:  return empty(held) ? std::optional<Value>{} : held;
        case Fallback::Drop:  return std::nullopt;
      }
      return std::nullopt;
    }

    case TOp::FilterOut: {
      const double x = as_number(in);
      for (double f : filter)
        if (f == x)
          return std::nullopt;
      return in;
    }

    case TOp::Format: {
      /* The value is converted to what the format string asks for, rather than
         passed as whatever it happens to be. A "%s" meeting a number used to
         read the bits of a double as a pointer; a "%.1f" meeting a string read
         a pointer as a double. Which of the two arrives depends on the
         transforms before this one, so it cannot be settled by the profile
         author and must not be left to chance. */
      char buf[96];
      switch (fmt_arg) {
        case FmtArg::Int:
          snprintf(buf, sizeof(buf), fmt.c_str(), static_cast<int>(as_number(in)));
          break;
        case FmtArg::Double:
          snprintf(buf, sizeof(buf), fmt.c_str(), as_number(in));
          break;
        case FmtArg::String: {
          const std::string text = to_string(in);
          snprintf(buf, sizeof(buf), fmt.c_str(), text.c_str());
          break;
        }
        case FmtArg::None:
          /* A format string with no conversion at all is a constant. */
          snprintf(buf, sizeof(buf), "%s", fmt.c_str());
          break;
      }
      return Value{std::string(buf)};
    }
  }
  return in;
}

}  // namespace dbus2::decoder
