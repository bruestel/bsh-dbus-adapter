/*
   D-Bus receive state machine -- pure logic, no hardware.

   This is deliberately free of UART, FreeRTOS and ESP-IDF dependencies so it can
   be compiled and tested on a host with sanitizers. The caller feeds bytes and
   supplies the clock; the parser hands back complete, CRC-checked frames.

   READ-ONLY BY CONSTRUCTION. The upstream implementation interleaved
   transmission into this state machine, comparing every sent byte against its
   loopback echo for collision detection and generating ACKs. None of that exists
   here: this firmware never drives the bus. ACK bytes emitted by *other*
   participants are still recognised and consumed, because the parser has to
   account for them to stay in sync -- observed, never produced.

   (C) 2026 Jonas Brüstel
   Derived from Open-DBus2, (C) 2024-2026 Hajo Noerenberg
   https://github.com/hn/bsh-home-appliances

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License version 3.0 as
   published by the Free Software Foundation.
*/

#pragma once

#include "dbus2/Frame.h"

#include <cstdint>
#include <functional>

namespace dbus2 {

enum class BusState : uint8_t {
  WaitForLen,
  WaitForDest,
  WaitForData,
  WaitForCrc,
  WaitForAck,
  SkipExcess,
};

const char *to_string(BusState s);

struct Stats {
  uint32_t frames_ok = 0;
  uint32_t crc_errors = 0;
  uint32_t zero_length = 0;
  uint32_t resyncs = 0;
  uint32_t acks_seen = 0;
  uint32_t dropped = 0;  // handler could not accept the frame (queue full)
  uint32_t bytes = 0;
  /* Acknowledgements whose low nibble was not 0x0A: the addressed node received
     the frame but refused it. Counted separately because a refusal means
     something quite different from silence -- somebody was listening and said
     no. */
  uint32_t naks_seen = 0;
};

class Protocol {
 public:
  /* Returns false if the frame could not be accepted (e.g. queue full), which
     is counted as a drop. */
  using FrameHandler = std::function<bool(const Frame &, int64_t rx_time_us)>;

  /* The single byte the addressed node answered the previous frame with.
     Reported separately rather than attached to the frame, because the frame is
     already on its way by the time the acknowledgement arrives -- delaying it
     would put the decoder behind the bus for the sake of a display detail. The
     frame's own receive time is passed along so the two can be paired again
     without guessing. */
  using AckHandler = std::function<void(uint8_t ack, int64_t frame_rx_time_us)>;

  explicit Protocol(uint32_t baud = 9600) { set_baud(baud); }

  void set_frame_handler(FrameHandler handler) { handler_ = std::move(handler); }
  void set_ack_handler(AckHandler handler) { ack_handler_ = std::move(handler); }

  /* Recomputes the timing derived from the bit rate. One byte is 10 bits in
     8N1, and the protocol resyncs after 2.5 byte times of silence. */
  void set_baud(uint32_t baud);
  uint32_t baud() const { return baud_; }
  int64_t byte_time_us() const { return byte_time_us_; }
  int64_t resync_timeout_us() const { return resync_timeout_us_; }

  /* Feed one received byte. now_us should be sampled right after the byte was
     read, so it approximates the end of that byte on the wire. */
  void feed(uint8_t byte, int64_t now_us);

  /* Call when no byte arrived, to let the inter-frame gap resync the parser.
     Cheap enough to call on every poll iteration. */
  void tick(int64_t now_us);

  void reset();

  BusState state() const { return state_; }
  const Stats &stats() const { return stats_; }
  void reset_stats() { stats_ = {}; }

 private:
  FrameHandler handler_;
  AckHandler ack_handler_;
  int64_t last_frame_rx_us_ = 0;

  uint32_t baud_ = 0;
  int64_t byte_time_us_ = 0;
  int64_t resync_timeout_us_ = 0;

  BusState state_ = BusState::WaitForLen;
  Frame frame_;
  uint16_t crc_ = 0;
  bool crc_high_seen_ = false;
  int64_t last_activity_us_ = 0;
  Stats stats_;
};

}  // namespace dbus2
