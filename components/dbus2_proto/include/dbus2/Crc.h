/*
   CRC16-XMODEM over D-Bus frames.

   Polynomial 0x1021, init 0x0000, non-reflected, no final XOR. Running the CRC
   over a whole frame including its two trailing checksum bytes yields 0.

   (C) 2026 Jonas Brüstel
   Derived from Open-DBus2, (C) 2024-2026 Hajo Noerenberg
   https://github.com/hn/bsh-home-appliances

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License version 3.0 as
   published by the Free Software Foundation.
*/

#pragma once

#include <cstddef>
#include <cstdint>

namespace dbus2 {

constexpr uint16_t crc16_update(uint16_t crc, uint8_t data) {
  crc ^= static_cast<uint16_t>(data) << 8;
  for (int i = 0; i < 8; i++)
    crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021) : static_cast<uint16_t>(crc << 1);
  return crc;
}

constexpr uint16_t crc16(const uint8_t *data, size_t len, uint16_t crc = 0) {
  for (size_t i = 0; i < len; i++)
    crc = crc16_update(crc, data[i]);
  return crc;
}

}  // namespace dbus2
