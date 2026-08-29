/*
   D-Bus receive state machine.

   (C) 2026 Jonas Brüstel
   Derived from Open-DBus2, (C) 2024-2026 Hajo Noerenberg
   https://github.com/hn/bsh-home-appliances

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License version 3.0 as
   published by the Free Software Foundation.
*/

#include "dbus2/Protocol.h"
#include "dbus2/Crc.h"

namespace dbus2 {

const char *to_string(BusState s) {
  switch (s) {
    case BusState::WaitForLen: return "WaitForLen";
    case BusState::WaitForDest: return "WaitForDest";
    case BusState::WaitForData: return "WaitForData";
    case BusState::WaitForCrc: return "WaitForCrc";
    case BusState::WaitForAck: return "WaitForAck";
    case BusState::SkipExcess: return "SkipExcess";
  }
  return "?";
}

void Protocol::set_baud(uint32_t baud) {
  baud_ = baud;
  /* 8N1 is 10 bits on the wire per byte. */
  byte_time_us_ = baud ? (10 * 1000000 / baud) : 0;
  resync_timeout_us_ = byte_time_us_ * 5 / 2;
}

void Protocol::reset() {
  state_ = BusState::WaitForLen;
  frame_.clear();
  crc_ = 0;
  crc_high_seen_ = false;
}

void Protocol::tick(int64_t now_us) {
  if (state_ == BusState::WaitForLen)
    return;
  if ((now_us - last_activity_us_) <= resync_timeout_us_)
    return;

  /* Silence longer than the inter-frame gap: whatever we were in the middle of
     is unrecoverable, so drop it and wait for the next length byte. Leaving
     SkipExcess this way is the normal, expected path, not an error. */
  if (state_ != BusState::SkipExcess)
    stats_.resyncs++;
  reset();
}

void Protocol::feed(uint8_t b, int64_t now_us) {
  last_activity_us_ = now_us;
  stats_.bytes++;

  switch (state_) {
    case BusState::WaitForLen:
      if (b == 0) {
        /* A length of zero is not a frame. Stay put and wait for a plausible
           start rather than consuming the next byte as a destination. */
        stats_.zero_length++;
        return;
      }
      frame_.clear();
      frame_.length = b;
      crc_ = crc16_update(0, b);
      crc_high_seen_ = false;
      state_ = BusState::WaitForDest;
      return;

    case BusState::WaitForDest:
      frame_.dest = b;
      crc_ = crc16_update(crc_, b);
      state_ = BusState::WaitForData;
      return;

    case BusState::WaitForData:
      frame_.data.push_back(b);
      crc_ = crc16_update(crc_, b);
      if (frame_.data.size() >= frame_.length)
        state_ = BusState::WaitForCrc;
      return;

    case BusState::WaitForCrc:
      if (!crc_high_seen_) {
        if (b != (crc_ >> 8)) {
          stats_.crc_errors++;
          state_ = BusState::SkipExcess;
          return;
        }
        crc_high_seen_ = true;
        return;
      }
      if (b != (crc_ & 0xFF)) {
        stats_.crc_errors++;
        state_ = BusState::SkipExcess;
        return;
      }

      stats_.frames_ok++;
      last_frame_rx_us_ = now_us;
      if (handler_ && !handler_(frame_, now_us))
        stats_.dropped++;

      /* The addressed node answers every frame with a single ACK byte. We do
         not produce one -- we only need to swallow it so it is not mistaken for
         the next frame's length byte. */
      state_ = BusState::WaitForAck;
      return;

    case BusState::WaitForAck:
      /* Low nibble 0xA means accepted, 0x3 buffer too small, 0x7 bad CRC.
         Counted apart, because a refusal is evidence of a listener that
         disagreed -- quite different from a frame nobody answered at all. */
      if ((b & 0x0F) == 0x0A)
        stats_.acks_seen++;
      else
        stats_.naks_seen++;
      if (ack_handler_)
        ack_handler_(b, last_frame_rx_us_);
      state_ = BusState::SkipExcess;
      return;

    case BusState::SkipExcess:
      /* Anything else on the wire before the next gap is not ours to parse. */
      return;
  }
}

}  // namespace dbus2
