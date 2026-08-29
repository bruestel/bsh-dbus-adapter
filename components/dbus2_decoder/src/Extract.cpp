/*
   Pulling a value out of a frame payload.

   (C) 2026 Jonas Brüstel
   Licensed under the GNU General Public License version 3.0.
*/

#include "dbus2/decoder/Extract.h"

#include <cstdio>
#include <cstring>

namespace dbus2::decoder {
namespace {

struct Name {
  Op op;
  const char *name;
};

/* The names are the schema's vocabulary, so they live in exactly one place --
   parser and serialiser both read from here. */
constexpr Name kNames[] = {
    {Op::U8, "u8"},       {Op::I8, "i8"},       {Op::U16BE, "u16be"}, {Op::U16LE, "u16le"},
    {Op::I16BE, "i16be"}, {Op::I16LE, "i16le"}, {Op::U24BE, "u24be"}, {Op::U32BE, "u32be"},
    {Op::Bit, "bit"},     {Op::Bits, "bits"},   {Op::Mask, "mask"},   {Op::Eq, "eq"},
    {Op::Const, "const"}, {Op::Bytes, "bytes"}, {Op::Ascii, "ascii"}, {Op::Len, "len"},
};

/* How many bytes starting at `at` the operation needs. */
size_t width(const Extract &e) {
  switch (e.op) {
    case Op::U8: case Op::I8: case Op::Bit: case Op::Bits: case Op::Mask: case Op::Eq:
      return 1;
    case Op::U16BE: case Op::U16LE: case Op::I16BE: case Op::I16LE:
      return 2;
    case Op::U24BE:
      return 3;
    case Op::U32BE:
      return 4;
    case Op::Bytes: case Op::Ascii:
      return e.len;
    case Op::Const: case Op::Len:
      return 0;
  }
  return 0;
}

}  // namespace

const char *to_string(Op op) {
  for (const auto &n : kNames)
    if (n.op == op)
      return n.name;
  return "?";
}

std::optional<Op> op_from_string(const char *name) {
  if (!name)
    return std::nullopt;
  for (const auto &n : kNames)
    if (std::strcmp(n.name, name) == 0)
      return n.op;
  return std::nullopt;
}

std::optional<Value> Extract::eval(std::span<const uint8_t> p) const {
  const size_t need = width(*this);
  if (need > 0 && (static_cast<size_t>(at) + need) > p.size())
    return std::nullopt;

  const uint8_t *b = p.data() + at;

  switch (op) {
    case Op::U8:    return Value{static_cast<double>(b[0])};
    case Op::I8:    return Value{static_cast<double>(static_cast<int8_t>(b[0]))};
    case Op::U16BE: return Value{static_cast<double>((b[0] << 8) | b[1])};
    case Op::U16LE: return Value{static_cast<double>((b[1] << 8) | b[0])};
    case Op::I16BE: return Value{static_cast<double>(static_cast<int16_t>((b[0] << 8) | b[1]))};
    case Op::I16LE: return Value{static_cast<double>(static_cast<int16_t>((b[1] << 8) | b[0]))};
    case Op::U24BE: return Value{static_cast<double>((b[0] << 16) | (b[1] << 8) | b[2])};
    case Op::U32BE:
      return Value{static_cast<double>((static_cast<uint32_t>(b[0]) << 24) | (b[1] << 16) | (b[2] << 8) | b[3])};

    case Op::Bit: {
      const bool set = ((b[0] >> bit) & 1) != 0;
      return Value{invert ? !set : set};
    }
    case Op::Bits:
      return Value{static_cast<double>((b[0] >> shift) & mask)};
    case Op::Mask: {
      const bool set = (b[0] & mask) != 0;
      return Value{invert ? !set : set};
    }
    case Op::Eq: {
      const bool hit = b[0] == static_cast<uint8_t>(value);
      return Value{invert ? !hit : hit};
    }

    case Op::Const: return Value{value};
    case Op::Len:   return Value{static_cast<double>(p.size())};

    case Op::Bytes: {
      std::string out;
      out.reserve(len * 2);
      char buf[3];
      for (uint8_t i = 0; i < len; i++) {
        snprintf(buf, sizeof(buf), "%02X", b[i]);
        out += buf;
      }
      return Value{out};
    }
    case Op::Ascii: {
      std::string out;
      out.reserve(len);
      for (uint8_t i = 0; i < len; i++) {
        /* Appliance strings are padded and terminated inconsistently; stop at
           the first byte that is not printable rather than emit control
           characters into MQTT and the UI. */
        if (b[i] < 0x20 || b[i] > 0x7E)
          break;
        out += static_cast<char>(b[i]);
      }
      return Value{out};
    }
  }
  return std::nullopt;
}

}  // namespace dbus2::decoder
