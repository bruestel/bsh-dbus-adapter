/*
   Pulling a value out of a frame payload.

   These operations are the direct replacement for the lambdas in the ESPHome
   configurations. `return x[0];` becomes {u8, at 0}; `(x[1] << 8) | x[2]` cast
   to int16_t becomes {i16be, at 1}. Expressing them as data rather than code is
   what lets a new appliance be a file instead of a firmware build.

   Every operation is bounds-checked. The lambdas were not, and index blindly
   into the payload -- a short frame reads past the end.

   (C) 2026 Jonas Brüstel
   Licensed under the GNU General Public License version 3.0.
*/

#pragma once

#include "dbus2/decoder/Value.h"

#include <cstdint>
#include <optional>
#include <span>

namespace dbus2::decoder {

enum class Op : uint8_t {
  U8, I8,
  U16BE, U16LE, I16BE, I16LE,
  U24BE, U32BE,
  Bit,     // single bit, optionally inverted
  Bits,    // shift + mask
  Mask,    // byte & mask, truthy
  Eq,      // byte == value
  Const,   // fixed value, for momentary events
  Bytes,   // raw run as uppercase hex
  Ascii,   // raw run as text
  Len,     // payload length
};

struct Extract {
  Op op = Op::U8;
  uint8_t at = 0;       // byte offset into the payload
  uint8_t bit = 0;      // Bit
  uint8_t shift = 0;    // Bits
  uint32_t mask = 0xFF; // Bits, Mask
  double value = 0;     // Eq, Const
  uint8_t len = 1;      // Bytes, Ascii
  bool invert = false;  // Bit

  /* Returns nothing if the payload is too short for this operation, which the
     caller counts rather than guessing a value. */
  std::optional<Value> eval(std::span<const uint8_t> payload) const;
};

const char *to_string(Op op);
std::optional<Op> op_from_string(const char *name);

}  // namespace dbus2::decoder
