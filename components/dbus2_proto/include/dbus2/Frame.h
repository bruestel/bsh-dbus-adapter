/*
   A D-Bus frame.

   Wire format:  LL DS CC CC <payload> RR RR
     LL  length, counts the data bytes only (frame total = LL + 4)
     DS  destination node (high nibble) and subsystem (low nibble)
     CC  command, the first two data bytes -- present on command frames
     RR  CRC16-XMODEM over everything before it

   (C) 2026 Jonas Brüstel
   Derived from Open-DBus2, (C) 2024-2026 Hajo Noerenberg
   https://github.com/hn/bsh-home-appliances

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License version 3.0 as
   published by the Free Software Foundation.
*/

#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace dbus2 {

struct Frame {
  uint8_t length = 0;
  uint8_t dest = 0;
  std::vector<uint8_t> data;

  uint8_t node_id() const { return (dest >> 4) & 0x0F; }
  uint8_t subsystem_id() const { return dest & 0x0F; }
  bool is_broadcast() const { return node_id() == 0; }

  /* Command frames carry the command in the first two data bytes. Frames
     shorter than that exist on the bus, so callers must check has_command()
     before trusting command(). */
  bool has_command() const { return data.size() >= 2; }
  uint16_t command() const { return has_command() ? static_cast<uint16_t>((data[0] << 8) | data[1]) : 0; }

  std::span<const uint8_t> payload() const {
    return has_command() ? std::span<const uint8_t>(data).subspan(2) : std::span<const uint8_t>{};
  }

  void clear() {
    length = 0;
    dest = 0;
    data.clear();
  }
};

}  // namespace dbus2
